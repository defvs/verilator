// DESCRIPTION: Verilator: Fault API generation and direct state mutation
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module dut (
   input  logic [3:0] source,
   input  logic       bank_index,
   output logic [3:0] observed,
   output logic [2:0] bank_observed,
   output logic [2:0] bank_dynamic_observed
);
   logic [7:0] state_q;
   logic [3:0] internal_d;
   logic [2:0] bank [1:0];
   assign internal_d = source;
   assign observed = internal_d;
   assign bank_observed = bank[1];
   assign bank_dynamic_observed = bank[bank_index];
endmodule

module t (
   input  logic [3:0] source,
   input  logic       bank_index,
   output logic [3:0] observed,
   output logic [2:0] bank_observed,
   output logic [2:0] bank_dynamic_observed
);
   logic tb_only;
   dut u_dut(.source, .bank_index, .observed, .bank_observed, .bank_dynamic_observed);
endmodule
