// DESCRIPTION: Verilator: Fault API generation and direct state mutation
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module dut;
   logic [7:0] state_q;
   logic [3:0] internal_d;
endmodule

module t;
   logic tb_only;
   dut u_dut();
endmodule
