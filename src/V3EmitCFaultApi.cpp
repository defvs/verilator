// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit experimental fault-injection API
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitC.h"
#include "V3EmitCBase.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

enum class SiteKind : uint8_t {
    UNKNOWN = 0,
    SEQUENTIAL = 1,
    LATCH = 2,
    COMBINATIONAL = 3,
};

struct SiteSemantics final {
    uint8_t m_writerKinds = 0;
    bool m_sawUnknownWriter = false;
};

std::unordered_map<std::string, SiteSemantics> s_siteSemantics;

static std::string declarationKey(const AstVar* varp) {
    const FileLine* const flp = varp->fileline();
    return flp->filename() + ":" + cvtToStr(flp->lineno()) + ":"
           + cvtToStr(flp->firstColumn()) + ":" + AstNode::prettyName(varp->origName());
}

struct AccountingSite final {
    std::string m_path;
    std::string m_declarationKey;
    std::string m_source;
    std::string m_objectKind;
    std::string m_earlyDisposition;
    std::string m_earlyReason;
    int m_line = 0;
    int m_column = 0;
    int m_width = 0;
    std::vector<std::pair<int, int>> m_dimensions;
    SiteKind m_siteKind = SiteKind::UNKNOWN;
    std::string m_uncertainReason;
};

std::vector<AccountingSite> s_accountingSites;

static const char* siteKindName(SiteKind kind) {
    switch (kind) {
    case SiteKind::SEQUENTIAL: return "sequential";
    case SiteKind::LATCH: return "latch";
    case SiteKind::COMBINATIONAL: return "combinational";
    case SiteKind::UNKNOWN: return "unknown";
    }
    return "unknown";
}

static uint8_t siteKindBit(SiteKind kind) {
    return static_cast<uint8_t>(1U << static_cast<uint8_t>(kind));
}

struct FaultTarget final {
    const AstVar* m_varp;
    std::string m_name;
    std::string m_source;
    int m_width;
    std::vector<uint32_t> m_indices;
    std::vector<std::pair<int, int>> m_dimensions;
    SiteKind m_siteKind;
    std::string m_uncertainReason;
};

static const char* targetSiteKindName(const FaultTarget& target) {
    if (!target.m_indices.empty()
        && (target.m_siteKind == SiteKind::SEQUENTIAL || target.m_siteKind == SiteKind::LATCH)) {
        return "memory_element";
    }
    return siteKindName(target.m_siteKind);
}

static bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

static const AstNodeDType*
leafDTypep(const AstNodeDType* dtypep, std::vector<const AstUnpackArrayDType*>& unpacked) {
    while (dtypep) {
        dtypep = dtypep->skipRefp();
        if (const AstUnpackArrayDType* const unpackp = VN_CAST(dtypep, UnpackArrayDType)) {
            unpacked.push_back(unpackp);
            dtypep = unpackp->subDTypep();
        } else {
            return dtypep;
        }
    }
    return nullptr;
}

static bool isInjectableLeaf(const AstNodeDType* leafp) {
    const AstBasicDType* const basicp = leafp ? leafp->basicp() : nullptr;
    return basicp && basicp->isBitLogic() && !leafp->isCompound();
}

static std::vector<std::string> supportedModels(SiteKind kind) {
    std::vector<std::string> models;
    if (kind == SiteKind::SEQUENTIAL || kind == SiteKind::LATCH) models.push_back("seu");
    if (kind != SiteKind::UNKNOWN) {
        models.push_back("stuck_at_0");
        models.push_back("stuck_at_1");
    }
    return models;
}

static bool isFaultTarget(const AstVar* varp) {
    if (!varp || !varp->isSignal() || varp->isIO() || varp->declDirection().isAny()
        || varp->isConst()) {
        return false;
    }
    const std::string name = AstNode::prettyName(varp->name());
    const std::string prefix = v3Global.opt.faultRoot() + ".";
    if (!startsWith(name, prefix) || name.find("__Vforce") != std::string::npos) return false;
    std::vector<const AstUnpackArrayDType*> unpacked;
    return isInjectableLeaf(leafDTypep(varp->dtypep(), unpacked));
}

class EmitCFaultApi final : public EmitCBaseVisitorConst {
    AstNodeModule* const m_topModulep;
    const std::string m_apiBase = v3Global.opt.prefix() + "__fault_api";
    const std::string m_apiClass = v3Global.opt.prefix() + "FaultApi";
    std::vector<FaultTarget> m_targets;

    void visit(AstNode*) override {}

    FaultTarget makeTarget(const AstVar* varp, const std::string& name,
                           const std::vector<uint32_t>& indices,
                           const std::vector<const AstUnpackArrayDType*>& unpacked) const {
        std::vector<std::pair<int, int>> dimensions;
        for (const AstUnpackArrayDType* const unpackp : unpacked) {
            dimensions.emplace_back(unpackp->lo(), unpackp->hi());
        }

        SiteKind kind = SiteKind::UNKNOWN;
        std::string uncertainReason = "no elaborated write context was found";
        const auto semanticsIt = s_siteSemantics.find(declarationKey(varp));
        if (semanticsIt != s_siteSemantics.end()) {
            const uint8_t kinds = semanticsIt->second.m_writerKinds;
            if (!semanticsIt->second.m_sawUnknownWriter && kinds
                && !(kinds & (kinds - 1U))) {
                if (kinds == siteKindBit(SiteKind::SEQUENTIAL)) {
                    kind = SiteKind::SEQUENTIAL;
                } else if (kinds == siteKindBit(SiteKind::LATCH)) {
                    kind = SiteKind::LATCH;
                } else if (kinds == siteKindBit(SiteKind::COMBINATIONAL)) {
                    kind = SiteKind::COMBINATIONAL;
                }
                uncertainReason.clear();
            } else {
                uncertainReason
                    = "conflicting or semantically ambiguous elaborated write contexts";
            }
        }
        return {varp, name, varp->fileline()->filename(), varp->width(), indices, dimensions,
                kind, uncertainReason};
    }

    void collectUnpackedTargets(const AstVar* varp,
                                const std::vector<const AstUnpackArrayDType*>& unpacked,
                                size_t dim, std::vector<uint32_t>& indices,
                                std::string name) {
        if (dim == unpacked.size()) {
            m_targets.push_back(makeTarget(varp, name, indices, unpacked));
            return;
        }

        const AstUnpackArrayDType* const unpackp = unpacked[dim];
        for (int offset = 0; offset < unpackp->elementsConst(); ++offset) {
            const int sourceIndex = unpackp->lo() + offset;
            indices.push_back(static_cast<uint32_t>(offset));
            collectUnpackedTargets(varp, unpacked, dim + 1, indices,
                                   name + "[" + cvtToStr(sourceIndex) + "]");
            indices.pop_back();
        }
    }

    void collectTargets() {
        const std::string prefix = v3Global.opt.faultRoot() + ".";
        for (const AstNode* nodep = m_topModulep->stmtsp(); nodep; nodep = nodep->nextp()) {
            const AstVar* const varp = VN_CAST(nodep, Var);
            if (!isFaultTarget(varp)) continue;
            std::vector<const AstUnpackArrayDType*> unpacked;
            const AstNodeDType* const leafp = leafDTypep(varp->dtypep(), unpacked);
            UASSERT_OBJ(isInjectableLeaf(leafp), varp, "Fault target became non-injectable");

            const std::string name = AstNode::prettyName(varp->name());
            UASSERT_OBJ(startsWith(name, prefix), varp, "Fault target escaped fault root");
            if (unpacked.empty()) {
                m_targets.push_back(makeTarget(varp, name, {}, unpacked));
            } else {
                std::vector<uint32_t> indices;
                collectUnpackedTargets(varp, unpacked, 0, indices, name);
            }
        }
        std::stable_sort(m_targets.begin(), m_targets.end(),
                         [](const FaultTarget& lhs, const FaultTarget& rhs) {
                             return lhs.m_name < rhs.m_name;
                         });
        if (m_targets.empty()) {
            v3error("--fault-root '" + v3Global.opt.faultRoot()
                    + "' did not match any injectable packed or unpacked RTL signals");
        }
    }

    void emitHeader() {
        openNewOutputHeaderFile(m_apiBase, "Experimental fault-injection API");
        ofp()->putsGuard();
        puts("\n#include <cstdint>\n");
        puts("#include <string>\n");
        puts("#include <vector>\n\n");
        puts("class " + v3Global.opt.prefix() + ";\n\n");
        puts("class " + m_apiClass + " final {\n");
        puts("public:\n");
        puts("    using TargetId = std::uint32_t;\n");
        puts("    static constexpr TargetId invalidTarget = UINT32_MAX;\n\n");
        puts("    struct Target final {\n");
        puts("        TargetId id;\n");
        puts("        const char* name;\n");
        puts("        const char* kind;\n");
        puts("        const char* site_kind;\n");
        puts("        const char* object_kind;\n");
        puts("        std::uint32_t width;\n");
        puts("        const char* source;\n");
        puts("    };\n\n");
        puts("    explicit " + m_apiClass + "(" + v3Global.opt.prefix() + "* modelp);\n");
        puts("    static const std::vector<Target>& targets();\n");
        puts("    TargetId lookup(const std::string& name) const;\n");
        puts("    bool set(TargetId id, std::uint32_t bit, bool value);\n");
        puts("    bool flip(TargetId id, std::uint32_t bit);\n");
        if (v3Global.opt.faultForce()) {
            puts("    bool force(TargetId id, std::uint32_t bit, bool value);\n");
            puts("    bool release(TargetId id, std::uint32_t bit);\n");
        }
        puts("\n");
        puts("private:\n");
        puts("    " + v3Global.opt.prefix() + "* const m_modelp;\n");
        puts("};\n");
        ofp()->putsEndGuard();
        closeOutputFile();
    }

    void emitSource() {
        openNewOutputSourceFile(m_apiBase, false, false, "Experimental fault-injection API");
        puts("#include \"" + m_apiBase + ".h\"\n");
        puts("#include \"" + v3Global.opt.prefix() + ".h\"\n");
        puts("#include \"" + EmitCUtil::prefixNameProtect(m_topModulep) + ".h\"\n\n");

        puts(m_apiClass + "::" + m_apiClass + "(" + v3Global.opt.prefix() + "* modelp)\n");
        puts("    : m_modelp{modelp} {}\n\n");

        puts("const std::vector<" + m_apiClass + "::Target>& " + m_apiClass + "::targets() {\n");
        puts("    static const std::vector<Target> targets{\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            puts("        {" + cvtToStr(id) + "U, ");
            putsQuoted(target.m_name);
            puts(", ");
            putsQuoted(targetSiteKindName(target));
            puts(", ");
            putsQuoted(targetSiteKindName(target));
            puts(", ");
            putsQuoted(target.m_indices.empty()
                           ? (target.m_width == 1 ? "scalar" : "packed_vector")
                           : "unpacked_array_element");
            puts(", " + cvtToStr(target.m_width) + "U, ");
            putsQuoted(target.m_source);
            puts("},\n");
        }
        puts("    };\n");
        puts("    return targets;\n");
        puts("}\n\n");

        puts(m_apiClass + "::TargetId " + m_apiClass
             + "::lookup(const std::string& name) const {\n");
        puts("    for (const Target& target : targets()) {\n");
        puts("        if (name == target.name) return target.id;\n");
        puts("    }\n");
        puts("    return invalidTarget;\n");
        puts("}\n\n");

        puts("bool " + m_apiClass + "::set(TargetId id, std::uint32_t bit, bool value) {\n");
        puts("    if (!m_modelp) return false;\n");
        puts("    switch (id) {\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            std::string member = "m_modelp->rootp->" + target.m_varp->nameProtect();
            for (const uint32_t index : target.m_indices) member += "[" + cvtToStr(index) + "U]";
            puts("    case " + cvtToStr(id) + "U:\n");
            puts("        if (bit >= " + cvtToStr(target.m_width) + "U) return false;\n");
            if (target.m_width <= 32) {
                puts("        if (value) " + member + " |= (1U << bit);\n");
                puts("        else " + member + " &= ~(1U << bit);\n");
            } else if (target.m_width <= 64) {
                puts("        if (value) " + member + " |= (1ULL << bit);\n");
                puts("        else " + member + " &= ~(1ULL << bit);\n");
            } else {
                puts("        if (value) " + member + "[bit / 32U] |= (1U << (bit % 32U));\n");
                puts("        else " + member + "[bit / 32U] &= ~(1U << (bit % 32U));\n");
            }
            puts("        return true;\n");
        }
        puts("    default: return false;\n");
        puts("    }\n");
        puts("}\n");

        puts("bool " + m_apiClass + "::flip(TargetId id, std::uint32_t bit) {\n");
        puts("    if (!m_modelp) return false;\n");
        puts("    switch (id) {\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            std::string member = "m_modelp->rootp->" + target.m_varp->nameProtect();
            for (const uint32_t index : target.m_indices) member += "[" + cvtToStr(index) + "U]";
            puts("    case " + cvtToStr(id) + "U:\n");
            puts("        if (bit >= " + cvtToStr(target.m_width) + "U) return false;\n");
            if (target.m_width <= 32) {
                puts("        " + member + " ^= (1U << bit);\n");
            } else if (target.m_width <= 64) {
                puts("        " + member + " ^= (1ULL << bit);\n");
            } else {
                puts("        " + member + "[bit / 32U] ^= (1U << (bit % 32U));\n");
            }
            puts("        return true;\n");
        }
        puts("    default: return false;\n");
        puts("    }\n");
        puts("}\n");

        if (v3Global.opt.faultForce()) {
            emitForceMethod(false);
            emitForceMethod(true);
        }
        closeOutputFile();
    }

    void emitForceMethod(bool release) {
        puts("\nbool " + m_apiClass + "::" + (release ? "release" : "force")
             + "(TargetId id, std::uint32_t bit"
             + (release ? "" : ", bool value") + ") {\n");
        puts("    if (!m_modelp) return false;\n");
        puts("    switch (id) {\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            std::string enable = "m_modelp->rootp->" + target.m_varp->nameProtect()
                                 + "__VforceEn";
            std::string value = "m_modelp->rootp->" + target.m_varp->nameProtect()
                                + "__VforceVal";
            for (const uint32_t index : target.m_indices) {
                const std::string select = "[" + cvtToStr(index) + "U]";
                enable += select;
                value += select;
            }
            puts("    case " + cvtToStr(id) + "U:\n");
            puts("        if (bit >= " + cvtToStr(target.m_width) + "U) return false;\n");
            if (target.m_width <= 32) {
                const std::string mask = "(1U << bit)";
                if (!release) {
                    puts("        if (value) " + value + " |= " + mask + ";\n");
                    puts("        else " + value + " &= ~" + mask + ";\n");
                }
                puts("        " + enable + (release ? " &= ~" : " |= ") + mask + ";\n");
            } else if (target.m_width <= 64) {
                const std::string mask = "(1ULL << bit)";
                if (!release) {
                    puts("        if (value) " + value + " |= " + mask + ";\n");
                    puts("        else " + value + " &= ~" + mask + ";\n");
                }
                puts("        " + enable + (release ? " &= ~" : " |= ") + mask + ";\n");
            } else {
                const std::string word = "[bit / 32U]";
                const std::string mask = "(1U << (bit % 32U))";
                if (!release) {
                    puts("        if (value) " + value + word + " |= " + mask + ";\n");
                    puts("        else " + value + word + " &= ~" + mask + ";\n");
                }
                puts("        " + enable + word + (release ? " &= ~" : " |= ") + mask + ";\n");
            }
            puts("        return true;\n");
        }
        puts("    default: return false;\n");
        puts("    }\n");
        puts("}\n");
    }

    void emitManifest() const {
        V3OutJsonFile of{v3Global.opt.makeDir() + "/fault_targets.json"};
        of.put("schema_version", 2);
        of.put("manifest_type", "vfi_fault_targets");
        of.put("top_module", AstNode::prettyName(v3Global.opt.topModule()));
        of.put("fault_root", v3Global.opt.faultRoot());
        of.begin("targets", '[');
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            of.begin();
            of.put("id", static_cast<int>(id));
            of.put("name", target.m_name);
            of.put("path", target.m_name);
            of.put("kind", targetSiteKindName(target));
            of.put("site_kind", targetSiteKindName(target));
            of.put("object_kind", target.m_indices.empty()
                                      ? (target.m_width == 1 ? "scalar" : "packed_vector")
                                      : "unpacked_array_element");
            of.put("width", target.m_width);
            of.put("source", target.m_source);
            of.put("declaration_line", target.m_varp->fileline()->lineno());
            of.put("declaration_column", target.m_varp->fileline()->firstColumn());
            of.begin("dimensions", '[');
            for (const auto& dimension : target.m_dimensions) {
                of.begin();
                of.put("kind", "unpacked");
                of.put("left", dimension.first);
                of.put("right", dimension.second);
                of.put("size",
                       dimension.first >= dimension.second ? dimension.first - dimension.second + 1
                                                           : dimension.second - dimension.first + 1);
                of.end();
            }
            of.end();
            const std::vector<std::string> models = supportedModels(target.m_siteKind);
            of.begin("supported_fault_models", '[');
            for (const std::string& model : models) of.put(model);
            of.end();
            if (!target.m_uncertainReason.empty()) {
                of.put("uncertain_reason", target.m_uncertainReason);
            }
            of.end();
        }
        of.end();
    }

    struct AccountingResult final {
        AccountingSite m_site;
        std::string m_disposition;
        std::string m_reason;
        int m_targetId = -1;
    };

    static void increment(std::map<std::string, std::pair<int, int>>& totals,
                          const std::string& key, int bits) {
        auto& total = totals[key];
        ++total.first;
        total.second += bits;
    }

    static const char* accountingSiteKindName(const AccountingSite& site) {
        if (!site.m_dimensions.empty()
            && (site.m_siteKind == SiteKind::SEQUENTIAL
                || site.m_siteKind == SiteKind::LATCH)) {
            return "memory_element";
        }
        return siteKindName(site.m_siteKind);
    }

    static void emitTotals(V3OutJsonFile& of, const std::string& name,
                           const std::string& keyName,
                           const std::map<std::string, std::pair<int, int>>& totals) {
        of.begin(name, '[');
        for (const auto& entry : totals) {
            of.begin();
            of.put(keyName, entry.first);
            of.put("objects", entry.second.first);
            of.put("bits", entry.second.second);
            of.end();
        }
        of.end();
    }

    void emitAccounting() const {
        std::unordered_map<std::string, size_t> targetByPath;
        std::unordered_set<std::string> generatedDeclarationKeys;
        for (size_t id = 0; id < m_targets.size(); ++id) {
            targetByPath.emplace(m_targets[id].m_name, id);
            generatedDeclarationKeys.insert(declarationKey(m_targets[id].m_varp));
        }

        std::vector<AccountingResult> results;
        std::unordered_set<size_t> correlatedTargets;
        results.reserve(s_accountingSites.size() + m_targets.size());
        for (const AccountingSite& site : s_accountingSites) {
            AccountingResult result{site, "", "", -1};
            const auto targetIt = targetByPath.find(site.m_path);
            if (!site.m_earlyDisposition.empty()) {
                result.m_disposition = site.m_earlyDisposition;
                result.m_reason = site.m_earlyReason;
            } else if (targetIt != targetByPath.end()) {
                const FaultTarget& target = m_targets[targetIt->second];
                result.m_targetId = static_cast<int>(targetIt->second);
                correlatedTargets.insert(targetIt->second);
                result.m_site.m_siteKind = target.m_siteKind;
                result.m_site.m_uncertainReason = target.m_uncertainReason;
                if (target.m_siteKind == SiteKind::UNKNOWN) {
                    result.m_disposition = "ambiguous_semantics";
                    result.m_reason = target.m_uncertainReason;
                } else {
                    result.m_disposition = "injectable";
                    result.m_reason = "correlated with generated fault target";
                }
            } else if (generatedDeclarationKeys.count(site.m_declarationKey)) {
                result.m_disposition = "aliased_or_collapsed";
                result.m_reason
                    = "the declaration survived under another generated target path";
            } else {
                result.m_disposition = "optimized_or_elided";
                result.m_reason = "no generated target survived optimization for this object";
            }
            results.push_back(std::move(result));
        }

        // A generated target without a pre-optimization candidate must never disappear from
        // the audit. Account it as unresolved so strict builds fail visibly.
        for (size_t id = 0; id < m_targets.size(); ++id) {
            if (correlatedTargets.count(id)) continue;
            const FaultTarget& target = m_targets[id];
            AccountingSite site;
            site.m_path = target.m_name;
            site.m_declarationKey = declarationKey(target.m_varp);
            site.m_source = target.m_source;
            site.m_objectKind
                = target.m_indices.empty()
                      ? (target.m_width == 1 ? "scalar" : "packed_vector")
                      : "unpacked_array_element";
            site.m_line = target.m_varp->fileline()->lineno();
            site.m_column = target.m_varp->fileline()->firstColumn();
            site.m_width = target.m_width;
            site.m_dimensions = target.m_dimensions;
            site.m_siteKind = target.m_siteKind;
            site.m_uncertainReason = target.m_uncertainReason;
            results.push_back({site, "internal_tool_failure/unresolved",
                               "generated target was absent from the pre-optimization inventory",
                               static_cast<int>(id)});
        }

        std::stable_sort(results.begin(), results.end(),
                         [](const AccountingResult& lhs, const AccountingResult& rhs) {
                             return lhs.m_site.m_path < rhs.m_site.m_path;
                         });

        int candidateObjects = 0;
        int candidateBits = 0;
        int injectableObjects = 0;
        int injectableBits = 0;
        int excludedObjects = 0;
        int excludedBits = 0;
        int unresolvedObjects = 0;
        int unresolvedBits = 0;
        std::map<std::string, std::pair<int, int>> byHierarchy;
        std::map<std::string, std::pair<int, int>> bySiteKind;
        std::map<std::string, std::pair<int, int>> byModel;
        std::map<std::string, std::pair<int, int>> byExclusion;
        std::map<std::string, std::pair<int, int>> byDisposition;
        for (const AccountingResult& result : results) {
            const AccountingSite& site = result.m_site;
            ++candidateObjects;
            candidateBits += site.m_width;
            if (result.m_disposition == "injectable") {
                ++injectableObjects;
                injectableBits += site.m_width;
            } else if (result.m_disposition == "internal_tool_failure/unresolved") {
                ++unresolvedObjects;
                unresolvedBits += site.m_width;
            } else {
                ++excludedObjects;
                excludedBits += site.m_width;
                increment(byExclusion, result.m_disposition, site.m_width);
            }
            const size_t separator = site.m_path.rfind('.');
            increment(byHierarchy,
                      separator == std::string::npos ? site.m_path
                                                    : site.m_path.substr(0, separator),
                      site.m_width);
            const char* const kind = accountingSiteKindName(site);
            increment(bySiteKind, kind, site.m_width);
            const std::vector<std::string> models
                = result.m_disposition == "injectable"
                      ? supportedModels(site.m_siteKind)
                      : std::vector<std::string>{};
            if (models.empty()) {
                increment(byModel, "none", site.m_width);
            } else {
                for (const std::string& model : models) increment(byModel, model, site.m_width);
            }
            increment(byDisposition, result.m_disposition, site.m_width);
        }

        V3OutJsonFile of{v3Global.opt.makeDir() + "/fault_site_accounting.json"};
        of.put("schema_version", 1);
        of.put("manifest_type", "vfi_fault_site_accounting");
        of.put("top_module", AstNode::prettyName(v3Global.opt.topModule()));
        of.put("fault_root", v3Global.opt.faultRoot());
        of.put("accounting_complete", unresolvedBits == 0);
        of.begin("summary");
        of.put("candidate_objects", candidateObjects);
        of.put("candidate_bits", candidateBits);
        of.put("injectable_objects", injectableObjects);
        of.put("injectable_bits", injectableBits);
        of.put("excluded_objects", excludedObjects);
        of.put("excluded_bits", excludedBits);
        of.put("unresolved_objects", unresolvedObjects);
        of.put("unresolved_bits", unresolvedBits);
        of.put("invariant_holds",
               candidateBits == injectableBits + excludedBits + unresolvedBits);
        of.end();
        emitTotals(of, "by_hierarchy", "hierarchy", byHierarchy);
        emitTotals(of, "by_site_kind", "site_kind", bySiteKind);
        emitTotals(of, "by_supported_fault_model", "fault_model", byModel);
        emitTotals(of, "by_exclusion_reason", "reason", byExclusion);
        emitTotals(of, "by_disposition", "disposition", byDisposition);
        of.begin("sites", '[');
        for (const AccountingResult& result : results) {
            const AccountingSite& site = result.m_site;
            of.begin();
            of.put("path", site.m_path);
            of.put("source", site.m_source);
            of.put("declaration_line", site.m_line);
            of.put("declaration_column", site.m_column);
            of.put("width", site.m_width);
            of.put("object_kind", site.m_objectKind);
            of.put("site_kind", accountingSiteKindName(site));
            of.put("disposition", result.m_disposition);
            of.put("reason", result.m_reason);
            if (result.m_targetId >= 0) of.put("generated_target_id", result.m_targetId);
            of.begin("dimensions", '[');
            for (const auto& dimension : site.m_dimensions) {
                of.begin();
                of.put("kind", "unpacked");
                of.put("left", dimension.first);
                of.put("right", dimension.second);
                of.put("size",
                       dimension.first >= dimension.second ? dimension.first - dimension.second + 1
                                                           : dimension.second - dimension.first + 1);
                of.end();
            }
            of.end();
            const std::vector<std::string> models
                = result.m_disposition == "injectable"
                      ? supportedModels(site.m_siteKind)
                      : std::vector<std::string>{};
            of.begin("supported_fault_models", '[');
            for (const std::string& model : models) of.put(model);
            of.end();
            if (!site.m_uncertainReason.empty()) {
                of.put("uncertain_reason", site.m_uncertainReason);
            }
            of.end();
        }
        of.end();
    }

public:
    explicit EmitCFaultApi(AstNodeModule* topModulep)
        : m_topModulep{topModulep} {
        collectTargets();
        if (!m_targets.empty()) {
            emitHeader();
            emitSource();
            emitManifest();
            emitAccounting();
        }
    }
};

}  // namespace

void V3EmitC::prepareFaultApi() {
    UINFO(2, __FUNCTION__ << ":");

    class SiteSemanticsVisitor final : public VNVisitorConst {
        SiteKind m_context = SiteKind::UNKNOWN;

        void iterateWithContext(AstNode* nodep, SiteKind context) {
            const SiteKind previous = m_context;
            m_context = context;
            iterateChildrenConst(nodep);
            m_context = previous;
        }

        void visit(AstAlways* nodep) override {
            SiteKind context = SiteKind::UNKNOWN;
            if (nodep->keyword() == VAlwaysKwd::ALWAYS_FF
                || (nodep->sentreep() && nodep->sentreep()->hasEdge())) {
                context = SiteKind::SEQUENTIAL;
            } else if (nodep->keyword() == VAlwaysKwd::ALWAYS_LATCH) {
                context = SiteKind::LATCH;
            } else if (nodep->keyword() == VAlwaysKwd::ALWAYS_COMB
                       || nodep->keyword() == VAlwaysKwd::CONT_ASSIGN) {
                context = SiteKind::COMBINATIONAL;
            }
            iterateWithContext(nodep, context);
        }
        void visit(AstAssignW* nodep) override {
            iterateWithContext(nodep, SiteKind::COMBINATIONAL);
        }
        void visit(AstAssignCont* nodep) override {
            iterateWithContext(nodep, SiteKind::COMBINATIONAL);
        }
        void visit(AstVarRef* nodep) override {
            if (nodep->access().isWriteOrRW()) {
                SiteSemantics& semantics = s_siteSemantics[declarationKey(nodep->varp())];
                if (m_context == SiteKind::UNKNOWN) {
                    semantics.m_sawUnknownWriter = true;
                } else {
                    semantics.m_writerKinds |= siteKindBit(m_context);
                }
            }
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        explicit SiteSemanticsVisitor(AstNetlist* nodep) { iterateConst(nodep); }
    };

    s_siteSemantics.clear();
    s_accountingSites.clear();
    SiteSemanticsVisitor semantics{v3Global.rootp()};

    class ModuleContentsVisitor final : public VNVisitorConst {
        bool m_mark;

        void visit(AstVar* nodep) override {
            if (!m_mark || !nodep->isSignal() || nodep->isIO()
                || nodep->declDirection().isAny() || nodep->isConst()) {
                return;
            }
            std::vector<const AstUnpackArrayDType*> unpacked;
            if (v3Global.opt.faultForce()
                && isInjectableLeaf(leafDTypep(nodep->dtypep(), unpacked))) {
                const_cast<AstVar*>(nodep)->setForceable();
            }
        }
        void visit(AstCell* nodep) override {
            m_cellps.push_back(nodep);
            iterateChildrenConst(nodep);
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        std::vector<AstCell*> m_cellps;
        ModuleContentsVisitor(AstNodeModule* modulep, bool mark)
            : m_mark{mark} {
            iterateConst(modulep);
        }
    };

    std::vector<std::string> path;
    const std::string root = v3Global.opt.faultRoot();
    for (size_t begin = 0; begin <= root.size();) {
        const size_t end = root.find('.', begin);
        path.push_back(root.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    AstNodeModule* modulep = v3Global.rootp()->topModulep();
    size_t component = 0;
    std::string inventoryRoot = path.empty() ? root : path[0];
    if (!path.empty()
        && (path[0] == AstNode::prettyName(modulep->name())
            || path[0] == AstNode::prettyName(v3Global.opt.topModule()))) {
        component = 1;
    }
    for (; modulep && component < path.size(); ++component) {
        ModuleContentsVisitor contents{modulep, false};
        AstCell* matchp = nullptr;
        for (AstCell* const cellp : contents.m_cellps) {
            if (path[component] == AstNode::prettyName(cellp->name())
                || path[component] == AstNode::prettyName(cellp->origName())) {
                matchp = cellp;
                break;
            }
        }
        // Named generate/block scopes are hierarchy components but not AstCells.
        // Leave the module unchanged for those components; the visitor sees cells
        // nested below such scopes on the following iteration.
        if (matchp) {
            modulep = matchp->modp();
            inventoryRoot.clear();
            for (size_t index = 0; index <= component; ++index) {
                if (index) inventoryRoot += ".";
                inventoryRoot += path[index];
            }
        }
    }
    if (!modulep) {
        v3error("--fault-root '" + root + "' could not be resolved for force instrumentation");
        return;
    }

    const auto recordModule = [](AstNodeModule* rootModulep, const std::string& rootPath) {
        class InventoryVisitor final : public VNVisitorConst {
            const std::string& m_instancePath;

            void record(const AstVar* varp) {
                if (!varp->isSignal() && !varp->isIO() && !varp->isConst()) return;
                std::vector<const AstUnpackArrayDType*> unpacked;
                const AstNodeDType* const leafp = leafDTypep(varp->dtypep(), unpacked);
                std::vector<std::pair<int, int>> dimensions;
                for (const AstUnpackArrayDType* const unpackp : unpacked) {
                    dimensions.emplace_back(unpackp->lo(), unpackp->hi());
                }
                const int width = std::max(1, leafp ? leafp->width() : varp->width());
                const std::string objectKind
                    = unpacked.empty()
                          ? (width == 1 ? "scalar" : "packed_vector")
                          : "unpacked_array_element";
                SiteKind kind = SiteKind::UNKNOWN;
                std::string uncertainReason = "no elaborated write context was found";
                const std::string key = declarationKey(varp);
                const auto semanticsIt = s_siteSemantics.find(key);
                if (semanticsIt != s_siteSemantics.end()) {
                    const uint8_t kinds = semanticsIt->second.m_writerKinds;
                    if (!semanticsIt->second.m_sawUnknownWriter && kinds
                        && !(kinds & (kinds - 1U))) {
                        if (kinds == siteKindBit(SiteKind::SEQUENTIAL)) {
                            kind = SiteKind::SEQUENTIAL;
                        } else if (kinds == siteKindBit(SiteKind::LATCH)) {
                            kind = SiteKind::LATCH;
                        } else if (kinds == siteKindBit(SiteKind::COMBINATIONAL)) {
                            kind = SiteKind::COMBINATIONAL;
                        }
                        uncertainReason.clear();
                    } else {
                        uncertainReason
                            = "conflicting or semantically ambiguous elaborated write contexts";
                    }
                }

                std::string earlyDisposition;
                std::string earlyReason;
                if (varp->isIO() || varp->declDirection().isAny() || varp->isConst()) {
                    earlyDisposition = "port_or_constant";
                    earlyReason = "ports and constants are outside the injectable internal state";
                } else if (!isInjectableLeaf(leafp)) {
                    earlyDisposition = "unsupported_datatype";
                    earlyReason = "datatype is not a packed bit/logic scalar or vector";
                }

                const std::string basePath
                    = m_instancePath + "." + AstNode::prettyName(varp->name());
                const auto add = [&](const std::string& path) {
                    s_accountingSites.push_back(
                        {path, key, varp->fileline()->filename(), objectKind,
                         earlyDisposition, earlyReason, varp->fileline()->lineno(),
                         varp->fileline()->firstColumn(), width, dimensions, kind,
                         uncertainReason});
                };
                if (unpacked.empty()) {
                    add(basePath);
                    return;
                }
                const std::function<void(size_t, std::string)> expand
                    = [&](size_t dim, std::string path) {
                          if (dim == unpacked.size()) {
                              add(path);
                              return;
                          }
                          const AstUnpackArrayDType* const unpackp = unpacked[dim];
                          for (int offset = 0; offset < unpackp->elementsConst(); ++offset) {
                              const int sourceIndex = unpackp->lo() + offset;
                              expand(dim + 1, path + "[" + cvtToStr(sourceIndex) + "]");
                          }
                      };
                expand(0, basePath);
            }

            void visit(AstVar* nodep) override { record(nodep); }
            void visit(AstNodeModule*) override {}
            void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

        public:
            InventoryVisitor(AstNodeModule* modulep, const std::string& instancePath)
                : m_instancePath{instancePath} {
                iterateChildrenConst(modulep);
            }
        };

        const std::function<void(AstNodeModule*, const std::string&,
                                 std::unordered_set<AstNodeModule*>)>
            walk = [&](AstNodeModule* currentp, const std::string& instancePath,
                       std::unordered_set<AstNodeModule*> ancestors) {
                if (!currentp || !ancestors.insert(currentp).second) return;
                InventoryVisitor inventory{currentp, instancePath};
                ModuleContentsVisitor contents{currentp, false};
                for (AstCell* const cellp : contents.m_cellps) {
                    walk(cellp->modp(),
                         instancePath + "." + AstNode::prettyName(cellp->name()), ancestors);
                }
            };
        walk(rootModulep, rootPath, {});
    };
    recordModule(modulep, inventoryRoot);
    const std::string accountingPrefix = root + ".";
    s_accountingSites.erase(
        std::remove_if(s_accountingSites.begin(), s_accountingSites.end(),
                       [&](const AccountingSite& site) {
                           return site.m_path != root
                                  && !startsWith(site.m_path, accountingPrefix);
                       }),
        s_accountingSites.end());
    std::stable_sort(s_accountingSites.begin(), s_accountingSites.end(),
                     [](const AccountingSite& lhs, const AccountingSite& rhs) {
                         return lhs.m_path < rhs.m_path;
                     });
    s_accountingSites.erase(
        std::unique(s_accountingSites.begin(), s_accountingSites.end(),
                    [](const AccountingSite& lhs, const AccountingSite& rhs) {
                        return lhs.m_path == rhs.m_path;
                    }),
        s_accountingSites.end());

    std::vector<AstNodeModule*> pending{modulep};
    std::unordered_set<AstNodeModule*> visited;
    if (v3Global.opt.faultForce()) v3Global.setHasForceableSignals();
    while (!pending.empty()) {
        AstNodeModule* const currentp = pending.back();
        pending.pop_back();
        if (!currentp || !visited.insert(currentp).second) continue;
        ModuleContentsVisitor contents{currentp, true};
        for (AstCell* const cellp : contents.m_cellps) pending.push_back(cellp->modp());
    }
}

void V3EmitC::emitcFaultApi() {
    UINFO(2, __FUNCTION__ << ":");
    EmitCFaultApi{v3Global.rootp()->topModulep()};
}
