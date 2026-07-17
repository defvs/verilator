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
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

struct FaultTarget final {
    const AstVar* m_varp;
    std::string m_name;
    std::string m_source;
    int m_width;
    std::vector<uint32_t> m_indices;
};

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

    void collectUnpackedTargets(const AstVar* varp,
                                const std::vector<const AstUnpackArrayDType*>& unpacked,
                                size_t dim, std::vector<uint32_t>& indices,
                                std::string name) {
        if (dim == unpacked.size()) {
            m_targets.push_back({varp, name, varp->fileline()->filename(), varp->width(), indices});
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
                m_targets.push_back({varp, name, varp->fileline()->filename(), varp->width(), {}});
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
            puts(", \"reg\", " + cvtToStr(target.m_width) + "U, ");
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
        of.put("top_module", AstNode::prettyName(v3Global.opt.topModule()));
        of.put("fault_root", v3Global.opt.faultRoot());
        of.begin("targets", '[');
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            of.begin();
            of.put("id", static_cast<int>(id));
            of.put("name", target.m_name);
            of.put("kind", "reg");
            of.put("width", target.m_width);
            of.put("source", target.m_source);
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
        }
    }
};

}  // namespace

void V3EmitC::prepareFaultApi() {
    UINFO(2, __FUNCTION__ << ":");

    class ModuleContentsVisitor final : public VNVisitorConst {
        bool m_mark;

        void visit(AstVar* nodep) override {
            if (!m_mark || !nodep->isSignal() || nodep->isIO()
                || nodep->declDirection().isAny() || nodep->isConst()) {
                return;
            }
            std::vector<const AstUnpackArrayDType*> unpacked;
            if (isInjectableLeaf(leafDTypep(nodep->dtypep(), unpacked))) {
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
        if (matchp) modulep = matchp->modp();
    }
    if (!modulep) {
        v3error("--fault-root '" + root + "' could not be resolved for force instrumentation");
        return;
    }

    std::vector<AstNodeModule*> pending{modulep};
    std::unordered_set<AstNodeModule*> visited;
    v3Global.setHasForceableSignals();
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
