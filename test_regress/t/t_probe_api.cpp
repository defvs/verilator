// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Probe API golden capture and comparison
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "Vt_probe_api.h"
#include "Vt_probe_api__probe_api.h"
#include "Vt_probe_api___024root.h"
#include "verilated.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

std::string goldenPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string prefix = "+golden=";
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "probe_api.golden";
}

void setInitial(Vt_probe_api___024root* rootp) {
    rootp->t__DOT__u_dut__DOT__scalar = 0;
    rootp->t__DOT__u_dut__DOT__data_bus = 0x12U;
    rootp->t__DOT__u_dut__DOT__reg_bank[2] = 0x34U;
    rootp->t__DOT__u_dut__DOT__wide_bus[0] = 0x89abcdefU;
    rootp->t__DOT__u_dut__DOT__wide_bus[1] = 0x01234567U;
    rootp->t__DOT__u_dut__DOT__wide_bus[2] = 0xfedcba98U;
    rootp->t__DOT__u_dut__DOT__error_flag = 0;
}

void setChanged(Vt_probe_api___024root* rootp) {
    rootp->t__DOT__u_dut__DOT__scalar = 1;
    rootp->t__DOT__u_dut__DOT__data_bus = 0x5aU;
    rootp->t__DOT__u_dut__DOT__reg_bank[2] = 0x77U;
    rootp->t__DOT__u_dut__DOT__wide_bus[0] = 0x76543210U;
    rootp->t__DOT__u_dut__DOT__wide_bus[1] = 0xfedcba98U;
    rootp->t__DOT__u_dut__DOT__wide_bus[2] = 0x01234567U;
}

}  // namespace

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<Vt_probe_api> topp{new Vt_probe_api{contextp.get()}};

    const auto& probeMeta = Vt_probe_apiProbeApi::signals();
    if (probeMeta.size() != 6) return 1;
    if (std::string{probeMeta[0].name} != "scalar") return 2;
    if (std::string{probeMeta[1].name} != "bus") return 3;
    if (std::string{probeMeta[2].name} != "bit3") return 4;
    if (std::string{probeMeta[3].name} != "elem") return 5;
    if (std::string{probeMeta[4].name} != "wide") return 6;
    if (probeMeta[2].width != 1) return 7;
    if (probeMeta[4].width != 96) return 8;
    if (std::string{probeMeta[5].kind} != "checker") return 24;

    const std::string path = goldenPath(argc, argv);
    Vt_probe_apiProbeApi capture{topp.get()};
    setInitial(topp->rootp);
    if (!capture.capture(0)) return 9;
    setChanged(topp->rootp);
    if (!capture.capture(10)) return 10;
    if (!capture.writeGolden(path)) return 11;

    Vt_probe_apiProbeApi compare{topp.get()};
    if (!compare.loadGolden(path)) return 12;
    setInitial(topp->rootp);
    if (!compare.sample(0)) return 13;
    setChanged(topp->rootp);
    if (!compare.sample(10)) return 14;
    if (!compare.finalCheck(10)) return 15;

    Vt_probe_apiProbeApi mismatch{topp.get()};
    if (!mismatch.loadGolden(path)) return 17;
    setInitial(topp->rootp);
    if (!mismatch.sample(0)) return 18;
    if (!mismatch.beginInjection(0)) return 25;
    setChanged(topp->rootp);
    topp->rootp->t__DOT__u_dut__DOT__reg_bank[2] ^= 1U;
    topp->rootp->t__DOT__u_dut__DOT__error_flag = 1;
    if (!mismatch.sample(10)) return 19;
    if (!mismatch.finalCheck(10)) return 20;
    if (!mismatch.writeResults(path + ".results", 10)) return 26;

    Vt_probe_apiProbeApi unread{topp.get()};
    if (!unread.loadGolden(path)) return 21;
    setInitial(topp->rootp);
    if (!unread.sample(0)) return 22;
    if (!unread.beginInjection(0)) return 27;
    if (!unread.finalCheck(0)) return 23;

    std::printf("*-* All Finished *-*\n");
    return 0;
}
