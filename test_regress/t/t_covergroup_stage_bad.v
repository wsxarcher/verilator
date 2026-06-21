// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// Covergroup array-bins diagnostics gathered into one test: unsupported
// array-bin sizes/widths (errors) and out-of-domain bin values that are dropped
// or clamped (COVERIGN warnings).  See IEEE 1800-2023 19.5.1 (specifying bins)
// and 19.5.7 (value resolution).

module t;
  bit [1:0] data;
  bit [16:0] data17;
  bit [64:0] data65;
  bit [1:0] u;  // unsigned domain 0..3
  logic signed [2:0] s;  // signed domain -4..3

  // Errors: array-bins size/width limits (IEEE 1800-2023 19.5.1).  A fixed bins
  // count must be a positive two-state constant and is capped; open '[]'/'$'/
  // wildcard/default arrays may not exceed Verilator's per-value expansion limits.
  covergroup cg_fixed;
    cp: coverpoint data {bins huge_size[64'd4294967296] = {1};}
  endgroup
  covergroup cg_neg;
    cp: coverpoint data {bins neg_size[-1] = {1};}
  endgroup
  covergroup cg_too_many;
    cp: coverpoint data17 {bins all17[] = {[0 : $]};}
  endgroup
  covergroup cg_dollar_wide;
    cp: coverpoint data65 {bins all65[] = {[$ : $]};}
  endgroup
  covergroup cg_wildcard_wide;
    cp: coverpoint data65 {wildcard bins wb[] = {65'b1z};}
  endgroup
  covergroup cg_default_wide;
    cp: coverpoint data17 {bins others[] = default;}
  endgroup

  // Warnings: values outside the coverpoint domain (IEEE 1800-2023 19.5.7).
  // Out-of-domain singletons (negative on unsigned, too high, or x/z) drop the
  // bin; a partially in-range '[lo:hi]' is clamped; a wholly out-of-range one is
  // dropped.
  covergroup cg_u;
    cp: coverpoint u {
      bins neg_value[] = {-1};
      bins hi_value[] = {20};
      bins xz_value[] = {2'b1x};
      bins hi_range[] = {[0 : 20]};
      bins out_range[] = {[10 : 20]};
      bins neg_range[] = {[-5 : -1]};
    }
  endgroup
  covergroup cg_s;
    cp: coverpoint s {bins lo_value[] = {-9}; bins span_range[] = {[-9 : 9]};}
  endgroup
endmodule
