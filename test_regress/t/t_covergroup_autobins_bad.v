// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// Tests for automatic bins error conditions

module t;
  int size_var;
  logic [3:0] cp_expr;


  covergroup cg1;
    cp1: coverpoint cp_expr {
      bins auto[size_var];
    }
  endgroup


  covergroup cg2;
    cp1: coverpoint cp_expr {
      bins auto[0];
    }
  endgroup


  covergroup cg2b;
    cp1: coverpoint cp_expr {
      bins auto[1001];
    }
  endgroup


  covergroup cg3;
    cp1: coverpoint cp_expr {
      bins b[] = {[size_var:size_var]};
      bins b_mixed[] = {[0:size_var]};
      bins b_range = {[size_var:4]};
      bins b_range2 = {[0:size_var]};
      bins b2 = {size_var};
      bins b_ncv[] = {size_var};
      bins b_szc[size_var] = {1};
      bins b_sz0[0] = {1};
      ignore_bins ign = {size_var};
      ignore_bins ign_range = {[0:size_var]};
    }
  endgroup


  covergroup cg4;
    cp1: coverpoint cp_expr {
      option.at_least = size_var;
    }
  endgroup


  covergroup cg5;
    cp1: coverpoint cp_expr {
      bins b_xz = {[4'bxxxx:4'hF]};
      ignore_bins ign_xz_lo = {[4'bxxxx:4'hF]};
      ignore_bins ign_xz_hi = {[4'h0:4'bzzzz]};
      ignore_bins ign_nclo = {[size_var:4]};
      bins b_nc_ub = {[size_var:$]};
      bins b_xz_ub = {[4'bxxxx:$]};
    }
  endgroup

  cg1 cg1_inst = new;
  cg2 cg2_inst = new;
  cg2b cg2b_inst = new;
  cg3 cg3_inst = new;
  cg4 cg4_inst = new;
  cg5 cg5_inst = new;

  initial $finish;
endmodule
