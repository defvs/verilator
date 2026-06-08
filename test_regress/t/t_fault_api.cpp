// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Fault API generation and direct state mutation
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "Vt.h"
#include "Vt__fault_api.h"
#include "Vt___024root.h"
#include "verilated.h"

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<Vt> topp{new Vt{contextp.get()}};
    VtFaultApi faults{topp.get()};

    for (const auto& target : faults.targets()) {
        if (std::string{target.name}.find("t.u_dut.") != 0) return 1;
        if (std::string{target.name}.find("tb_only") != std::string::npos) return 2;
    }

    const auto id = faults.lookup("t.u_dut.state_q");
    if (id == VtFaultApi::invalidTarget) return 3;
    if (faults.lookup("t.tb_only") != VtFaultApi::invalidTarget) return 4;
    if (faults.flip(id, 8)) return 5;

    const CData before = topp->rootp->t__DOT__u_dut__DOT__state_q;
    if (!faults.flip(id, 2)) return 6;
    if (topp->rootp->t__DOT__u_dut__DOT__state_q != (before ^ 4U)) return 7;

    std::printf("*-* All Finished *-*\n");
    return 0;
}
