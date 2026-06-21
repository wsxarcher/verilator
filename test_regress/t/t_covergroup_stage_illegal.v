// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t;
  bit [1:0] data;

  covergroup cg;
    cp: coverpoint data {bins known = {0}; illegal_bins bad = default;}
  endgroup

  cg cg_inst = new;

  initial begin
    data = 1;
    cg_inst.sample();
    $write("%%Error: illegal_bins default did not fire\n");
    $finish;
  end
endmodule
