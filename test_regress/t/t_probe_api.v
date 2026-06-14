// DESCRIPTION: Verilator: Probe API golden capture and comparison
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module dut;
   logic        scalar;
   logic [7:0]  data_bus;
   logic [95:0] wide_bus;
   logic [7:0]  reg_bank [0:3];
   logic        error_flag;
   logic [3:0]  unprobed;
endmodule

module t;
   dut u_dut();
endmodule
