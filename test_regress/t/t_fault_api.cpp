// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Fault API generation and direct state mutation
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "Vt_fault_api.h"
#include "Vt_fault_api__fault_api.h"
#include "Vt_fault_api___024root.h"
#include "verilated.h"

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<Vt_fault_api> topp{new Vt_fault_api{contextp.get()}};
    Vt_fault_apiFaultApi faults{topp.get()};

    for (const auto& target : faults.targets()) {
        if (std::string{target.name}.find("t.u_dut.") != 0) return 1;
        if (std::string{target.name}.find("tb_only") != std::string::npos) return 2;
    }

    const auto id = faults.lookup("t.u_dut.state_q");
    if (id == Vt_fault_apiFaultApi::invalidTarget) return 3;
    if (std::string{faults.targets().at(id).site_kind} != "unknown") return 27;
    if (faults.lookup("t.tb_only") != Vt_fault_apiFaultApi::invalidTarget) return 4;
    if (faults.flip(id, 8)) return 5;

    const CData before = topp->rootp->t__DOT__u_dut__DOT__state_q;
    if (!faults.flip(id, 2)) return 6;
    if (topp->rootp->t__DOT__u_dut__DOT__state_q != (before ^ 4U)) return 7;
    if (faults.set(id, 8, true)) return 8;
    if (!faults.set(id, 2, false)) return 9;
    if (topp->rootp->t__DOT__u_dut__DOT__state_q & 4U) return 10;
    if (!faults.set(id, 2, true)) return 11;
    if ((topp->rootp->t__DOT__u_dut__DOT__state_q & 4U) == 0) return 12;

    const auto comb_id = faults.lookup("t.u_dut.internal_d");
    if (comb_id == Vt_fault_apiFaultApi::invalidTarget) return 13;
    if (std::string{faults.targets().at(comb_id).site_kind} != "combinational") return 28;
    topp->source = 0;
    topp->eval();
    if (topp->observed != 0) return 14;
    if (!faults.force(comb_id, 1, true)) return 15;
    topp->eval();
    if (topp->observed != 2) return 16;
    topp->source = 0xf;
    topp->eval();
    if (topp->observed != 0xf) return 17;
    if (!faults.force(comb_id, 1, false)) return 18;
    topp->eval();
    if (topp->observed != 0xd) return 19;
    if (!faults.release(comb_id, 1)) return 20;
    topp->eval();
    if (topp->observed != 0xf) return 21;

    const auto array_id = faults.lookup("t.u_dut.bank[1]");
    if (array_id == Vt_fault_apiFaultApi::invalidTarget) return 22;
    if (std::string{faults.targets().at(array_id).site_kind} != "unknown") return 29;
    topp->bank_index = 1;
    if (!faults.force(array_id, 2, true)) return 23;
    topp->eval();
    if (topp->bank_observed != 4) return 24;
    if (topp->bank_dynamic_observed != 4) return 26;
    if (!faults.release(array_id, 2)) return 25;

    std::printf("*-* All Finished *-*\n");
    return 0;
}
