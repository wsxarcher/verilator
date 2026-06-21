// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// Runtime covergroup coverage: staging edge cases, bin set value expressions,
// '$'/open array bins, and wide (>64-bit) coverpoints (IEEE 1800-2023 19.5).

// verilog_format: off
`define checkr(gotv, expv) do if ((gotv) != (expv)) begin $write("%%Error: %s:%0d: got=%f exp=%f\n", `__FILE__, `__LINE__, (gotv), (expv)); errors++; end while (0);
// verilog_format: on

module t;
  int errors;
  bit en;
  bit [7:0] data8;
  bit [3:0] data4;
  bit [2:0] state;
  bit [1:0] u2;
  logic signed [2:0] s3;
  bit [1:0] a2;
  bit [1:0] b2;
  logic [3:0] be4;
  bit [1:0] dl2;
  bit [65:0] w;

  covergroup cg_empty_fixed;
    cp: coverpoint data8 {bins fe[5] = {1, 4, 7};}
  endgroup

  covergroup cg_zero_denom;
    cp: coverpoint u2 {bins dropped[] = {8};}
  endgroup

  covergroup cg_default_arr;
    cp: coverpoint u2 {
      bins known = {0};
      bins others[] = default;
    }
  endgroup

  covergroup cg_wild_arr;
    cp: coverpoint data4 {wildcard bins w[] = {4'b11??};}
  endgroup

  covergroup cg_trans_arr;
    cp: coverpoint state {bins ta[] = (0, 1 => 2, 3);}
  endgroup

  covergroup cg_cross_arr;
    cp_a: coverpoint a2 {bins av[] = {0, 1};}
    cp_b: coverpoint b2 {bins bv[] = {0, 1, 2};}
    cr: cross cp_a, cp_b;
  endgroup

  covergroup cg_ignore_overlap;
    cp: coverpoint u2 {
      bins keep = {0};
      bins drop = {1};
      ignore_bins ign = {1};
    }
  endgroup

  covergroup cg_default_resolve;
    cp: coverpoint s3 {
      bins arr[] = {[0 : 10]};
      bins def = default;
    }
  endgroup

  covergroup cg_trans_iff;
    cp: coverpoint state {bins t = (1 => 2) iff (en);}
  endgroup

  covergroup cg_trans3_iff;
    cp: coverpoint state {bins t = (3 => 4 => 5) iff (en);}
  endgroup

  covergroup cg_with_iff;
    cp: coverpoint data4 {bins wb = {[0 : 3]} with (item >= 0) iff (en);}
  endgroup

  covergroup cg_default_iff;
    cp: coverpoint u2 {
      bins known = {0};
      bins def = default iff (en);
    }
    cp2: coverpoint a2 {bins z = {0};}
    cr: cross cp, cp2;
  endgroup

  covergroup cg_implicit_range;
    [2:0] cp: coverpoint data8 {bins hi[] = {[4 : $]};}
  endgroup

  covergroup cg_var_implicit;
    var [2:0] cp: coverpoint data8 {bins hi[] = {[4 : $]};}
  endgroup

  covergroup cg_signed_cast;
    cp: coverpoint s3 {bins neg_one[] = {3'b111};}
  endgroup

  covergroup cg_nonarray_resolve;
    cp: coverpoint u2 {
      bins ok = {1};
      bins hi = {8};
    }
  endgroup

  // Bin set value expressions (constant and range): {0}, {1<<0}, {[2:3]}.
  // Unlabeled coverpoint -> bin hierarchy uses the sampled variable name.
  covergroup cg_bin_expr;
    coverpoint be4 {bins zero = {0}; bins one = {1 << 0}; bins low = {[2 : 3]};}
  endgroup

  // A '$' array-bins size parses as an open '[]' array (one bin per value),
  // identical to an explicit '[]' (IEEE 1800-2023 19.5.1).
  covergroup cg_dollar;
    cp: coverpoint dl2 {bins ds[$] = {0, 1, 2};}
  endgroup
  covergroup cg_open;
    cp: coverpoint dl2 {bins ds[] = {0, 1, 2};}
  endgroup

  // Wide (>64-bit) coverpoint: open-array range, with out-of-domain bins dropped.
  covergroup cg_wide;
    cp: coverpoint w {
      bins lo[] = {[66'd0 : 66'd1]};
      bins neg[] = {-1};
      bins high[] = {67'h4_0000_0000_0000_0000};
    }
  endgroup

  cg_empty_fixed empty_fixed = new;
  cg_zero_denom zero_denom = new;
  cg_default_arr default_arr = new;
  cg_wild_arr wild_arr = new;
  cg_trans_arr trans_arr = new;
  cg_cross_arr cross_arr = new;
  cg_ignore_overlap ignore_overlap = new;
  cg_default_resolve default_resolve = new;
  cg_trans_iff trans_iff = new;
  cg_trans3_iff trans3_iff = new;
  cg_with_iff with_iff = new;
  cg_default_iff default_iff = new;
  cg_implicit_range implicit_range = new;
  cg_var_implicit var_implicit = new;
  cg_signed_cast signed_cast = new;
  cg_nonarray_resolve nonarray_resolve = new;
  cg_bin_expr be_inst = new;
  cg_dollar cgd_inst = new;
  cg_open cgo_inst = new;
  cg_wide wide_inst = new;

  initial begin
    data8 = 1; empty_fixed.sample();
    data8 = 4; empty_fixed.sample();
    data8 = 7; empty_fixed.sample();
    `checkr(empty_fixed.get_inst_coverage(), 100.0);

    u2 = 0; zero_denom.sample();
    `checkr(zero_denom.get_inst_coverage(), 0.0);

    u2 = 0; default_arr.sample();
    u2 = 1; default_arr.sample();
    u2 = 2; default_arr.sample();
    u2 = 3; default_arr.sample();
    `checkr(default_arr.get_inst_coverage(), 100.0);

    data4 = 12; wild_arr.sample();
    data4 = 13; wild_arr.sample();
    data4 = 14; wild_arr.sample();
    data4 = 15; wild_arr.sample();
    `checkr(wild_arr.get_inst_coverage(), 100.0);

    state = 0; trans_arr.sample();
    state = 2; trans_arr.sample();
    `checkr(trans_arr.get_inst_coverage(), 25.0);
    state = 0; trans_arr.sample();
    state = 3; trans_arr.sample();
    `checkr(trans_arr.get_inst_coverage(), 50.0);
    state = 1; trans_arr.sample();
    state = 2; trans_arr.sample();
    `checkr(trans_arr.get_inst_coverage(), 75.0);
    state = 1; trans_arr.sample();
    state = 3; trans_arr.sample();
    `checkr(trans_arr.get_inst_coverage(), 100.0);

    for (int ai = 0; ai < 2; ++ai) begin
      for (int bi = 0; bi < 3; ++bi) begin
        a2 = ai[1:0];
        b2 = bi[1:0];
        cross_arr.sample();
      end
    end

    u2 = 1; ignore_overlap.sample();
    `checkr(ignore_overlap.get_inst_coverage(), 0.0);
    u2 = 0; ignore_overlap.sample();
    `checkr(ignore_overlap.get_inst_coverage(), 100.0);

    s3 = 3; default_resolve.sample();
    s3 = -1; default_resolve.sample();

    en = 0;
    state = 1; trans_iff.sample();
    state = 2; trans_iff.sample();
    `checkr(trans_iff.get_inst_coverage(), 0.0);
    en = 1;
    state = 1; trans_iff.sample();
    state = 2; trans_iff.sample();
    `checkr(trans_iff.get_inst_coverage(), 100.0);

    en = 0;
    state = 3; trans3_iff.sample();
    state = 4; trans3_iff.sample();
    state = 5; trans3_iff.sample();
    `checkr(trans3_iff.get_inst_coverage(), 0.0);
    en = 1;
    state = 3; trans3_iff.sample();
    state = 4; trans3_iff.sample();
    state = 5; trans3_iff.sample();
    `checkr(trans3_iff.get_inst_coverage(), 100.0);

    en = 0;
    data4 = 1; with_iff.sample();
    `checkr(with_iff.get_inst_coverage(), 0.0);
    en = 1;
    data4 = 1; with_iff.sample();
    `checkr(with_iff.get_inst_coverage(), 100.0);

    en = 0;
    a2 = 0;
    u2 = 1; default_iff.sample();
    en = 1;
    u2 = 1; default_iff.sample();

    data8 = 4; implicit_range.sample();
    data8 = 5; implicit_range.sample();
    data8 = 6; implicit_range.sample();
    data8 = 7; implicit_range.sample();
    `checkr(implicit_range.get_inst_coverage(), 100.0);

    data8 = 4; var_implicit.sample();
    data8 = 5; var_implicit.sample();
    data8 = 6; var_implicit.sample();
    data8 = 7; var_implicit.sample();
    `checkr(var_implicit.get_inst_coverage(), 100.0);

    s3 = -1; signed_cast.sample();
    `checkr(signed_cast.get_inst_coverage(), 100.0);

    u2 = 1; nonarray_resolve.sample();
    `checkr(nonarray_resolve.get_inst_coverage(), 100.0);

    be4 = 0; be_inst.sample();
    be4 = 1; be_inst.sample();
    be4 = 2; be_inst.sample();
    `checkr(be_inst.get_inst_coverage(), 100.0);
    be4 = 3; be_inst.sample();
    `checkr(be_inst.get_inst_coverage(), 100.0);

    dl2 = 0; cgd_inst.sample(); cgo_inst.sample();
    dl2 = 1; cgd_inst.sample(); cgo_inst.sample();
    dl2 = 2; cgd_inst.sample(); cgo_inst.sample();
    `checkr(cgd_inst.get_inst_coverage(), cgo_inst.get_inst_coverage());
    `checkr(cgd_inst.get_inst_coverage(), 100.0);

    w = 0; wide_inst.sample();
    `checkr(wide_inst.get_inst_coverage(), 50.0);
    w = 1; wide_inst.sample();
    `checkr(wide_inst.get_inst_coverage(), 100.0);

    if (errors != 0) begin
      $write("%%Error: t/t_covergroup_stage_rt.v: saw %0d failures\n", errors);
      $stop;
    end
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
