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
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

struct FaultTarget final {
    const AstVar* m_varp;
    std::string m_name;
    std::string m_source;
    int m_width;
};

class EmitCFaultApi final : public EmitCBaseVisitorConst {
    AstNodeModule* const m_topModulep;
    const std::string m_apiBase = v3Global.opt.prefix() + "__fault_api";
    const std::string m_apiClass = v3Global.opt.prefix() + "FaultApi";
    std::vector<FaultTarget> m_targets;

    void visit(AstNode*) override {}

    static bool startsWith(const std::string& text, const std::string& prefix) {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    void collectTargets() {
        const std::string prefix = v3Global.opt.faultRoot() + ".";
        for (const AstNode* nodep = m_topModulep->stmtsp(); nodep; nodep = nodep->nextp()) {
            const AstVar* const varp = VN_CAST(nodep, Var);
            if (!varp || !varp->isSignal() || varp->isIO()
                || varp->declDirection().isAny() || varp->isConst()) {
                continue;
            }
            const AstNodeDType* const dtypep = varp->dtypep()->skipRefp();
            if (!varp->basicp() || !varp->isBitLogic() || dtypep->isCompound()
                || VN_IS(dtypep, UnpackArrayDType)) {
                continue;
            }

            const std::string name = AstNode::prettyName(varp->name());
            if (!startsWith(name, prefix)) continue;
            m_targets.push_back({varp, name, varp->fileline()->filename(), varp->width()});
        }
        std::stable_sort(m_targets.begin(), m_targets.end(),
                         [](const FaultTarget& lhs, const FaultTarget& rhs) {
                             return lhs.m_name < rhs.m_name;
                         });
        if (m_targets.empty()) {
            v3error("--fault-root '" + v3Global.opt.faultRoot()
                    + "' did not match any injectable packed RTL signals");
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
        puts("    bool flip(TargetId id, std::uint32_t bit);\n\n");
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

        puts("bool " + m_apiClass + "::flip(TargetId id, std::uint32_t bit) {\n");
        puts("    if (!m_modelp) return false;\n");
        puts("    switch (id) {\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const FaultTarget& target = m_targets[id];
            const std::string member = "m_modelp->rootp->" + target.m_varp->nameProtect();
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
        closeOutputFile();
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

void V3EmitC::emitcFaultApi() {
    UINFO(2, __FUNCTION__ << ":");
    EmitCFaultApi{v3Global.rootp()->topModulep()};
}
