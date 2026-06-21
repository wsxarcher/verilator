// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain
// SPDX-FileCopyrightText: 2026 Matthew Ballance
// SPDX-License-Identifier: CC0-1.0

// Test array bins - separate bin per value, including range expressions

// verilog_format: off
`define stop $stop
`define checkr(gotv,expv) do if ((gotv) != (expv)) begin $write("%%Error: %s:%0d:  got=%f exp=%f\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
// verilog_format: on

module t;
  bit [7:0] data;
  bit [1:0] nib;
  logic signed [2:0] sval;
  bit [63:0] u64;
  longint s64;

  covergroup cg;
    coverpoint data {
      // Array bins: creates 3 separate bins
      bins values[] = {1, 5, 9};

      // Non-array bin: creates 1 bin covering all values
      bins grouped = {2, 6, 10};
    }
  endgroup

  // cg2: array bins using a range expression - one bin per value in the range
  covergroup cg2;
    cp: coverpoint data {
      bins range_arr[] = {[0 : 3]};  // range expression: creates 4 separate bins
    }
  endgroup

  // cg3: fixed-size array bins with one value per non-empty bin.
  covergroup cg3;
    cp: coverpoint data {bins range_sized[4] = {[4 : 7]};}
  endgroup

  // cg4: fixed-size array bins with an open-ended upper '$' (issue reproducer).
  // '$' resolves to the coverpoint domain max (3 for a 2-bit value), so [0:$]
  // expands to [0:3] -> 4 values distributed one per bin.
  covergroup cg4;
    cp: coverpoint nib {bins hi_open[4] = {[0 : $]};}
  endgroup

  // cg5: open-ended lower '$' in array bins. '$' resolves to the domain min (0),
  // so [$:1] expands to [0:1] -> 2 bins.
  covergroup cg5;
    cp: coverpoint nib {bins lo_open[] = {[$ : 1]};}
  endgroup

  // cg6: open-ended upper '$' on a signed coverpoint. '$' is the max signed value
  // (3 for a 3-bit signed value), so [0:$] expands to [0:3] -> 4 bins.
  covergroup cg6;
    cp: coverpoint sval {bins pos_open[] = {[0 : $]};}
  endgroup

  // cg7: open-ended lower '$' on a signed coverpoint. '$' is the min signed value
  // (-4 for a 3-bit signed value), so [$:-1] expands to [-4:-1] -> 4 bins.
  covergroup cg7;
    cp: coverpoint sval {bins neg_open[] = {[$ : -1]};}
  endgroup

  // cg8: 64-bit unsigned coverpoint, '$' at both domain limits.  Exercises the
  // full-width unsigned max (2**64-1).  ulo {[$:1]} -> [0:1] (lower limit 0);
  // uhi {[2**64-2:$]} -> [2**64-2:2**64-1] (upper limit).
  covergroup cg8;
    cp: coverpoint u64 {bins ulo[] = {[$ : 1]}; bins uhi[] = {[64'hFFFF_FFFF_FFFF_FFFE : $]};}
  endgroup

  // cg9: 64-bit signed coverpoint, '$' at both domain limits.  Exercises the full-width
  // signed min (-2**63) and max (2**63-1).  slo {[$:-2**63+1]} -> [-2**63:-2**63+1]
  // (lower limit); shi {[2**63-2:$]} -> [2**63-2:2**63-1] (upper limit).
  covergroup cg9;
    cp: coverpoint s64 {
      bins slo[] = {[$ : 64'sh8000_0000_0000_0001]}; bins shi[] = {[64'sh7FFF_FFFF_FFFF_FFFE : $]};
    }
  endgroup

  // cg_fixed_dist: fixed-size bins distribute values in source order.
  covergroup cg_fixed_dist;
    cp: coverpoint data {bins fixed[4] = {[1 : 10], 1, 4, 7};}
  endgroup

  // cg_intersect_u: explicit negative lower bound intersects with the unsigned
  // coverpoint domain, so [-1:$] becomes [0:3].
  covergroup cg_intersect_u;
    cp: coverpoint nib {bins unsigned_intersect[] = {[-1 : $]};}
  endgroup

  // cg_both: '$' on both sides covers the full coverpoint domain.
  covergroup cg_both;
    cp: coverpoint nib {bins both_open[] = {[$ : $]};}
  endgroup

  // cg_intersect_s: values above signed 3-bit max are removed, so [0:10]
  // becomes [0:3].
  covergroup cg_intersect_s;
    cp: coverpoint sval {bins signed_intersect[] = {[0 : 10]};}
  endgroup

  // cg_fixed_empty: a fixed-size array bin with more bins than values creates surplus
  // empty bins (IEEE 1800-2023 19.5.1), but empty bins do not contribute to
  // coverage (19.11.1).
  covergroup cg_fixed_empty;
    cp: coverpoint data {bins fe[5] = {1, 4, 7};}
  endgroup

  // cg_typed: a declared coverpoint type narrower than the expression sets the
  // effective type (IEEE 1800-2023 19.5.7(a)), so '$' is the declared 3-bit max
  // (7), not the 8-bit expression max.  [4:$] -> [4:7] = 4 bins, not 252.
  covergroup cg_typed;
    bit [2:0] tcp: coverpoint data {bins thi[] = {[4 : $]};}
  endgroup

  initial begin
    cg cg_inst;
    cg2 cg2_inst;
    cg3 cg3_inst;
    cg4 cg4_inst;
    cg5 cg5_inst;
    cg6 cg6_inst;
    cg7 cg7_inst;
    cg8 cg8_inst;
    cg9 cg9_inst;
    cg_fixed_dist cg_fixed_dist_inst;
    cg_intersect_u cg_intersect_u_inst;
    cg_both cg_both_inst;
    cg_intersect_s cg_intersect_s_inst;
    cg_fixed_empty cg_fixed_empty_inst;
    cg_typed cg_typed_inst;

    cg_inst = new();
    cg2_inst = new();
    cg3_inst = new();
    cg4_inst = new();
    cg5_inst = new();
    cg6_inst = new();
    cg7_inst = new();
    cg8_inst = new();
    cg9_inst = new();
    cg_fixed_dist_inst = new();
    cg_intersect_u_inst = new();
    cg_both_inst = new();
    cg_intersect_s_inst = new();
    cg_fixed_empty_inst = new();
    cg_typed_inst = new();

    // Hit first array bin value (1)
    data = 1;
    cg_inst.sample();
    `checkr(cg_inst.get_inst_coverage(), 25.0);

    // Hit second array bin value (5)
    data = 5;
    cg_inst.sample();
    `checkr(cg_inst.get_inst_coverage(), 50.0);

    // Hit the grouped bin (covers all of 2, 6, 10)
    data = 6;
    cg_inst.sample();
    `checkr(cg_inst.get_inst_coverage(), 75.0);

    // Hit third array bin value (9)
    data = 9;
    cg_inst.sample();
    `checkr(cg_inst.get_inst_coverage(), 100.0);

    // Verify hitting other values in grouped bin doesn't increase coverage
    data = 2;
    cg_inst.sample();
    `checkr(cg_inst.get_inst_coverage(), 100.0);

    // Hit range_arr bins ([0:3])
    data = 0;
    cg2_inst.sample();
    `checkr(cg2_inst.get_inst_coverage(), 25.0);
    data = 1;
    cg2_inst.sample();
    `checkr(cg2_inst.get_inst_coverage(), 50.0);
    data = 2;
    cg2_inst.sample();
    `checkr(cg2_inst.get_inst_coverage(), 75.0);

    // Hit range_sized bins ([4:7])
    data = 4;
    cg3_inst.sample();
    `checkr(cg3_inst.get_inst_coverage(), 25.0);
    data = 5;
    cg3_inst.sample();
    `checkr(cg3_inst.get_inst_coverage(), 50.0);
    data = 6;
    cg3_inst.sample();
    `checkr(cg3_inst.get_inst_coverage(), 75.0);

    // cg4: open-ended upper '$' -> [0:3], one bin per value (4 bins)
    nib = 0;
    cg4_inst.sample();
    `checkr(cg4_inst.get_inst_coverage(), 25.0);
    nib = 1;
    cg4_inst.sample();
    `checkr(cg4_inst.get_inst_coverage(), 50.0);
    nib = 2;
    cg4_inst.sample();
    `checkr(cg4_inst.get_inst_coverage(), 75.0);
    nib = 3;
    cg4_inst.sample();
    `checkr(cg4_inst.get_inst_coverage(), 100.0);

    // cg5: open-ended lower '$' -> [0:1] (2 bins); values above 1 are not binned
    nib = 0;
    cg5_inst.sample();
    `checkr(cg5_inst.get_inst_coverage(), 50.0);
    nib = 1;
    cg5_inst.sample();
    `checkr(cg5_inst.get_inst_coverage(), 100.0);
    nib = 3;
    cg5_inst.sample();
    `checkr(cg5_inst.get_inst_coverage(), 100.0);

    // cg6: signed coverpoint, open-ended upper '$' -> [0:3] (4 bins)
    sval = 0;
    cg6_inst.sample();
    `checkr(cg6_inst.get_inst_coverage(), 25.0);
    sval = 1;
    cg6_inst.sample();
    `checkr(cg6_inst.get_inst_coverage(), 50.0);
    sval = 2;
    cg6_inst.sample();
    `checkr(cg6_inst.get_inst_coverage(), 75.0);
    sval = 3;
    cg6_inst.sample();
    `checkr(cg6_inst.get_inst_coverage(), 100.0);

    // cg7: signed coverpoint, open-ended lower '$' -> [-4:-1] (4 bins)
    sval = -4;
    cg7_inst.sample();
    `checkr(cg7_inst.get_inst_coverage(), 25.0);
    sval = -3;
    cg7_inst.sample();
    `checkr(cg7_inst.get_inst_coverage(), 50.0);
    sval = -2;
    cg7_inst.sample();
    `checkr(cg7_inst.get_inst_coverage(), 75.0);
    sval = -1;
    cg7_inst.sample();
    `checkr(cg7_inst.get_inst_coverage(), 100.0);

    // cg8: 64-bit unsigned '$' limits -> ulo[0:1] (lower) + uhi[2**64-2:2**64-1] (upper)
    u64 = 0;
    cg8_inst.sample();
    `checkr(cg8_inst.get_inst_coverage(), 25.0);
    u64 = 1;
    cg8_inst.sample();
    `checkr(cg8_inst.get_inst_coverage(), 50.0);
    u64 = 64'hFFFF_FFFF_FFFF_FFFE;
    cg8_inst.sample();
    `checkr(cg8_inst.get_inst_coverage(), 75.0);
    u64 = 64'hFFFF_FFFF_FFFF_FFFF;
    cg8_inst.sample();
    `checkr(cg8_inst.get_inst_coverage(), 100.0);

    // cg9: 64-bit signed '$' limits -> slo[-2**63:-2**63+1] (lower) + shi[2**63-2:2**63-1] (upper)
    s64 = 64'sh8000_0000_0000_0000;
    cg9_inst.sample();
    `checkr(cg9_inst.get_inst_coverage(), 25.0);
    s64 = 64'sh8000_0000_0000_0001;
    cg9_inst.sample();
    `checkr(cg9_inst.get_inst_coverage(), 50.0);
    s64 = 64'sh7FFF_FFFF_FFFF_FFFE;
    cg9_inst.sample();
    `checkr(cg9_inst.get_inst_coverage(), 75.0);
    s64 = 64'sh7FFF_FFFF_FFFF_FFFF;
    cg9_inst.sample();
    `checkr(cg9_inst.get_inst_coverage(), 100.0);

    // cg_fixed_dist: fixed bins distribute 13 values as
    // <1,2,3>, <4,5,6>, <7,8,9>, <10,1,4,7>
    data = 2;
    cg_fixed_dist_inst.sample();
    `checkr(cg_fixed_dist_inst.get_inst_coverage(), 25.0);
    data = 5;
    cg_fixed_dist_inst.sample();
    `checkr(cg_fixed_dist_inst.get_inst_coverage(), 50.0);
    data = 8;
    cg_fixed_dist_inst.sample();
    `checkr(cg_fixed_dist_inst.get_inst_coverage(), 75.0);
    data = 10;
    cg_fixed_dist_inst.sample();
    `checkr(cg_fixed_dist_inst.get_inst_coverage(), 100.0);

    // cg_intersect_u: [-1:$] intersects with unsigned 2-bit [0:3]
    nib = 0;
    cg_intersect_u_inst.sample();
    `checkr(cg_intersect_u_inst.get_inst_coverage(), 25.0);
    nib = 1;
    cg_intersect_u_inst.sample();
    `checkr(cg_intersect_u_inst.get_inst_coverage(), 50.0);
    nib = 2;
    cg_intersect_u_inst.sample();
    `checkr(cg_intersect_u_inst.get_inst_coverage(), 75.0);
    nib = 3;
    cg_intersect_u_inst.sample();
    `checkr(cg_intersect_u_inst.get_inst_coverage(), 100.0);

    // cg_both: [$:$] covers the complete 2-bit domain
    nib = 0;
    cg_both_inst.sample();
    `checkr(cg_both_inst.get_inst_coverage(), 25.0);
    nib = 1;
    cg_both_inst.sample();
    `checkr(cg_both_inst.get_inst_coverage(), 50.0);
    nib = 2;
    cg_both_inst.sample();
    `checkr(cg_both_inst.get_inst_coverage(), 75.0);
    nib = 3;
    cg_both_inst.sample();
    `checkr(cg_both_inst.get_inst_coverage(), 100.0);

    // cg_intersect_s: [0:10] intersects with signed 3-bit [-4:3] as [0:3]
    sval = 0;
    cg_intersect_s_inst.sample();
    `checkr(cg_intersect_s_inst.get_inst_coverage(), 25.0);
    sval = 1;
    cg_intersect_s_inst.sample();
    `checkr(cg_intersect_s_inst.get_inst_coverage(), 50.0);
    sval = 2;
    cg_intersect_s_inst.sample();
    `checkr(cg_intersect_s_inst.get_inst_coverage(), 75.0);
    sval = 3;
    cg_intersect_s_inst.sample();
    `checkr(cg_intersect_s_inst.get_inst_coverage(), 100.0);
    sval = -1;
    cg_intersect_s_inst.sample();
    `checkr(cg_intersect_s_inst.get_inst_coverage(), 100.0);

    // cg_fixed_empty: fe[5] = {1,4,7} -> <1>,<4>,<7>,<>,<>; the two empty bins
    // are excluded from coverage, so the three non-empty bins can reach 100%.
    data = 1;
    cg_fixed_empty_inst.sample();
    `checkr(cg_fixed_empty_inst.get_inst_coverage(), 100.0 / 3.0);
    data = 4;
    cg_fixed_empty_inst.sample();
    `checkr(cg_fixed_empty_inst.get_inst_coverage(), 200.0 / 3.0);
    data = 7;
    cg_fixed_empty_inst.sample();
    `checkr(cg_fixed_empty_inst.get_inst_coverage(), 100.0);

    // cg_typed: thi[] = {[4:$]} on a bit[2:0] coverpoint -> [4:7] = 4 bins.
    data = 4;
    cg_typed_inst.sample();
    `checkr(cg_typed_inst.get_inst_coverage(), 25.0);
    data = 5;
    cg_typed_inst.sample();
    `checkr(cg_typed_inst.get_inst_coverage(), 50.0);
    data = 6;
    cg_typed_inst.sample();
    `checkr(cg_typed_inst.get_inst_coverage(), 75.0);
    data = 7;
    cg_typed_inst.sample();
    `checkr(cg_typed_inst.get_inst_coverage(), 100.0);

    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
