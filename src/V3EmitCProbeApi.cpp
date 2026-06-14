// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit experimental probe/checker API
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

enum class SignalKind : uint8_t { PROBE, CHECKER };
enum class ProbeMode : uint8_t { ALL, FIRST };

struct SignalSpec final {
    SignalKind m_kind = SignalKind::PROBE;
    std::string m_name;
    std::string m_path;
    std::string m_basePath;
    std::string m_signalName;
    ProbeMode m_mode = ProbeMode::FIRST;
    uint32_t m_logLimit = 1;
    bool m_conditionEquals = false;
    std::string m_conditionText = "!= 0";
    std::string m_conditionHex = "0x0";
    std::vector<int> m_selects;
};

struct AssessmentSpec final {
    std::string m_name;
    std::vector<std::string> m_probes;
    std::vector<std::string> m_checkers;
    std::string m_detectorPolicy = "any";
    std::string m_maxDelay;
    std::string m_delayFrom = "deviation";
};

struct ProbeConfig final {
    std::vector<SignalSpec> m_signals;
    std::vector<AssessmentSpec> m_assessments;
};

struct SignalTarget final {
    const AstVar* m_varp = nullptr;
    SignalSpec m_spec;
    std::string m_source;
    int m_width = 0;
    int m_packedBit = -1;
    std::vector<uint32_t> m_indices;
};

struct ConfigCommand final {
    int m_lineno = 0;
    std::vector<std::string> m_tokens;
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
    return false;
}

static std::string stripHexZeros(std::string digits) {
    size_t first = 0;
    while (first + 1 < digits.size() && digits[first] == '0') ++first;
    return digits.substr(first);
}

static bool decimalToHex(std::string digits, std::string* const hexp) {
    if (digits.empty()) return false;
    for (const char ch : digits) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    digits.erase(0, std::min(digits.find_first_not_of('0'), digits.size() - 1));
    std::string hex;
    static const char* const hexChars = "0123456789abcdef";
    while (digits != "0") {
        std::string quotient;
        unsigned carry = 0;
        for (const char ch : digits) {
            const unsigned value = carry * 10U + static_cast<unsigned>(ch - '0');
            if (!quotient.empty() || value / 16U != 0U) {
                quotient += static_cast<char>('0' + value / 16U);
            }
            carry = value % 16U;
        }
        hex += hexChars[carry];
        digits = quotient.empty() ? "0" : quotient;
    }
    if (hex.empty()) hex = "0";
    std::reverse(hex.begin(), hex.end());
    *hexp = "0x" + hex;
    return true;
}

static bool canonicalInteger(const std::string& text, std::string* const hexp) {
    if (text.empty()) return false;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        std::string digits = text.substr(2);
        if (digits.empty()) return false;
        for (char& ch : digits) {
            if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        *hexp = "0x" + stripHexZeros(digits);
        return true;
    }
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        const std::string bits = text.substr(2);
        if (bits.empty()) return false;
        for (const char ch : bits) {
            if (ch != '0' && ch != '1') return false;
        }
        std::string padded((4U - bits.size() % 4U) % 4U, '0');
        padded += bits;
        std::string digits;
        static const char* const hexChars = "0123456789abcdef";
        for (size_t pos = 0; pos < padded.size(); pos += 4) {
            unsigned value = 0;
            for (size_t bit = 0; bit < 4; ++bit) value = value * 2U + (padded[pos + bit] - '0');
            digits += hexChars[value];
        }
        *hexp = "0x" + stripHexZeros(digits);
        return true;
    }
    return decimalToHex(text, hexp);
}

static int hexWidth(const std::string& text) {
    const std::string digits = text.substr(2);
    if (digits == "0") return 1;
    const char first = digits[0];
    const int value = first <= '9' ? first - '0' : first - 'a' + 10;
    int highBits = 0;
    for (int copy = value; copy; copy >>= 1) ++highBits;
    return static_cast<int>((digits.size() - 1U) * 4U) + highBits;
}

static bool parseCondition(const std::string& text, SignalSpec* const specp) {
    std::string compact;
    for (const char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) compact += ch;
    }
    if (compact.rfind("==", 0) == 0) {
        specp->m_conditionEquals = true;
    } else if (compact.rfind("!=", 0) == 0) {
        specp->m_conditionEquals = false;
    } else {
        return false;
    }
    const std::string value = compact.substr(2);
    if (!canonicalInteger(value, &specp->m_conditionHex)) return false;
    specp->m_conditionText = std::string{specp->m_conditionEquals ? "== " : "!= "} + value;
    return true;
}

static bool validTime(const std::string& text) {
    static const std::unordered_set<std::string> units{"fs", "ps", "ns", "us", "ms", "s"};
    size_t pos = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos == 0) return false;
    if (pos < text.size() && text[pos] == '.') {
        const size_t fractional = ++pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos == fractional) return false;
    }
    return units.count(text.substr(pos));
}

static std::vector<ConfigCommand> tokenizeConfig(const std::string& text,
                                                 const std::string& filename) {
    std::vector<ConfigCommand> commands;
    ConfigCommand command;
    std::string token;
    bool tokenActive = false;
    char quote = 0;
    int lineno = 1;
    int tokenLine = 1;
    for (size_t pos = 0; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (quote) {
            if (ch == quote) {
                quote = 0;
            } else if (ch == '\\' && quote == '"' && pos + 1 < text.size()) {
                if (text[pos + 1] == '\n') {
                    ++pos;
                    ++lineno;
                } else {
                    token += text[++pos];
                }
            } else {
                token += ch;
                if (ch == '\n') ++lineno;
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            if (!tokenActive) tokenLine = lineno;
            tokenActive = true;
            quote = ch;
        } else if (ch == '\\' && pos + 1 < text.size()) {
            if (text[pos + 1] == '\n') {
                ++pos;
                ++lineno;
            } else {
                if (!tokenActive) tokenLine = lineno;
                tokenActive = true;
                token += text[++pos];
            }
        } else if (ch == '#') {
            while (pos + 1 < text.size() && text[pos + 1] != '\n') ++pos;
        } else if (ch == '\n') {
            if (tokenActive) {
                if (command.m_tokens.empty()) command.m_lineno = tokenLine;
                command.m_tokens.push_back(token);
                token.clear();
                tokenActive = false;
            }
            if (!command.m_tokens.empty()) {
                commands.push_back(command);
                command = ConfigCommand{};
            }
            ++lineno;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            if (tokenActive) {
                if (command.m_tokens.empty()) command.m_lineno = tokenLine;
                command.m_tokens.push_back(token);
                token.clear();
                tokenActive = false;
            }
        } else {
            if (!tokenActive) tokenLine = lineno;
            tokenActive = true;
            token += ch;
        }
    }
    if (quote) {
        v3error(filename + ":" + cvtToStr(tokenLine) + ": unterminated quote in probe config");
        return {};
    }
    if (tokenActive) {
        if (command.m_tokens.empty()) command.m_lineno = tokenLine;
        command.m_tokens.push_back(token);
    }
    if (!command.m_tokens.empty()) commands.push_back(command);
    return commands;
}

static bool parseLocation(SignalSpec* const specp, const std::string& filename,
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
        const size_t end = signal.find(']', pos + 1);
        if (end == std::string::npos) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": unterminated probe selector: " + specp->m_path);
            return false;
        }
        const std::string indexText = signal.substr(pos + 1, end - pos - 1);
        if (indexText.find(':') != std::string::npos) {
            v3error(filename + ":" + cvtToStr(lineno)
                    + ": probe part-select ranges are not supported: " + specp->m_path);
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

static bool parseSignalCommand(const ConfigCommand& command, const std::string& filename,
                               const SignalKind kind, SignalSpec* const specp) {
    const std::vector<std::string>& tokens = command.m_tokens;
    if (tokens.size() < 2) {
        v3error(filename + ":" + cvtToStr(command.m_lineno) + ": command requires a name");
        return false;
    }
    specp->m_kind = kind;
    specp->m_name = tokens[1];
    specp->m_logLimit = kind == SignalKind::PROBE ? 1 : 1000;
    bool havePath = false;
    bool haveLogLimit = false;
    for (size_t pos = 2; pos < tokens.size();) {
        const std::string& option = tokens[pos++];
        if (pos == tokens.size()) {
            v3error(filename + ":" + cvtToStr(command.m_lineno)
                    + ": missing value for option " + option);
            return false;
        }
        const std::string& value = tokens[pos++];
        if (option == "--path") {
            specp->m_path = value;
            havePath = true;
        } else if (option == "--log-limit") {
            if (!parseUint32(value, &specp->m_logLimit)) {
                v3error(filename + ":" + cvtToStr(command.m_lineno)
                        + ": --log-limit must be a non-negative integer: " + value);
                return false;
            }
            haveLogLimit = true;
        } else if (kind == SignalKind::PROBE && option == "--mode") {
            if (!parseMode(value, &specp->m_mode)) {
                v3error(filename + ":" + cvtToStr(command.m_lineno)
                        + ": --mode must be first or all: " + value);
                return false;
            }
        } else if (kind == SignalKind::CHECKER && option == "--condition") {
            if (!parseCondition(value, specp)) {
                v3error(filename + ":" + cvtToStr(command.m_lineno)
                        + ": --condition must use == or != with an integer: " + value);
                return false;
            }
        } else {
            v3error(filename + ":" + cvtToStr(command.m_lineno) + ": unknown option " + option);
            return false;
        }
    }
    if (!havePath) {
        v3error(filename + ":" + cvtToStr(command.m_lineno) + ": command requires --path");
        return false;
    }
    if (kind == SignalKind::PROBE && specp->m_mode == ProbeMode::ALL && !haveLogLimit) {
        specp->m_logLimit = 1000;
    }
    return parseLocation(specp, filename, command.m_lineno);
}

static bool parseAssessmentCommand(const ConfigCommand& command, const std::string& filename,
                                   AssessmentSpec* const specp) {
    const std::vector<std::string>& tokens = command.m_tokens;
    if (tokens.size() < 2) {
        v3error(filename + ":" + cvtToStr(command.m_lineno) + ": command requires a name");
        return false;
    }
    specp->m_name = tokens[1];
    for (size_t pos = 2; pos < tokens.size();) {
        const std::string& option = tokens[pos++];
        if (pos == tokens.size()) {
            v3error(filename + ":" + cvtToStr(command.m_lineno)
                    + ": missing value for option " + option);
            return false;
        }
        const std::string& value = tokens[pos++];
        if (option == "--probe") {
            specp->m_probes.push_back(value);
        } else if (option == "--checker") {
            specp->m_checkers.push_back(value);
        } else if (option == "--detector-policy") {
            if (value != "any" && value != "all") {
                v3error(filename + ":" + cvtToStr(command.m_lineno)
                        + ": --detector-policy must be any or all: " + value);
                return false;
            }
            specp->m_detectorPolicy = value;
        } else if (option == "--max-delay") {
            if (!validTime(value)) {
                v3error(filename + ":" + cvtToStr(command.m_lineno)
                        + ": --max-delay requires a non-negative unit-suffixed time: " + value);
                return false;
            }
            specp->m_maxDelay = value;
        } else if (option == "--delay-from") {
            if (value != "deviation" && value != "injection") {
                v3error(filename + ":" + cvtToStr(command.m_lineno)
                        + ": --delay-from must be deviation or injection: " + value);
                return false;
            }
            specp->m_delayFrom = value;
        } else {
            v3error(filename + ":" + cvtToStr(command.m_lineno) + ": unknown option " + option);
            return false;
        }
    }
    if (specp->m_probes.empty() || specp->m_checkers.empty()) {
        v3error(filename + ":" + cvtToStr(command.m_lineno)
                + ": assessment requires at least one --probe and one --checker");
        return false;
    }
    const std::unordered_set<std::string> probes{specp->m_probes.begin(), specp->m_probes.end()};
    const std::unordered_set<std::string> checkers{specp->m_checkers.begin(),
                                                   specp->m_checkers.end()};
    if (probes.size() != specp->m_probes.size() || checkers.size() != specp->m_checkers.size()) {
        v3error(filename + ":" + cvtToStr(command.m_lineno)
                + ": assessment probe and checker references must be unique");
        return false;
    }
    return true;
}

static ProbeConfig parseProbeConfig() {
    const std::string filename = v3Global.opt.probeConfig();
    const std::unique_ptr<std::ifstream> ifp{V3File::new_ifstream(filename)};
    if (ifp->fail()) {
        v3error("Cannot open --probe-config file: " + filename);
        return {};
    }
    std::ostringstream buffer;
    buffer << ifp->rdbuf();
    const std::vector<ConfigCommand> commands = tokenizeConfig(buffer.str(), filename);
    ProbeConfig config;
    std::unordered_set<std::string> names;
    for (const ConfigCommand& command : commands) {
        const std::string& verb = command.m_tokens[0];
        std::string name;
        bool ok = false;
        if (verb == "create_probe" || verb == "create_checker") {
            SignalSpec spec;
            ok = parseSignalCommand(command, filename,
                                    verb == "create_probe" ? SignalKind::PROBE
                                                           : SignalKind::CHECKER,
                                    &spec);
            name = spec.m_name;
            if (ok) config.m_signals.push_back(spec);
        } else if (verb == "create_assessment") {
            AssessmentSpec spec;
            ok = parseAssessmentCommand(command, filename, &spec);
            name = spec.m_name;
            if (ok) config.m_assessments.push_back(spec);
        } else {
            v3error(filename + ":" + cvtToStr(command.m_lineno) + ": unknown command " + verb);
        }
        if (ok && (!names.insert(name).second || name.empty())) {
            v3error(filename + ":" + cvtToStr(command.m_lineno)
                    + ": object names must be non-empty and unique: " + name);
        }
    }
    std::unordered_set<std::string> probes;
    std::unordered_set<std::string> checkers;
    for (const SignalSpec& spec : config.m_signals) {
        (spec.m_kind == SignalKind::PROBE ? probes : checkers).insert(spec.m_name);
    }
    for (const AssessmentSpec& spec : config.m_assessments) {
        for (const std::string& name : spec.m_probes) {
            if (!probes.count(name)) {
                v3error("assessment '" + spec.m_name + "' references unknown probe: " + name);
            }
        }
        for (const std::string& name : spec.m_checkers) {
            if (!checkers.count(name)) {
                v3error("assessment '" + spec.m_name + "' references unknown checker: " + name);
            }
        }
    }
    if (config.m_signals.empty()) {
        v3error("--probe-config did not define any probes or checkers: " + filename);
    }
    return config;
}

static const ProbeConfig& probeConfig() {
    static const ProbeConfig config = parseProbeConfig();
    return config;
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
        for (const SignalSpec& spec : probeConfig().m_signals) m_signalNames.insert(spec.m_signalName);
        if (!m_signalNames.empty()) iterate(rootp);
    }
};

class EmitCProbeApi final : public EmitCBaseVisitorConst {
    AstNodeModule* const m_topModulep;
    const std::string m_apiBase = v3Global.opt.prefix() + "__probe_api";
    const std::string m_apiClass = v3Global.opt.prefix() + "ProbeApi";
    std::vector<SignalSpec> m_specs;
    std::vector<SignalTarget> m_targets;

    void visit(AstNode*) override {}

    bool makeTarget(const SignalSpec& spec, const AstVar* const varp, SignalTarget* const targetp) {
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
        }
        if (spec.m_kind == SignalKind::CHECKER && hexWidth(spec.m_conditionHex) > targetp->m_width) {
            v3error("checker '" + spec.m_name + "' condition does not fit signal width: "
                    + spec.m_conditionText);
            return false;
        }
        return true;
    }

    void collectTargets() {
        std::vector<bool> matched(m_specs.size(), false);
        for (size_t i = 0; i < m_specs.size(); ++i) {
            const SignalSpec& spec = m_specs[i];
            for (const AstNode* nodep = m_topModulep->stmtsp(); nodep; nodep = nodep->nextp()) {
                const AstVar* const varp = VN_CAST(nodep, Var);
                if (!varp) continue;
                if (AstNode::prettyName(varp->name()) != spec.m_basePath) continue;
                matched[i] = true;
                SignalTarget target;
                if (makeTarget(spec, varp, &target)) m_targets.push_back(target);
                break;
            }
        }
        for (size_t i = 0; i < m_specs.size(); ++i) {
            if (!matched[i]) v3error("probe did not match a generated signal: " + m_specs[i].m_path);
        }
    }

    std::string memberExpr(const SignalTarget& target) const {
        std::string member = "m_modelp->rootp->" + target.m_varp->nameProtect();
        for (const uint32_t index : target.m_indices) member += "[" + cvtToStr(index) + "U]";
        return member;
    }

    void emitHeader() {
        openNewOutputHeaderFile(m_apiBase, "Experimental probe/checker API");
        ofp()->putsGuard();
        puts("\n#include \"verilated.h\"\n");
        puts("#include <cstdint>\n#include <string>\n#include <vector>\n\n");
        puts("class " + v3Global.opt.prefix() + ";\n\n");
        puts("class " + m_apiClass + " final {\npublic:\n");
        puts("    struct Signal final {\n");
        puts("        std::uint32_t id;\n        const char* kind;\n        const char* name;\n");
        puts("        const char* path;\n        const char* mode;\n        std::uint32_t logLimit;\n");
        puts("        std::uint32_t width;\n        const char* condition;\n");
        puts("        const char* conditionValue;\n        bool conditionEquals;\n");
        puts("        const char* source;\n    };\n\n");
        puts("    explicit " + m_apiClass + "(" + v3Global.opt.prefix() + "* modelp);\n");
        puts("    static const std::vector<Signal>& signals();\n");
        puts("    bool capture(vluint64_t time);\n");
        puts("    bool writeGolden(const std::string& path) const;\n");
        puts("    bool loadGolden(const std::string& path);\n");
        puts("    bool beginInjection(vluint64_t time);\n");
        puts("    bool sample(vluint64_t time);\n");
        puts("    bool finalCheck(vluint64_t time);\n");
        puts("    bool writeResults(const std::string& path, vluint64_t endTime) const;\n");
        puts("    const std::string& error() const { return m_error; }\n\nprivate:\n");
        puts("    struct Transition final { vluint64_t time; std::uint32_t id; std::string value; };\n");
        puts("    struct ProbeEvent final { vluint64_t time; std::uint32_t id; std::string expected; std::string actual; };\n");
        puts("    struct CheckerEvent final { vluint64_t time; std::uint32_t id; std::string value; };\n\n");
        puts("    " + v3Global.opt.prefix() + "* const m_modelp;\n");
        puts("    std::vector<Transition> m_captured;\n    std::vector<Transition> m_golden;\n");
        puts("    std::vector<std::string> m_lastCaptured;\n    std::vector<std::string> m_expected;\n");
        puts("    std::vector<bool> m_checkerInitialized;\n    std::vector<bool> m_checkerActive;\n");
        puts("    std::vector<int> m_activeAtInjection;\n    std::vector<std::uint64_t> m_counts;\n");
        puts("    std::vector<vluint64_t> m_firstTimes;\n    std::vector<vluint64_t> m_lastTimes;\n");
        puts("    std::vector<ProbeEvent> m_probeEvents;\n    std::vector<CheckerEvent> m_checkerEvents;\n");
        puts("    std::size_t m_nextGolden = 0;\n    bool m_goldenLoaded = false;\n");
        puts("    bool m_injected = false;\n    vluint64_t m_injectionTime = 0;\n");
        puts("    std::string m_error;\n\n");
        puts("    std::string readValue(std::uint32_t id) const;\n");
        puts("    bool conditionActive(const Signal& signal) const;\n");
        puts("    void advanceGolden(vluint64_t time);\n");
        puts("    bool checkFunctional(vluint64_t time);\n");
        puts("    void sampleCheckers(vluint64_t time);\n");
        puts("    void recordDeviation(vluint64_t time, const Signal& signal, const std::string& expected, const std::string& actual);\n");
        puts("    void recordChecker(vluint64_t time, const Signal& signal, const std::string& value);\n");
        puts("};\n");
        ofp()->putsEndGuard();
        closeOutputFile();
    }

    void emitSource() {
        openNewOutputSourceFile(m_apiBase, false, false, "Experimental probe/checker API");
        puts("#include \"" + m_apiBase + ".h\"\n#include \"" + v3Global.opt.prefix()
             + ".h\"\n#include \"" + EmitCUtil::prefixNameProtect(m_topModulep) + ".h\"\n\n");
        puts("#include <algorithm>\n#include <cctype>\n#include <cstdio>\n#include <fstream>\n");
        puts("#include <sstream>\n#include <unordered_set>\n\nnamespace {\n\n");
        puts("std::string trimProbeLine(const std::string& text) {\n");
        puts("    std::size_t begin = 0;\n");
        puts("    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;\n");
        puts("    std::size_t end = text.size();\n");
        puts("    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;\n");
        puts("    return text.substr(begin, end - begin);\n}\n\n");
        puts("std::string canonicalHex(std::string value) {\n");
        puts("    if (value.rfind(\"0x\", 0) == 0) value.erase(0, 2);\n");
        puts("    std::size_t first = 0;\n");
        puts("    while (first + 1 < value.size() && value[first] == '0') ++first;\n");
        puts("    return \"0x\" + value.substr(first);\n}\n\n");
        puts("std::string jsonQuote(const std::string& value) {\n");
        puts("    std::string out{\"\\\"\"};\n");
        puts("    for (const char ch : value) {\n");
        puts("        if (ch == '\\\\' || ch == '\"') out += '\\\\';\n");
        puts("        out += ch;\n    }\n    out += '\"';\n    return out;\n}\n\n");
        puts("std::string hexFromQ(QData value, std::uint32_t bits) {\n");
        puts("    static const char* const hex = \"0123456789abcdef\";\n");
        puts("    if (bits < 64U) value &= ((QData{1} << bits) - 1U);\n");
        puts("    const std::uint32_t digits = (bits + 3U) / 4U;\n");
        puts("    std::string out(digits + 2U, '0');\n    out[1] = 'x';\n");
        puts("    for (std::uint32_t i = 0; i < digits; ++i) {\n");
        puts("        const std::uint32_t shift = (digits - 1U - i) * 4U;\n");
        puts("        out[i + 2U] = hex[(value >> shift) & 0xfU];\n    }\n    return out;\n}\n\n");
        puts("std::string hexFromWords(const EData* words, std::uint32_t bits) {\n");
        puts("    static const char* const hex = \"0123456789abcdef\";\n");
        puts("    const std::uint32_t digits = (bits + 3U) / 4U;\n");
        puts("    std::string out(digits + 2U, '0');\n    out[1] = 'x';\n");
        puts("    for (std::uint32_t nibble = 0; nibble < digits; ++nibble) {\n");
        puts("        const std::uint32_t bitBase = (digits - 1U - nibble) * 4U;\n");
        puts("        std::uint32_t value = 0;\n");
        puts("        for (std::uint32_t bit = 0; bit < 4U; ++bit) {\n");
        puts("            const std::uint32_t sourceBit = bitBase + bit;\n");
        puts("            if (sourceBit < bits && ((words[sourceBit / 32U] >> (sourceBit % 32U)) & 1U)) value |= 1U << bit;\n");
        puts("        }\n        out[nibble + 2U] = hex[value];\n    }\n    return out;\n}\n\n");
        puts("}  // namespace\n\n");

        puts(m_apiClass + "::" + m_apiClass + "(" + v3Global.opt.prefix() + "* modelp)\n");
        puts("    : m_modelp{modelp}\n    , m_lastCaptured(signals().size())\n");
        puts("    , m_expected(signals().size())\n    , m_checkerInitialized(signals().size(), false)\n");
        puts("    , m_checkerActive(signals().size(), false)\n    , m_activeAtInjection(signals().size(), -1)\n");
        puts("    , m_counts(signals().size(), 0)\n    , m_firstTimes(signals().size(), 0)\n");
        puts("    , m_lastTimes(signals().size(), 0) {}\n\n");

        puts("const std::vector<" + m_apiClass + "::Signal>& " + m_apiClass + "::signals() {\n");
        puts("    static const std::vector<Signal> signals{\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const SignalTarget& target = m_targets[id];
            puts("        {" + cvtToStr(id) + "U, ");
            putsQuoted(target.m_spec.m_kind == SignalKind::PROBE ? "probe" : "checker");
            puts(", ");
            putsQuoted(target.m_spec.m_name);
            puts(", ");
            putsQuoted(target.m_spec.m_path);
            puts(", ");
            putsQuoted(target.m_spec.m_kind == SignalKind::PROBE ? modeName(target.m_spec.m_mode) : "");
            puts(", " + cvtToStr(target.m_spec.m_logLimit) + "U, " + cvtToStr(target.m_width) + "U, ");
            putsQuoted(target.m_spec.m_kind == SignalKind::CHECKER ? target.m_spec.m_conditionText : "");
            puts(", ");
            putsQuoted(target.m_spec.m_kind == SignalKind::CHECKER ? target.m_spec.m_conditionHex : "");
            puts(", " + std::string{target.m_spec.m_conditionEquals ? "true" : "false"} + ", ");
            putsQuoted(target.m_source);
            puts("},\n");
        }
        puts("    };\n    return signals;\n}\n\n");

        puts("std::string " + m_apiClass + "::readValue(std::uint32_t id) const {\n    switch (id) {\n");
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const SignalTarget& target = m_targets[id];
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
        puts("    default: return \"\";\n    }\n}\n\n");

        puts("bool " + m_apiClass + "::conditionActive(const Signal& signal) const {\n");
        puts("    const bool equal = canonicalHex(readValue(signal.id)) == signal.conditionValue;\n");
        puts("    return signal.conditionEquals ? equal : !equal;\n}\n\n");
        puts("void " + m_apiClass + "::advanceGolden(vluint64_t time) {\n");
        puts("    while (m_nextGolden < m_golden.size() && m_golden[m_nextGolden].time <= time) {\n");
        puts("        const Transition& transition = m_golden[m_nextGolden++];\n");
        puts("        m_expected[transition.id] = transition.value;\n    }\n}\n\n");

        puts("bool " + m_apiClass + "::capture(vluint64_t time) {\n");
        puts("    if (!m_modelp) { m_error = \"probe model pointer is null\"; return false; }\n");
        puts("    for (const Signal& signal : signals()) {\n");
        puts("        if (std::string{signal.kind} != \"probe\") continue;\n");
        puts("        const std::string value = readValue(signal.id);\n");
        puts("        if (m_lastCaptured[signal.id] != value) {\n");
        puts("            m_captured.push_back({time, signal.id, value});\n");
        puts("            m_lastCaptured[signal.id] = value;\n        }\n    }\n    return true;\n}\n\n");

        puts("bool " + m_apiClass + "::writeGolden(const std::string& path) const {\n");
        puts("    std::ofstream os{path};\n    if (!os) return false;\n");
        puts("    std::size_t count = 0;\n");
        puts("    for (const Signal& signal : signals()) if (std::string{signal.kind} == \"probe\") ++count;\n");
        puts("    os << \"# verilator probe golden v2\\n# probes \" << count << '\\n';\n");
        puts("    for (const Transition& transition : m_captured) os << transition.time << ' ' << transition.id << ' ' << transition.value << '\\n';\n");
        puts("    return static_cast<bool>(os);\n}\n\n");

        puts("bool " + m_apiClass + "::loadGolden(const std::string& path) {\n");
        puts("    std::ifstream is{path};\n");
        puts("    if (!is) { m_error = \"could not open golden file: \" + path; return false; }\n");
        puts("    m_golden.clear();\n    m_nextGolden = 0;\n");
        puts("    std::fill(m_expected.begin(), m_expected.end(), std::string{});\n");
        puts("    std::string header;\n    if (!std::getline(is, header) || header != \"# verilator probe golden v2\") {\n");
        puts("        m_error = \"unsupported golden stream format\";\n        return false;\n    }\n");
        puts("    std::string line;\n    std::uint32_t lineno = 1;\n");
        puts("    while (std::getline(is, line)) {\n        ++lineno;\n");
        puts("        const std::size_t comment = line.find('#');\n");
        puts("        if (comment != std::string::npos) line.erase(comment);\n");
        puts("        line = trimProbeLine(line);\n        if (line.empty()) continue;\n");
        puts("        std::istringstream iss{line};\n        unsigned long long time = 0;\n");
        puts("        std::uint32_t id = 0;\n        std::string value;\n        std::string extra;\n");
        puts("        if (!(iss >> time >> id >> value) || (iss >> extra)) {\n");
        puts("            m_error = \"invalid golden line \" + std::to_string(lineno);\n            return false;\n        }\n");
        puts("        if (id >= signals().size() || std::string{signals()[id].kind} != \"probe\") {\n");
        puts("            m_error = \"golden line \" + std::to_string(lineno) + \" uses unknown probe id\";\n            return false;\n        }\n");
        puts("        if (value.rfind(\"0x\", 0) != 0) {\n");
        puts("            m_error = \"golden line \" + std::to_string(lineno) + \" value must be hex\";\n            return false;\n        }\n");
        puts("        m_golden.push_back({static_cast<vluint64_t>(time), id, value});\n    }\n");
        puts("    std::stable_sort(m_golden.begin(), m_golden.end(), [](const Transition& a, const Transition& b) { return a.time < b.time; });\n");
        puts("    m_goldenLoaded = true;\n    return true;\n}\n\n");

        puts("void " + m_apiClass + "::recordDeviation(vluint64_t time, const Signal& signal,\n");
        puts("                                              const std::string& expected, const std::string& actual) {\n");
        puts("    ++m_counts[signal.id];\n");
        puts("    if (m_counts[signal.id] == 1U) m_firstTimes[signal.id] = time;\n");
        puts("    m_lastTimes[signal.id] = time;\n");
        puts("    if (std::string{signal.mode} == \"all\" || m_counts[signal.id] == 1U) m_probeEvents.push_back({time, signal.id, expected, actual});\n");
        puts("    if (m_counts[signal.id] <= signal.logLimit) {\n");
        puts("        std::fprintf(stderr, \"VFI_PROBE_DEVIATION time_ticks=%llu name=%s path=%s expected=%s actual=%s count=%llu\\n\",\n");
        puts("                     static_cast<unsigned long long>(time), jsonQuote(signal.name).c_str(),\n");
        puts("                     jsonQuote(signal.path).c_str(), jsonQuote(expected).c_str(),\n");
        puts("                     jsonQuote(actual).c_str(), static_cast<unsigned long long>(m_counts[signal.id]));\n    }\n}\n\n");

        puts("void " + m_apiClass + "::recordChecker(vluint64_t time, const Signal& signal, const std::string& value) {\n");
        puts("    ++m_counts[signal.id];\n");
        puts("    if (m_counts[signal.id] == 1U) m_firstTimes[signal.id] = time;\n");
        puts("    m_lastTimes[signal.id] = time;\n    m_checkerEvents.push_back({time, signal.id, value});\n");
        puts("    if (m_counts[signal.id] <= signal.logLimit) {\n");
        puts("        std::fprintf(stderr, \"VFI_CHECKER_TRIGGER time_ticks=%llu name=%s path=%s condition=%s value=%s count=%llu\\n\",\n");
        puts("                     static_cast<unsigned long long>(time), jsonQuote(signal.name).c_str(),\n");
        puts("                     jsonQuote(signal.path).c_str(), jsonQuote(signal.condition).c_str(),\n");
        puts("                     jsonQuote(value).c_str(), static_cast<unsigned long long>(m_counts[signal.id]));\n    }\n}\n\n");

        puts("bool " + m_apiClass + "::checkFunctional(vluint64_t time) {\n");
        puts("    if (!m_goldenLoaded) return true;\n");
        puts("    for (const Signal& signal : signals()) {\n");
        puts("        if (std::string{signal.kind} != \"probe\" || m_expected[signal.id].empty()) continue;\n");
        puts("        const std::string actual = readValue(signal.id);\n");
        puts("        if (actual == m_expected[signal.id]) continue;\n");
        puts("        if (!m_injected) {\n");
        puts("            m_error = \"functional probe deviated before injection: \" + std::string{signal.name};\n            return false;\n        }\n");
        puts("        recordDeviation(time, signal, m_expected[signal.id], actual);\n    }\n    return true;\n}\n\n");

        puts("void " + m_apiClass + "::sampleCheckers(vluint64_t time) {\n");
        puts("    for (const Signal& signal : signals()) {\n");
        puts("        if (std::string{signal.kind} != \"checker\") continue;\n");
        puts("        const bool active = conditionActive(signal);\n");
        puts("        if (m_checkerInitialized[signal.id] && m_injected && !m_checkerActive[signal.id] && active) recordChecker(time, signal, readValue(signal.id));\n");
        puts("        m_checkerInitialized[signal.id] = true;\n        m_checkerActive[signal.id] = active;\n    }\n}\n\n");

        puts("bool " + m_apiClass + "::beginInjection(vluint64_t time) {\n");
        puts("    if (!m_modelp) { m_error = \"probe model pointer is null\"; return false; }\n");
        puts("    advanceGolden(time);\n    if (!checkFunctional(time)) return false;\n");
        puts("    for (const Signal& signal : signals()) {\n");
        puts("        if (std::string{signal.kind} != \"checker\") continue;\n");
        puts("        const bool active = conditionActive(signal);\n");
        puts("        m_checkerInitialized[signal.id] = true;\n        m_checkerActive[signal.id] = active;\n");
        puts("        m_activeAtInjection[signal.id] = active ? 1 : 0;\n    }\n");
        puts("    m_injected = true;\n    m_injectionTime = time;\n    return true;\n}\n\n");

        puts("bool " + m_apiClass + "::sample(vluint64_t time) {\n");
        puts("    if (!m_modelp) { m_error = \"probe model pointer is null\"; return false; }\n");
        puts("    advanceGolden(time);\n    if (!checkFunctional(time)) return false;\n");
        puts("    sampleCheckers(time);\n    return true;\n}\n\n");

        puts("bool " + m_apiClass + "::finalCheck(vluint64_t time) {\n");
        puts("    if (!m_goldenLoaded || m_nextGolden == m_golden.size()) return true;\n");
        puts("    if (!m_injected) { m_error = \"golden stream has unread transitions before injection\"; return false; }\n");
        puts("    std::unordered_set<std::uint32_t> unread;\n");
        puts("    for (std::size_t pos = m_nextGolden; pos < m_golden.size(); ++pos) {\n");
        puts("        const Transition& transition = m_golden[pos];\n");
        puts("        if (unread.insert(transition.id).second) recordDeviation(time, signals()[transition.id], transition.value, readValue(transition.id));\n");
        puts("    }\n    m_nextGolden = m_golden.size();\n    return true;\n}\n\n");

        puts("bool " + m_apiClass + "::writeResults(const std::string& path, vluint64_t endTime) const {\n");
        puts("    std::ofstream os{path};\n    if (!os) return false;\n");
        puts("    os << \"VFI_PROBE_RESULTS_V1\\n\";\n");
        puts("    if (m_injected) os << \"injection \" << m_injectionTime << '\\n';\n");
        puts("    os << \"end \" << endTime << '\\n';\n");
        puts("    for (const Signal& signal : signals()) {\n");
        puts("        const char* first = m_counts[signal.id] ? nullptr : \"-\";\n");
        puts("        if (std::string{signal.kind} == \"probe\") {\n");
        puts("            os << \"probe \" << signal.id << ' ' << m_counts[signal.id] << ' ';\n");
        puts("            if (first) os << first << ' ' << first; else os << m_firstTimes[signal.id] << ' ' << m_lastTimes[signal.id];\n");
        puts("            os << '\\n';\n        } else {\n");
        puts("            if (m_activeAtInjection[signal.id] >= 0) os << \"active \" << signal.id << ' ' << m_activeAtInjection[signal.id] << '\\n';\n");
        puts("            os << \"checker \" << signal.id << ' ' << m_counts[signal.id] << ' ';\n");
        puts("            if (first) os << first << ' ' << first; else os << m_firstTimes[signal.id] << ' ' << m_lastTimes[signal.id];\n");
        puts("            os << '\\n';\n        }\n    }\n");
        puts("    for (const ProbeEvent& event : m_probeEvents) os << \"deviation \" << event.id << ' ' << event.time << ' ' << event.expected << ' ' << event.actual << '\\n';\n");
        puts("    for (const CheckerEvent& event : m_checkerEvents) os << \"trigger \" << event.id << ' ' << event.time << ' ' << event.value << '\\n';\n");
        puts("    return static_cast<bool>(os);\n}\n");
        closeOutputFile();
    }

    void emitManifest() const {
        V3OutJsonFile of{v3Global.opt.makeDir() + "/probe_points.json"};
        of.put("version", 1);
        of.put("top_module", AstNode::prettyName(v3Global.opt.topModule()));
        of.put("probe_config", v3Global.opt.probeConfig());
        of.begin("probes", '[');
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const SignalTarget& target = m_targets[id];
            if (target.m_spec.m_kind != SignalKind::PROBE) continue;
            of.begin();
            of.put("id", static_cast<int>(id));
            of.put("name", target.m_spec.m_name);
            of.put("path", target.m_spec.m_path);
            of.put("mode", modeName(target.m_spec.m_mode));
            of.put("log_limit", static_cast<int>(target.m_spec.m_logLimit));
            of.put("width", target.m_width);
            of.put("source", target.m_source);
            of.end();
        }
        of.end();
        of.begin("checkers", '[');
        for (size_t id = 0; id < m_targets.size(); ++id) {
            const SignalTarget& target = m_targets[id];
            if (target.m_spec.m_kind != SignalKind::CHECKER) continue;
            of.begin();
            of.put("id", static_cast<int>(id));
            of.put("name", target.m_spec.m_name);
            of.put("path", target.m_spec.m_path);
            of.put("condition", target.m_spec.m_conditionText);
            of.put("log_limit", static_cast<int>(target.m_spec.m_logLimit));
            of.put("width", target.m_width);
            of.put("source", target.m_source);
            of.end();
        }
        of.end();
        of.begin("assessments", '[');
        for (const AssessmentSpec& assessment : probeConfig().m_assessments) {
            of.begin();
            of.put("name", assessment.m_name);
            of.putList("probes", assessment.m_probes);
            of.putList("checkers", assessment.m_checkers);
            of.put("detector_policy", assessment.m_detectorPolicy);
            if (!assessment.m_maxDelay.empty()) of.put("max_delay", assessment.m_maxDelay);
            of.put("delay_from", assessment.m_delayFrom);
            of.end();
        }
        of.end();
    }

public:
    explicit EmitCProbeApi(AstNodeModule* topModulep)
        : m_topModulep{topModulep}
        , m_specs{probeConfig().m_signals} {
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
