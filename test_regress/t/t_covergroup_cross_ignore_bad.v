// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t;
  logic [1:0] a;
  logic b;
  logic c;

  covergroup cg;
    cp_a: coverpoint a {
      bins a0 = {0};
      bins a1 = {1};
    }
    cp_b: coverpoint b {bins b0 = {0}; bins b1 = {1};}
    cp_c: coverpoint c {bins c0 = {0}; bins c1 = {1};}
    ab: cross cp_a, cp_b {
      ignore_bins bad_coverpoint = binsof(cp_c);
      ignore_bins bad_bin = binsof(cp_a.missing);
      ignore_bins bad_x_value = binsof(cp_a) intersect {2'bx};
      ignore_bins bad_x_range = binsof(cp_a) intersect {[0 : 2'bx]};
    }
  endgroup

  cg cg_inst = new;
endmodule
