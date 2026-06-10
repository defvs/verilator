// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit experimental probe/golden comparison API
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
#include "V3File.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

enum class ProbeMode : uint8_t { ALL, FIRST, STOP };

struct ProbeSpec final {
    std::string m_name;
    std::string m_path;
    std::string m_basePath;
    std::string m_signalName;
    ProbeMode m_mode = ProbeMode::ALL;
    uint32_t m_limit = 0;
    std::vector<int> m_selects;
};

struct ProbeTarget final {
    const AstVar* m_varp = nullptr;
    ProbeSpec m_spec;
    std::string m_source;
    int m_width = 0;
    int m_packedBit = -1;
    std::vector<uint32_t> m_indices;
};

static std::string trim(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

static std::string modeName(const ProbeMode mode) {
    switch (mode) {
    case ProbeMode::ALL: return "all";
    case ProbeMode::FIRST: return "first";
    case ProbeMode::STOP: return "stop";
    }
    VL_UNREACHABLE;
    return "";
}

static bool parseInt(const std::string& text, int* const valuep) {
    char* endp = nullptr;
    const long value = std::strtol(text.c_str(), &endp, 0);
    if (!endp || *endp != '\0') return false;
    *valuep = static_cast<int>(value);
    return true;
}

static bool parseUint32(const std::string& text, uint32_t* const valuep) {
    char* endp = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &endp, 0);
    if (!endp || *endp != '\0' || value > UINT32_MAX) return false;
    *valuep = static_cast<uint32_t>(value);
    return true;
}

static bool parseMode(const std::string& text, ProbeMode* const modep) {
    if (text == "all") {
        *modep = ProbeMode::ALL;
        return true;
    }
    if (text == "first") {
        *modep = ProbeMode::FIRST;
        return true;
    }
    if (text == "stop") {
        *modep = ProbeMode::STOP;
        return true;
    }
    return false;
}

static bool parseLocation(ProbeSpec* const specp, const std::string& filename,
                          const int lineno) {
    const size_t dot = specp->m_path.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= specp->m_path.size()) {
        v3error(filename + ":" + cvtToStr(lineno)
                + ": probe path must be a hierarchical signal path: " + specp->m_path);
        return false;
    }

    const std::string scope = specp->m_path.substr(0, dot);
    const std::string signal = specp->m_path.substr(dot + 1);
    const size_t firstBracket = signal.find('[');
    specp->m_signalName = firstBracket == std::string::npos ? signal : signal.substr(0, firstBracket);
    if (specp->m_signalName.empty()) {
        v3error(filename + ":" + cvtToStr(lineno) + ": probe path has an empty signal name");
        return false;
    }
    specp->m_basePath = scope + "." + specp->m_signalName;

    size_t pos = firstBracket;
    while (pos != std::string::npos) {
        if (signal[pos] != '[') {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": unsupported probe selector syntax: " + specp->m_path);
            return false;
        }
        const size_t end = signal.find(']', pos + 1);
        if (end == std::string::npos) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": unterminated probe selector: " + specp->m_path);
            return false;
        }
        const std::string indexText = signal.substr(pos + 1, end - pos - 1);
        if (indexText.find(':') != std::string::npos) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": probe part-select ranges are not supported yet: " + specp->m_path);
            return false;
        }
        int index = 0;
        if (!parseInt(indexText, &index)) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": probe selector must be a constant integer: " + specp->m_path);
            return false;
        }
        specp->m_selects.push_back(index);
        pos = end + 1;
        if (pos == signal.size()) break;
        if (signal[pos] != '[') {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": unsupported probe selector suffix: " + specp->m_path);
            return false;
        }
    }
    return true;
}

static std::vector<ProbeSpec> parseProbeConfig() {
    const std::string filename = v3Global.opt.probeConfig();
    const std::unique_ptr<std::ifstream> ifp{V3File::new_ifstream(filename)};
    if (ifp->fail()) {
        v3error("Cannot open --probe-config file: " + filename);
        return {};
    }

    std::vector<ProbeSpec> specs;
    std::string line;
    int lineno = 0;
    while (std::getline(*ifp, line)) {
        ++lineno;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;

        std::istringstream iss{line};
        std::string modeText;
        std::string limitText;
        std::string extra;
        ProbeSpec spec;
        if (!(iss >> spec.m_name >> spec.m_path >> modeText >> limitText) || (iss >> extra)) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": expected probe config fields: name path mode limit");
            continue;
        }
        if (!parseMode(modeText, &spec.m_mode)) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": probe mode must be one of all, first, stop: " + modeText);
            continue;
        }
        if (!parseUint32(limitText, &spec.m_limit) || spec.m_limit == 0) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": probe message limit must be a positive integer: " + limitText);
            continue;
        }
        if (spec.m_mode != ProbeMode::ALL && spec.m_limit != 1) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": first/stop probe modes require message limit 1");
            continue;
        }
        if (!parseLocation(&spec, filename, lineno)) continue;
        specs.push_back(spec);
    }
    if (specs.empty()) v3error("--probe-config did not define any probes: " + filename);
    return specs;
}

static const std::vector<ProbeSpec>& probeSpecs() {
    static const std::vector<ProbeSpec> specs = parseProbeConfig();
    return specs;
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

static bool isProbeLeaf(const AstNodeDType* const leafp) {
    const AstBasicDType* const basicp = leafp ? leafp->basicp() : nullptr;
    return basicp && basicp->isBitLogic() && !leafp->isCompound();
}

class PrepareProbeApi final : public VNVisitor {
    std::unordered_set<std::string> m_signalNames;
    void visit(AstVar* nodep) override {
        const std::string pretty = AstNode::prettyName(nodep->name());
        const size_t dot = pretty.rfind('.');
        const std::string leaf = dot == std::string::npos ? pretty : pretty.substr(dot + 1);
        if (m_signalNames.count(leaf) || m_signalNames.count(AstNode::prettyName(nodep->origName()))) {
            nodep->sigUserRdPublic(true);
        }
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit PrepareProbeApi(AstNetlist* rootp) {
        for (const ProbeSpec& spec : probeSpecs()) m_signalNames.insert(spec.m_signalName);
        if (!m_signalNames.empty()) iterate(rootp);
    }
};

class EmitCProbeApi final : public EmitCBaseVisitorConst {
    AstNodeModule* const m_topModulep;
    const std::string m_apiBase = v3Global.opt.prefix() + "__probe_api";
    const std::string m_apiClass = v3Global.opt.prefix() + "ProbeApi";
    std::vector<ProbeSpec> m_specs;
    std::vector<ProbeTarget> m_targets;

    void visit(AstNode*) override {}

    bool makeTarget(const ProbeSpec& spec, const AstVar* const varp, ProbeTarget* const targetp) {
        if ((!varp->isSignal() && !varp->isIO()) || varp->isConst()) {
            v3error("probe '" + spec.m_name + "' does not resolve to a signal or port: "
                    + spec.m_path);
            return false;
        }

        std::vector<const AstUnpackArrayDType*> unpacked;
        const AstNodeDType* const leafp = leafDTypep(varp->dtypep(), unpacked);
        if (!isProbeLeaf(leafp)) {
            v3error("probe '" + spec.m_name
                    + "' must resolve to a 2-state/logic scalar or packed bus: " + spec.m_path);
            return false;
        }

        if (spec.m_selects.size() < unpacked.size()) {
            v3error("probe '" + spec.m_name + "' must select every unpacked array dimension: "
                    + spec.m_path);
            return false;
        }
        if (spec.m_selects.size() > unpacked.size() + 1) {
            v3error("probe '" + spec.m_name
                    + "' has too many selectors; only one packed bit select is supported: "
                    + spec.m_path);
            return false;
        }

        targetp->m_varp = varp;
        targetp->m_spec = spec;
        targetp->m_source = varp->fileline()->filename();
        targetp->m_width = varp->width();

        for (size_t dim = 0; dim < unpacked.size(); ++dim) {
            const AstUnpackArrayDType* const unpackp = unpacked[dim];
            const int index = spec.m_selects[dim];
            if (index < unpackp->lo() || index > unpackp->hi()) {
                v3error("probe '" + spec.m_name + "' unpacked array index is out of range: "
                        + spec.m_path);
                return false;
            }
            targetp->m_indices.push_back(static_cast<uint32_t>(index - unpackp->lo()));
        }

        if (spec.m_selects.size() == unpacked.size() + 1) {
            const int userBit = spec.m_selects.back();
            const AstBasicDType* const basicp = leafp->basicp();
            const VNumRange range = basicp->declRange();
            const int lo = range.ranged() ? range.lo() : 0;
            const int hi = range.ranged() ? range.hi() : basicp->width() - 1;
            if (userBit < lo || userBit > hi) {
                v3error("probe '" + spec.m_name + "' packed bit index is out of range: "
                        + spec.m_path);
                return false;
            }
            targetp->m_packedBit = userBit - lo;
            targetp->m_width = 1;
        } else if (!unpacked.empty() && spec.m_selects.size() != unpacked.size()) {
            v3error("probe '" + spec.m_name + "' must resolve to an unpacked array element: "
                    + spec.m_path);
            return false;
        }
        return true;
    }

    void collectTargets() {
        std::vector<bool> matched(m_specs.size(), false);
        for (size_t i = 0; i < m_specs.size(); ++i) {
            const ProbeSpec& spec = m_specs[i];
            for (const AstNode* nodep = m_topModulep->stmtsp(); nodep; nodep = nodep->nextp()) {
                const AstVar* const varp = VN_CAST(nodep, Var);
                if (!varp) continue;
                const std::string name = AstNode::prettyName(varp->name());
                if (name != spec.m_basePath) continue;
                matched[i] = true;
                ProbeTarget target;
                if (makeTarget(spec, varp, &target)) m_targets.push_back(target);
                break;
            }
        }
        for (size_t i = 0; i < m_specs.size(); ++i) {
            if (!matched[i]) v3error("probe did not match a generated signal: " + m_specs[i].m_path);
        }
    }

    std::string memberExpr(const ProbeTarget& target) const {
        std::string member = "m_modelp->rootp->" + target.m_varp->nameProtect();
        for (const uint32_t index : target.m_indices) member += "[" + cvtToStr(index) + "U]";
        return member;
    }

    void emitHeader() {
        openNewOutputHeaderFile(m_apiBase, "Experimental probe/golden comparison API");
        ofp()->putsGuard();
        puts("\n#include \"verilated.h\"\n");
        puts("#include <cstdint>\n");
        puts("#include <string>\n");
        puts("#include <vector>\n\n");
        puts("class " + v3Global.opt.prefix() + ";\n\n");
        puts("class " + m_apiClass + " final {\n");
        puts("public:\n");
        puts("    enum class CheckResult : std::uint8_t { Continue, Stop };\n\n");
        puts("    struct Probe final {\n");
        puts("        std::uint32_t id;\n");
        puts("        const char* name;\n");
        puts("        const char* path;\n");
        puts("        const char* mode;\n");
        puts("        std::uint32_t limit;\n");
        puts("        std::uint32_t width;\n");
        puts("        const char* source;\n");
        puts("    };\n\n");
        puts("    explicit " + m_apiClass + "(" + v3Global.opt.prefix() + "* modelp);\n");
        puts("    static const std::vector<Probe>& probes();\n");
        puts("    bool capture(vluint64_t time);\n");
        puts("    bool writeGolden(const std::string& path) const;\n");
        puts("    bool loadGolden(const std::string& path);\n");
        puts("    CheckResult check(vluint64_t time);\n");
        puts("    CheckResult finalCheck(vluint64_t time);\n");
        puts("    std::uint32_t deviations() const { return m_deviations; }\n");
        puts("    const std::string& error() const { return m_error; }\n\n");
        puts("private:\n");
        puts("    struct Transition final {\n");
        puts("        vluint64_t time;\n");
        puts("        std::uint32_t id;\n");
        puts("        std::string value;\n");
        puts("    };\n\n");
        puts("    " + v3Global.opt.prefix() + "* const m_modelp;\n");
        puts("    std::vector<Transition> m_captured;\n");
        puts("    std::vector<Transition> m_golden;\n");
        puts("    std::vector<std::string> m_lastCaptured;\n");
        puts("    std::vector<std::string> m_expected;\n");
        puts("    std::vector<std::uint32_t> m_counts;\n");
        puts("    std::size_t m_nextGolden = 0;\n");
        puts("    std::uint32_t m_deviations = 0;\n");
        puts("    std::string m_error;\n\n");
        puts("    std::string readValue(std::uint32_t id) const;\n");
        puts("    CheckResult reportDeviation(vluint64_t time, std::uint32_t id,\n");
        puts("                                const std::string& expected,\n");
        puts("                                const std::string& got);\n");
        puts("};\n");
        ofp()->putsEndGuard();
        closeOutputFile();
    }

    void emitSource() {
        openNewOutputSourceFile(m_apiBase, false, false,
                                "Experimental probe/golden comparison API");
        puts("#include \"" + m_apiBase + ".h\"\n");
        puts("#include \"" + v3Global.opt.prefix() + ".h\"\n");
        puts("#include \"" + EmitCUtil::prefixNameProtect(m_topModulep) + ".h\"\n\n");
        puts("#include <algorithm>\n");
        puts("#include <cerrno>\n");
        puts("#include <cctype>\n");
        puts("#include <cstdio>\n");
        puts("#include <cstdlib>\n");
        puts("#include <fstream>\n");
        puts("#include <sstream>\n\n");
        puts("namespace {\n\n");
        puts("std::string trimProbeLine(const std::string& text) {\n");
        puts("    std::size_t begin = 0;\n");
        puts("    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;\n");
        puts("    std::size_t end = text.size();\n");
        puts("    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;\n");
        puts("    return text.substr(begin, end - begin);\n");
        puts("}\n\n");
        puts("std::string hexFromQ(QData value, std::uint32_t bits) {\n");
        puts("    static const char* const hex = \"0123456789abcdef\";\n");
        puts("    if (bits < 64U) value &= ((QData{1} << bits) - 1U);\n");
        puts("    const std::uint32_t digits = (bits + 3U) / 4U;\n");
        puts("    std::string out(digits + 2U, '0');\n");
        puts("    out[1] = 'x';\n");
        puts("    for (std::uint32_t i = 0; i < digits; ++i) {\n");
        puts("        const std::uint32_t shift = (digits - 1U - i) * 4U;\n");
        puts("        out[i + 2U] = hex[(value >> shift) & 0xfU];\n");
        puts("    }\n");
        puts("    return out;\n");
        puts("}\n\n");
        puts("std::string hexFromWords(const EData* words, std::uint32_t bits) {\n");
        puts("    static const char* const hex = \"0123456789abcdef\";\n");
        puts("    const std::uint32_t digits = (bits + 3U) / 4U;\n");
        puts("    std::string out(digits + 2U, '0');\n");
        puts("    out[1] = 'x';\n");
        puts("    for (std::uint32_t nibble = 0; nibble < digits; ++nibble) {\n");
        puts("        const std::uint32_t bitBase = (digits - 1U - nibble) * 4U;\n");
        puts("        std::uint32_t value = 0;\n");
        puts("        for (std::uint32_t bit = 0; bit < 4U; ++bit) {\n");
        puts("            const std::uint32_t sourceBit = bitBase + bit;\n");
        puts("            if (sourceBit < bits && ((words[sourceBit / 32U] >> (sourceBit % 32U)) & 1U)) {\n");
        puts("                value |= 1U << bit;\n");
        puts("            }\n");
        puts("        }\n");
        puts("        out[nibble + 2U] = hex[value];\n");
        puts("    }\n");
        puts("    return out;\n");
        puts("}\n\n");
        puts("}  // namespace\n\n");

        puts(m_apiClass + "::" + m_apiClass + "(" + v3Global.opt.prefix() + "* modelp)\n");
        puts("    : m_modelp{modelp}\n");
        puts("    , m_lastCaptured(probes().size())\n");
        puts("    , m_expected(probes().size())\n");
        puts("    , m_counts(probes().size(), 0) {}\n\n");

        puts("const std::vector<" + m_apiClass + "::Probe>& " + m_apiClass + "::probes() {\n");
        puts("    static const std::vector<Probe> probes{\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const ProbeTarget& target = m_targets[id];
            puts("        {" + cvtToStr(id) + "U, ");
            putsQuoted(target.m_spec.m_name);
            puts(", ");
            putsQuoted(target.m_spec.m_path);
            puts(", ");
            putsQuoted(modeName(target.m_spec.m_mode));
            puts(", " + cvtToStr(target.m_spec.m_limit) + "U, " + cvtToStr(target.m_width)
                 + "U, ");
            putsQuoted(target.m_source);
            puts("},\n");
        }
        puts("    };\n");
        puts("    return probes;\n");
        puts("}\n\n");

        puts("std::string " + m_apiClass + "::readValue(std::uint32_t id) const {\n");
        puts("    switch (id) {\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const ProbeTarget& target = m_targets[id];
            const std::string member = memberExpr(target);
            puts("    case " + cvtToStr(id) + "U:\n");
            if (target.m_packedBit >= 0) {
                const int bit = target.m_packedBit;
                if (target.m_varp->width() <= 64) {
                    puts("        return hexFromQ((static_cast<QData>(" + member + ") >> "
                         + cvtToStr(bit) + "U) & 1U, 1U);\n");
                } else {
                    puts("        return hexFromQ((" + member + "[" + cvtToStr(bit / 32)
                         + "U] >> " + cvtToStr(bit % 32) + "U) & 1U, 1U);\n");
                }
            } else if (target.m_width <= 64) {
                puts("        return hexFromQ(static_cast<QData>(" + member + "), "
                     + cvtToStr(target.m_width) + "U);\n");
            } else {
                puts("        return hexFromWords(" + member + ".data(), "
                     + cvtToStr(target.m_width) + "U);\n");
            }
        }
        puts("    default: return \"\";\n");
        puts("    }\n");
        puts("}\n\n");

        puts("bool " + m_apiClass + "::capture(vluint64_t time) {\n");
        puts("    if (!m_modelp) { m_error = \"probe model pointer is null\"; return false; }\n");
        puts("    for (const Probe& probe : probes()) {\n");
        puts("        const std::string value = readValue(probe.id);\n");
        puts("        if (m_lastCaptured[probe.id] != value) {\n");
        puts("            m_captured.push_back({time, probe.id, value});\n");
        puts("            m_lastCaptured[probe.id] = value;\n");
        puts("        }\n");
        puts("    }\n");
        puts("    return true;\n");
        puts("}\n\n");

        puts("bool " + m_apiClass + "::writeGolden(const std::string& path) const {\n");
        puts("    std::ofstream os{path};\n");
        puts("    if (!os) return false;\n");
        puts("    os << \"# verilator probe golden v1\\n\";\n");
        puts("    os << \"# probes \" << probes().size() << \"\\n\";\n");
        puts("    for (const Transition& transition : m_captured) {\n");
        puts("        os << transition.time << ' ' << transition.id << ' ' << transition.value << '\\n';\n");
        puts("    }\n");
        puts("    return static_cast<bool>(os);\n");
        puts("}\n\n");

        puts("bool " + m_apiClass + "::loadGolden(const std::string& path) {\n");
        puts("    std::ifstream is{path};\n");
        puts("    if (!is) { m_error = \"could not open golden file: \" + path; return false; }\n");
        puts("    m_golden.clear();\n");
        puts("    m_nextGolden = 0;\n");
        puts("    std::fill(m_expected.begin(), m_expected.end(), std::string{});\n");
        puts("    std::fill(m_counts.begin(), m_counts.end(), 0U);\n");
        puts("    m_deviations = 0;\n");
        puts("    std::string line;\n");
        puts("    std::uint32_t lineno = 0;\n");
        puts("    while (std::getline(is, line)) {\n");
        puts("        ++lineno;\n");
        puts("        const std::size_t comment = line.find('#');\n");
        puts("        if (comment != std::string::npos) line.erase(comment);\n");
        puts("        line = trimProbeLine(line);\n");
        puts("        if (line.empty()) continue;\n");
        puts("        std::istringstream iss{line};\n");
        puts("        unsigned long long time = 0;\n");
        puts("        std::uint32_t id = 0;\n");
        puts("        std::string value;\n");
        puts("        std::string extra;\n");
        puts("        if (!(iss >> time >> id >> value) || (iss >> extra)) {\n");
        puts("            m_error = \"invalid golden line \" + std::to_string(lineno);\n");
        puts("            return false;\n");
        puts("        }\n");
        puts("        if (id >= probes().size()) {\n");
        puts("            m_error = \"golden line \" + std::to_string(lineno) + \" uses unknown probe id\";\n");
        puts("            return false;\n");
        puts("        }\n");
        puts("        if (value.rfind(\"0x\", 0) != 0) {\n");
        puts("            m_error = \"golden line \" + std::to_string(lineno) + \" value must be hex\";\n");
        puts("            return false;\n");
        puts("        }\n");
        puts("        m_golden.push_back({static_cast<vluint64_t>(time), id, value});\n");
        puts("    }\n");
        puts("    std::stable_sort(m_golden.begin(), m_golden.end(), [](const Transition& a, const Transition& b) {\n");
        puts("        return a.time < b.time;\n");
        puts("    });\n");
        puts("    return true;\n");
        puts("}\n\n");

        puts(m_apiClass + "::CheckResult " + m_apiClass
             + "::reportDeviation(vluint64_t time, std::uint32_t id,\n");
        puts("                                      const std::string& expected,\n");
        puts("                                      const std::string& got) {\n");
        puts("    const Probe& probe = probes()[id];\n");
        puts("    ++m_counts[id];\n");
        puts("    ++m_deviations;\n");
        puts("    const bool shouldPrint = m_counts[id] <= probe.limit;\n");
        puts("    if (shouldPrint) {\n");
        puts("        std::fprintf(stderr,\n");
        puts("                     \"VLT_PROBE_DEVIATION: time=%llu name=%s path=%s expected=%s got=%s count=%u limit=%u\\n\",\n");
        puts("                     static_cast<unsigned long long>(time), probe.name, probe.path,\n");
        puts("                     expected.c_str(), got.c_str(), m_counts[id], probe.limit);\n");
        puts("    }\n");
        puts("    if (std::string{probe.mode} == \"stop\") return CheckResult::Stop;\n");
        puts("    return CheckResult::Continue;\n");
        puts("}\n\n");

        puts(m_apiClass + "::CheckResult " + m_apiClass + "::check(vluint64_t time) {\n");
        puts("    if (!m_modelp) { m_error = \"probe model pointer is null\"; return CheckResult::Stop; }\n");
        puts("    while (m_nextGolden < m_golden.size() && m_golden[m_nextGolden].time <= time) {\n");
        puts("        const Transition& transition = m_golden[m_nextGolden++];\n");
        puts("        m_expected[transition.id] = transition.value;\n");
        puts("    }\n");
        puts("    CheckResult result = CheckResult::Continue;\n");
        puts("    for (const Probe& probe : probes()) {\n");
        puts("        if (m_expected[probe.id].empty()) continue;\n");
        puts("        const std::string got = readValue(probe.id);\n");
        puts("        if (got != m_expected[probe.id]) {\n");
        puts("            if (reportDeviation(time, probe.id, m_expected[probe.id], got) == CheckResult::Stop) {\n");
        puts("                result = CheckResult::Stop;\n");
        puts("            }\n");
        puts("        }\n");
        puts("    }\n");
        puts("    return result;\n");
        puts("}\n\n");

        puts(m_apiClass + "::CheckResult " + m_apiClass + "::finalCheck(vluint64_t time) {\n");
        puts("    const std::size_t unread = m_golden.size() - m_nextGolden;\n");
        puts("    if (m_nextGolden < m_golden.size()) {\n");
        puts("        const Transition& transition = m_golden[m_nextGolden];\n");
        puts("        const Probe& probe = probes()[transition.id];\n");
        puts("        std::fprintf(stderr,\n");
        puts("                     \"VLT_PROBE_GOLDEN_UNREAD: time=%llu name=%s path=%s golden_time=%llu expected=%s\\n\",\n");
        puts("                     static_cast<unsigned long long>(time), probe.name, probe.path,\n");
        puts("                     static_cast<unsigned long long>(transition.time), transition.value.c_str());\n");
        puts("        ++m_deviations;\n");
        puts("        std::fprintf(stderr,\n");
        puts("                     \"VLT_PROBE_SUMMARY: time=%llu probes=%zu deviations=%u unread=%zu\\n\",\n");
        puts("                     static_cast<unsigned long long>(time), probes().size(), m_deviations,\n");
        puts("                     unread);\n");
        puts("        return CheckResult::Stop;\n");
        puts("    }\n");
        puts("    std::fprintf(stderr,\n");
        puts("                 \"VLT_PROBE_SUMMARY: time=%llu probes=%zu deviations=%u unread=%zu\\n\",\n");
        puts("                 static_cast<unsigned long long>(time), probes().size(), m_deviations,\n");
        puts("                 unread);\n");
        puts("    return CheckResult::Continue;\n");
        puts("}\n");
        closeOutputFile();
    }

    void emitManifest() const {
        V3OutJsonFile of{v3Global.opt.makeDir() + "/probe_points.json"};
        of.put("top_module", AstNode::prettyName(v3Global.opt.topModule()));
        of.put("probe_config", v3Global.opt.probeConfig());
        of.begin("probes", '[');
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const ProbeTarget& target = m_targets[id];
            of.begin();
            of.put("id", static_cast<int>(id));
            of.put("name", target.m_spec.m_name);
            of.put("path", target.m_spec.m_path);
            of.put("mode", modeName(target.m_spec.m_mode));
            of.put("limit", static_cast<int>(target.m_spec.m_limit));
            of.put("width", target.m_width);
            of.put("source", target.m_source);
            of.end();
        }
        of.end();
    }

public:
    explicit EmitCProbeApi(AstNodeModule* topModulep)
        : m_topModulep{topModulep}
        , m_specs{probeSpecs()} {
        collectTargets();
        if (!m_targets.empty()) {
            emitHeader();
            emitSource();
            emitManifest();
        }
    }
};

}  // namespace

void V3EmitC::prepareProbeApi() {
    UINFO(2, __FUNCTION__ << ":");
    PrepareProbeApi{v3Global.rootp()};
}

void V3EmitC::emitcProbeApi() {
    UINFO(2, __FUNCTION__ << ":");
    EmitCProbeApi{v3Global.rootp()->topModulep()};
}
