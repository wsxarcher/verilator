// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// Test cross coverage: 2-way, 3-way, and 4-way crosses

// verilog_format: off
`define stop $stop
`define checkr(gotv,expv) do if ((gotv) != (expv)) begin $write("%%Error: %s:%0d:  got=%f exp=%f\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
// verilog_format: on

module t;
  logic [1:0] addr;
  logic cmd;
  logic mode;
  logic parity;
  logic signed [2:0] signed_data;
  logic signed [63:0] wide_signed_data;
  logic signed [64:0] huge_signed_data;
  logic [3:0] wildcard_data;

  typedef struct packed {logic m_p; logic h_mode;} cfg_t;
  cfg_t s_cfg = '0;

  // 2-way cross
  covergroup cg2;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd: cross cp_addr, cp_cmd;
  endgroup

  // 3-way cross
  covergroup cg3;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1}; bins addr2 = {2};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    cp_mode: coverpoint mode {bins normal = {0}; bins debug = {1};}
    addr_cmd_mode: cross cp_addr, cp_cmd, cp_mode;
  endgroup

  // 4-way cross
  covergroup cg4;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    cp_mode: coverpoint mode {bins normal = {0}; bins debug = {1};}
    cp_parity: coverpoint parity {bins even = {0}; bins odd = {1};}
    addr_cmd_mode_parity: cross cp_addr, cp_cmd, cp_mode, cp_parity;
  endgroup

  // Cross with option set inside the cross body
  covergroup cg5;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd_opt: cross cp_addr, cp_cmd{option.weight = 2;}
  endgroup

  // 2-way cross where one coverpoint uses a range bin
  covergroup cg_range;
    cp_addr: coverpoint addr {
      bins lo_range = {[0 : 1]};  // range bin
      bins hi_range = {[2 : 3]};
    }
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd_range: cross cp_addr, cp_cmd;
  endgroup

  // Cross where one coverpoint has ignore_bins - ignored values must not appear in cross bins
  covergroup cg_ignore;
    cp_addr: coverpoint addr {
      ignore_bins ign = {3};  // addr=3 excluded from cross
      bins a0 = {0};
      bins a1 = {1};
    }
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    cross_ab: cross cp_addr, cp_cmd;
  endgroup

  // Cross ignore_bins using binsof, intersect value/range lists, &&, and negation
  covergroup cg_cross_ignore;
    cp_addr: coverpoint addr {
      bins addr0 = {0};
      bins addr1 = {1};
      bins addr2 = {2};
      bins addr3 = {3};
    }
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd: cross cp_addr, cp_cmd {
      ignore_bins no_values = !binsof(cp_addr) intersect {[$ : $]};
      ignore_bins outside_domain = binsof(cp_addr) intersect {-1, 4};
      ignore_bins reversed_range = binsof(cp_addr) intersect {[3 : 1]};
      ignore_bins addr0 = !binsof(cp_addr) intersect {[1 : 3]};
      ignore_bins high_write
          = binsof(cp_addr) intersect {3, [2 : 2]} && binsof(cp_cmd.write);
    }
  endgroup

  // Cross ignore_bins using bare/qualified binsof, &&, ||, !, and parentheses
  covergroup cg_cross_ignore_logic;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd: cross cp_addr, cp_cmd {
      ignore_bins except_addr0_read
          = binsof(cp_addr) && (!binsof(cp_addr.addr0) || !binsof(cp_cmd.read));
    }
  endgroup

  // Cross ignore_bins with a signed coverpoint domain
  covergroup cg_cross_ignore_signed;
    cp_data: coverpoint signed_data {
      bins zero = {0};
      bins one = {1};
      bins two = {2};
      bins three = {3};
    }
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins low = binsof(cp_data) intersect {[0 : 1]};
    }
  endgroup

  // Cross ignore_bins with negative singleton, closed-range, and open-range selectors
  covergroup cg_cross_ignore_signed_negative;
    cp_data: coverpoint signed_data {
      bins neg_four = {-4};
      bins neg_three = {-3};
      bins neg_two = {-2};
      bins neg_one = {-1};
      bins zero = {0};
      bins one = {1};
    }
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins min_value = binsof(cp_data) intersect {[$ : -4]};
      ignore_bins negative_range = binsof(cp_data) intersect {[-3 : -2]};
      ignore_bins negative_singleton = binsof(cp_data) intersect {-1};
    }
  endgroup

  // Cross ignore_bins widens a negative signed selector to the coverpoint width
  covergroup cg_cross_ignore_signed_wide;
    cp_data: coverpoint wide_signed_data {
      bins neg_one = {-1};
      bins one = {1};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins neg_one = binsof(cp_data) intersect {-1};
    }
  endgroup

  // Qualified binsof on a wildcard bin
  covergroup cg_cross_ignore_wildcard;
    cp_data: coverpoint wildcard_data {
      wildcard bins low = {4'b0???};
      wildcard bins high = {4'b1???};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins low = binsof(cp_data.low);
    }
  endgroup

  // Qualified and unqualified intersect selectors on wildcard bins
  covergroup cg_cross_ignore_wildcard_intersect;
    cp_data: coverpoint wildcard_data {
      wildcard bins low = {4'b00??};
      wildcard bins middle = {4'b01??};
      wildcard bins high = {4'b1???};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins low = binsof(cp_data) intersect {4'h1};
      ignore_bins middle = binsof(cp_data.middle) intersect {4'h5};
    }
  endgroup

  // Wildcard array bins expand into one concrete bin per matching value
  covergroup cg_cross_ignore_wildcard_array;
    cp_data: coverpoint wildcard_data {
      wildcard bins patterns[] = {4'b11??};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins twelve = binsof(cp_data.patterns) intersect {4'hc};
    }
  endgroup

  // Qualified binsof with intersect selects one concrete array-bin element
  covergroup cg_cross_ignore_array;
    cp_data: coverpoint addr {
      bins empty[] = {[3 : 1]};
      bins values[] = {[0 : 3]};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins empty = binsof(cp_data.empty);
      ignore_bins zero = binsof(cp_data.values) intersect {0};
    }
  endgroup

  // Signed array-bin ranges retain per-element signedness in intersect selectors
  covergroup cg_cross_ignore_signed_array;
    cp_data: coverpoint signed_data {bins values[] = {[-2 : -1]};}
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins neg_one = binsof(cp_data.values) intersect {-1};
    }
  endgroup

  // Array-bin ranges are clipped before narrowing to the signed coverpoint width
  covergroup cg_cross_ignore_signed_array_clipped;
    cp_data: coverpoint signed_data {bins values[] = {[2 : 5]};}
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins three = binsof(cp_data.values) intersect {3};
    }
  endgroup

  // Signed array-bin range materialization also supports widths above 64 bits
  covergroup cg_cross_ignore_signed_array_wide;
    cp_data: coverpoint huge_signed_data {bins values[] = {[-2 : -1]};}
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins neg_one = binsof(cp_data.values) intersect {-1};
    }
  endgroup

  // Named default, ignore, and illegal bins resolve to empty cross-bin sets
  covergroup cg_cross_ignore_excluded_names;
    cp_data: coverpoint addr {
      bins zero = {0};
      bins fallback = default;
      ignore_bins ignored = {1};
      illegal_bins illegal = {2};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins excluded
          = binsof(cp_data.fallback)
            || binsof(cp_data.ignored)
            || binsof(cp_data.illegal);
    }
  endgroup

  // Array-element report names cannot collide with legal scalar bin identifiers
  covergroup cg_cross_name_collision;
    cp_data: coverpoint addr {
      bins values[] = {0};
      bins values_0_ = {1};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd;
  endgroup

  // Unbased unsized literals fill to the coverpoint width
  covergroup cg_cross_ignore_fill_literal;
    cp_data: coverpoint wildcard_data {
      bins one = {1};
      bins all_ones = {15};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins all_ones = binsof(cp_data) intersect {'1};
    }
  endgroup

  // Wildcard patterns resolve at their original width before domain filtering
  covergroup cg_cross_ignore_wildcard_narrow;
    cp_data: coverpoint signed_data {
      wildcard bins low = {4'b0???};
      bins neg_one = {-1};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins none = binsof(cp_data.low) intersect {-1};
    }
  endgroup

  // Wildcard arrays likewise filter original-width values before narrowing
  covergroup cg_cross_ignore_wildcard_array_narrow;
    cp_data: coverpoint signed_data {
      wildcard bins values[] = {4'b0???};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins zero = binsof(cp_data.values) intersect {0};
    }
  endgroup

  // Scalar ranges are clipped before narrowing to the signed coverpoint width
  covergroup cg_cross_ignore_scalar_range_clipped;
    cp_data: coverpoint signed_data {bins clipped = {[2 : 5]};}
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins none = binsof(cp_data) intersect {6};
    }
  endgroup

  // Scalar range comparisons support constants wider than 64 bits
  covergroup cg_cross_ignore_scalar_range_wide;
    cp_data: coverpoint huge_signed_data {
      bins low_to_zero = {[65'sh1_0000_0000_0000_0000 : 0]};
    }
    cp_cmd: coverpoint cmd {bins any = {[0 : 1]};}
    data_cmd: cross cp_data, cp_cmd {
      ignore_bins none = binsof(cp_data) intersect {1};
    }
  endgroup

  // Cross with option.at_least set in the cross body
  covergroup cg_at_least;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd_al: cross cp_addr, cp_cmd{option.at_least = 3;}
  endgroup

  // Cross with option.goal set in the cross body
  covergroup cg_goal;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd_goal: cross cp_addr, cp_cmd{option.goal = 90;}
  endgroup

  // Cross with an unsupported option (option.per_instance) - Verilator warns and ignores it
  covergroup cg_unsup_cross_opt;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    addr_cmd_unsup: cross cp_addr, cp_cmd{
      option.per_instance = 1;  // unsupported for cross - expect COVERIGN warning
    }
    // Non-standard hierarchical reference as a cross item (an implicit coverpoint):
    // accepted with NONSTD, but implicit coverpoints are unsupported so the whole
    // cross is dropped (COVERIGN, suppressed here) - it contributes no bins.
    /* verilator lint_off NONSTD */
    cross_hier: cross cp_addr, s_cfg.m_p;
    /* verilator lint_on NONSTD */
  endgroup

  // Covergroup with an unnamed cross - the cross is reported under the default name "cross"
  covergroup cg_unnamed_cross;
    cp_a: coverpoint addr {bins a0 = {0}; bins a1 = {1};}
    cp_c: coverpoint cmd {bins read = {0}; bins write = {1};}
    cross cp_a, cp_c;  // no label: reported under the default cross name
  endgroup

  // Cross plus an un-crossed coverpoint: get_inst_coverage must combine the converted
  // (VlCoverpoint) coverpoint cp_solo with the legacy cross/crossed-coverpoint bins.
  covergroup cg_mixed;
    cp_addr: coverpoint addr {bins addr0 = {0}; bins addr1 = {1};}
    cp_cmd: coverpoint cmd {bins read = {0}; bins write = {1};}
    cp_solo: coverpoint mode {bins normal = {0}; bins debug = {1};}  // not crossed
    ab: cross cp_addr, cp_cmd;
  endgroup

  // Crossed (hence non-convertible) coverpoint that also has a default bin: exercises the
  // legacy default-bin codegen path that converted coverpoints bypass.
  covergroup cg_def_cross;
    cp_a: coverpoint addr iff (mode) {bins a0 = {0}; bins a1 = {1}; bins ad = default;}
    cp_c: coverpoint cmd {bins read = {0}; bins write = {1};}
    axc: cross cp_a, cp_c;
  endgroup

  cg2 cg2_inst = new;
  cg_ignore cg_ignore_inst = new;
  cg_cross_ignore cg_cross_ignore_inst = new;
  cg_cross_ignore_array cg_cross_ignore_array_inst = new;
  cg_cross_ignore_excluded_names cg_cross_ignore_excluded_names_inst = new;
  cg_cross_ignore_fill_literal cg_cross_ignore_fill_literal_inst = new;
  cg_cross_ignore_logic cg_cross_ignore_logic_inst = new;
  cg_cross_ignore_scalar_range_clipped cg_cross_ignore_scalar_range_clipped_inst = new;
  cg_cross_ignore_scalar_range_wide cg_cross_ignore_scalar_range_wide_inst = new;
  cg_cross_ignore_signed_array cg_cross_ignore_signed_array_inst = new;
  cg_cross_ignore_signed_array_clipped cg_cross_ignore_signed_array_clipped_inst = new;
  cg_cross_ignore_signed_array_wide cg_cross_ignore_signed_array_wide_inst = new;
  cg_cross_ignore_signed cg_cross_ignore_signed_inst = new;
  cg_cross_ignore_signed_negative cg_cross_ignore_signed_negative_inst = new;
  cg_cross_ignore_signed_wide cg_cross_ignore_signed_wide_inst = new;
  cg_cross_ignore_wildcard cg_cross_ignore_wildcard_inst = new;
  cg_cross_ignore_wildcard_array cg_cross_ignore_wildcard_array_inst = new;
  cg_cross_ignore_wildcard_array_narrow cg_cross_ignore_wildcard_array_narrow_inst = new;
  cg_cross_ignore_wildcard_intersect cg_cross_ignore_wildcard_intersect_inst = new;
  cg_cross_ignore_wildcard_narrow cg_cross_ignore_wildcard_narrow_inst = new;
  cg_cross_name_collision cg_cross_name_collision_inst = new;
  cg_range cg_range_inst = new;
  cg3 cg3_inst = new;
  cg4 cg4_inst = new;
  cg5 cg5_inst = new;
  cg_at_least cg_at_least_inst = new;
  cg_goal cg_goal_inst = new;
  cg_unsup_cross_opt cg_unsup_cross_opt_inst = new;
  cg_unnamed_cross cg_unnamed_cross_inst = new;
  cg_mixed cg_mixed_inst = new;
  cg_def_cross cg_def_cross_inst = new;

  initial begin
    // Sample 2-way: hit all 4 combinations
    // cg2: 2 cp bins + 2 cp bins + 4 cross bins = 8 bins total (flat count)
    addr = 0;
    cmd = 0;
    mode = 0;
    parity = 0;
    cg2_inst.sample();  // addr0 x read
    `checkr(cg2_inst.get_inst_coverage(), 37.5);  // 3/8: addr0, read, addr0_x_read
    addr = 1;
    cmd = 1;
    mode = 0;
    parity = 0;
    cg2_inst.sample();  // addr1 x write
    `checkr(cg2_inst.get_inst_coverage(), 75.0);  // 6/8: all cp bins + 2 cross bins
    addr = 0;
    cmd = 1;
    mode = 0;
    parity = 0;
    cg2_inst.sample();  // addr0 x write
    `checkr(cg2_inst.get_inst_coverage(), 87.5);  // 7/8: 3 cross bins hit
    addr = 1;
    cmd = 0;
    mode = 0;
    parity = 0;
    cg2_inst.sample();  // addr1 x read
    `checkr(cg2_inst.get_inst_coverage(), 100.0);  // 8/8: all 4 cross bins hit

    // Sample 3-way: hit 4 of 12 combinations
    // cg3: 3+2+2+12=19 bins; 4 cross bins hit -> 11/19=57.9% (not clean; no intermediate checkr)
    addr = 0;
    cmd = 0;
    mode = 0;
    cg3_inst.sample();  // addr0 x read x normal
    addr = 1;
    cmd = 1;
    mode = 0;
    cg3_inst.sample();  // addr1 x write x normal
    addr = 2;
    cmd = 0;
    mode = 1;
    cg3_inst.sample();  // addr2 x read x debug
    addr = 0;
    cmd = 1;
    mode = 1;
    cg3_inst.sample();  // addr0 x write x debug

    // Sample 4-way: hit 4 of 16 combinations
    // cg4: 2+2+2+2+16=24 bins; 4 cross bins hit -> 12/24=50%
    addr = 0;
    cmd = 0;
    mode = 0;
    parity = 0;
    cg4_inst.sample();
    addr = 1;
    cmd = 1;
    mode = 0;
    parity = 1;
    cg4_inst.sample();
    `checkr(cg4_inst.get_inst_coverage(), 37.5);  // 9/24: all cp bins + 2 cross bins
    addr = 0;
    cmd = 1;
    mode = 1;
    parity = 0;
    cg4_inst.sample();
    addr = 1;
    cmd = 0;
    mode = 1;
    parity = 1;
    cg4_inst.sample();
    `checkr(cg4_inst.get_inst_coverage(), 50.0);  // 12/24: all cp bins + 4 cross bins

    // Sample cg5 (cross with option.weight=2; weight is ignored in flat bin count)
    // cg5: 2+2+4=8 bins; 2 cross bins hit -> 6/8=75%
    addr = 0;
    cmd = 0;
    cg5_inst.sample();
    `checkr(cg5_inst.get_inst_coverage(), 37.5);  // 3/8: addr0, read, addr0_x_read
    addr = 1;
    cmd = 1;
    cg5_inst.sample();
    `checkr(cg5_inst.get_inst_coverage(), 75.0);  // 6/8: all cp bins + 2 cross bins

    // Sample cg_ignore: addr=3 is in ignore_bins so no cross bins for it
    // cg_ignore: 2+2+4=8 bins total
    addr = 0;
    cmd = 0;
    cg_ignore_inst.sample();  // a0 x read
    `checkr(cg_ignore_inst.get_inst_coverage(), 37.5);  // 3/8
    addr = 1;
    cmd = 1;
    cg_ignore_inst.sample();  // a1 x write
    `checkr(cg_ignore_inst.get_inst_coverage(), 75.0);  // 6/8
    addr = 0;
    cmd = 1;
    cg_ignore_inst.sample();  // a0 x write
    `checkr(cg_ignore_inst.get_inst_coverage(), 87.5);  // 7/8
    addr = 1;
    cmd = 0;
    cg_ignore_inst.sample();  // a1 x read
    `checkr(cg_ignore_inst.get_inst_coverage(), 100.0);  // 8/8
    addr = 3;
    cmd = 0;
    cg_ignore_inst.sample();  // ignored (addr=3 in ignore_bins)
    `checkr(cg_ignore_inst.get_inst_coverage(), 100.0);  // still 100%

    // Sample cg_cross_ignore: four of eight Cartesian tuples are excluded.
    // Four addr bins + two cmd bins + four retained cross bins = 10 bins.
    addr = 0;
    cmd = 0;
    cg_cross_ignore_inst.sample();  // addr0 x read is ignored by the cross
    `checkr(cg_cross_ignore_inst.get_inst_coverage(), 20.0);  // addr0 + read
    addr = 1;
    cmd = 1;
    cg_cross_ignore_inst.sample();  // addr1 x write
    `checkr(cg_cross_ignore_inst.get_inst_coverage(), 50.0);
    addr = 1;
    cmd = 0;
    cg_cross_ignore_inst.sample();  // addr1 x read
    `checkr(cg_cross_ignore_inst.get_inst_coverage(), 60.0);
    addr = 2;
    cmd = 0;
    cg_cross_ignore_inst.sample();  // addr2 x read
    `checkr(cg_cross_ignore_inst.get_inst_coverage(), 80.0);
    addr = 3;
    cmd = 0;
    cg_cross_ignore_inst.sample();  // addr3 x read
    `checkr(cg_cross_ignore_inst.get_inst_coverage(), 100.0);

    // Sample cg_cross_ignore_logic: only addr0 x read remains in the cross.
    // Two addr bins + two cmd bins + one retained cross bin = five bins.
    addr = 0;
    cmd = 0;
    cg_cross_ignore_logic_inst.sample();
    `checkr(cg_cross_ignore_logic_inst.get_inst_coverage(), 60.0);
    addr = 1;
    cmd = 1;
    cg_cross_ignore_logic_inst.sample();  // ignored by the cross
    `checkr(cg_cross_ignore_logic_inst.get_inst_coverage(), 100.0);

    // Sample cg_cross_ignore_signed: low tuples are excluded from the cross.
    signed_data = 0;
    cmd = 0;
    cg_cross_ignore_signed_inst.sample();
    `checkr(cg_cross_ignore_signed_inst.get_inst_coverage(), 20.0);
    signed_data = 1;
    cmd = 1;
    cg_cross_ignore_signed_inst.sample();
    `checkr(cg_cross_ignore_signed_inst.get_inst_coverage(), 40.0);
    signed_data = 2;
    cmd = 0;
    cg_cross_ignore_signed_inst.sample();
    `checkr(cg_cross_ignore_signed_inst.get_inst_coverage(), 60.0);
    signed_data = 2;
    cmd = 1;
    cg_cross_ignore_signed_inst.sample();
    `checkr(cg_cross_ignore_signed_inst.get_inst_coverage(), 70.0);
    signed_data = 3;
    cmd = 0;
    cg_cross_ignore_signed_inst.sample();
    `checkr(cg_cross_ignore_signed_inst.get_inst_coverage(), 90.0);
    signed_data = 3;
    cmd = 1;
    cg_cross_ignore_signed_inst.sample();
    `checkr(cg_cross_ignore_signed_inst.get_inst_coverage(), 100.0);

    // Sample negative signed selectors: all negative tuples are excluded.
    signed_data = -4;
    cmd = 0;
    cg_cross_ignore_signed_negative_inst.sample();
    cmd = 1;
    cg_cross_ignore_signed_negative_inst.sample();
    signed_data = -3;
    cmd = 0;
    cg_cross_ignore_signed_negative_inst.sample();
    cmd = 1;
    cg_cross_ignore_signed_negative_inst.sample();
    signed_data = -2;
    cmd = 0;
    cg_cross_ignore_signed_negative_inst.sample();
    cmd = 1;
    cg_cross_ignore_signed_negative_inst.sample();
    signed_data = -1;
    cmd = 0;
    cg_cross_ignore_signed_negative_inst.sample();
    cmd = 1;
    cg_cross_ignore_signed_negative_inst.sample();
    `checkr(cg_cross_ignore_signed_negative_inst.get_inst_coverage(), 50.0);
    signed_data = 0;
    cmd = 0;
    cg_cross_ignore_signed_negative_inst.sample();
    cmd = 1;
    cg_cross_ignore_signed_negative_inst.sample();
    `checkr(cg_cross_ignore_signed_negative_inst.get_inst_coverage(), 75.0);
    signed_data = 1;
    cmd = 0;
    cg_cross_ignore_signed_negative_inst.sample();
    cmd = 1;
    cg_cross_ignore_signed_negative_inst.sample();
    `checkr(cg_cross_ignore_signed_negative_inst.get_inst_coverage(), 100.0);

    // A 32-bit -1 literal is sign-extended to the 64-bit coverpoint width.
    wide_signed_data = -1;
    cmd = 0;
    cg_cross_ignore_signed_wide_inst.sample();
    `checkr(cg_cross_ignore_signed_wide_inst.get_inst_coverage(), 50.0);
    wide_signed_data = 1;
    cg_cross_ignore_signed_wide_inst.sample();
    `checkr(cg_cross_ignore_signed_wide_inst.get_inst_coverage(), 100.0);

    // A qualified wildcard selector excludes only the low wildcard bin.
    wildcard_data = 4'h5;
    cmd = 0;
    cg_cross_ignore_wildcard_inst.sample();
    `checkr(cg_cross_ignore_wildcard_inst.get_inst_coverage(), 50.0);
    wildcard_data = 4'ha;
    cg_cross_ignore_wildcard_inst.sample();
    `checkr(cg_cross_ignore_wildcard_inst.get_inst_coverage(), 100.0);

    // Wildcard intersections select a bin when any value in its pattern overlaps.
    wildcard_data = 4'h1;
    cg_cross_ignore_wildcard_intersect_inst.sample();
    `checkr(cg_cross_ignore_wildcard_intersect_inst.get_inst_coverage(), 40.0);
    wildcard_data = 4'h5;
    cg_cross_ignore_wildcard_intersect_inst.sample();
    `checkr(cg_cross_ignore_wildcard_intersect_inst.get_inst_coverage(), 60.0);
    wildcard_data = 4'ha;
    cg_cross_ignore_wildcard_intersect_inst.sample();
    `checkr(cg_cross_ignore_wildcard_intersect_inst.get_inst_coverage(), 100.0);

    // The wildcard array contributes four concrete bins; only value 12 is excluded.
    wildcard_data = 4'hc;
    cg_cross_ignore_wildcard_array_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_inst.get_inst_coverage(), 25.0);
    wildcard_data = 4'hd;
    cg_cross_ignore_wildcard_array_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_inst.get_inst_coverage(), 50.0);
    wildcard_data = 4'he;
    cg_cross_ignore_wildcard_array_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_inst.get_inst_coverage(), 75.0);
    wildcard_data = 4'hf;
    cg_cross_ignore_wildcard_array_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_inst.get_inst_coverage(), 100.0);

    // Array-bin intersect excludes values[0], retaining the other three cross bins.
    addr = 0;
    cmd = 0;
    cg_cross_ignore_array_inst.sample();
    `checkr(cg_cross_ignore_array_inst.get_inst_coverage(), 25.0);
    addr = 1;
    cmd = 1;
    cg_cross_ignore_array_inst.sample();
    `checkr(cg_cross_ignore_array_inst.get_inst_coverage(), 50.0);
    addr = 2;
    cg_cross_ignore_array_inst.sample();
    `checkr(cg_cross_ignore_array_inst.get_inst_coverage(), 75.0);
    addr = 3;
    cg_cross_ignore_array_inst.sample();
    `checkr(cg_cross_ignore_array_inst.get_inst_coverage(), 100.0);

    // Signed array-bin intersect excludes only the -1 element.
    signed_data = -1;
    cg_cross_ignore_signed_array_inst.sample();
    `checkr(cg_cross_ignore_signed_array_inst.get_inst_coverage(), 50.0);
    signed_data = -2;
    cg_cross_ignore_signed_array_inst.sample();
    `checkr(cg_cross_ignore_signed_array_inst.get_inst_coverage(), 100.0);

    // [2:5] clips to signed_data's domain, yielding concrete values 2 and 3.
    signed_data = 3;
    cg_cross_ignore_signed_array_clipped_inst.sample();
    `checkr(cg_cross_ignore_signed_array_clipped_inst.get_inst_coverage(), 50.0);
    signed_data = 2;
    cg_cross_ignore_signed_array_clipped_inst.sample();
    `checkr(cg_cross_ignore_signed_array_clipped_inst.get_inst_coverage(), 100.0);

    // A 65-bit signed range likewise retains -2 and -1 as distinct elements.
    huge_signed_data = -1;
    cg_cross_ignore_signed_array_wide_inst.sample();
    `checkr(cg_cross_ignore_signed_array_wide_inst.get_inst_coverage(), 50.0);
    huge_signed_data = -2;
    cg_cross_ignore_signed_array_wide_inst.sample();
    `checkr(cg_cross_ignore_signed_array_wide_inst.get_inst_coverage(), 100.0);

    // Excluded bin declarations resolve but add no cross tuples.
    addr = 0;
    cg_cross_ignore_excluded_names_inst.sample();
    `checkr(cg_cross_ignore_excluded_names_inst.get_inst_coverage(), 100.0);
    addr = 3;
    cg_cross_ignore_excluded_names_inst.sample();
    `checkr(cg_cross_ignore_excluded_names_inst.get_inst_coverage(), 100.0);

    // Array and scalar bins with sanitization-equivalent names remain distinct.
    addr = 0;
    cg_cross_name_collision_inst.sample();
    `checkr(cg_cross_name_collision_inst.get_inst_coverage(), 60.0);
    addr = 1;
    cg_cross_name_collision_inst.sample();
    `checkr(cg_cross_name_collision_inst.get_inst_coverage(), 100.0);

    // '1 resolves to 4'hf, so only the all_ones cross tuple is excluded.
    wildcard_data = 4'hf;
    cg_cross_ignore_fill_literal_inst.sample();
    `checkr(cg_cross_ignore_fill_literal_inst.get_inst_coverage(), 50.0);
    wildcard_data = 4'h1;
    cg_cross_ignore_fill_literal_inst.sample();
    `checkr(cg_cross_ignore_fill_literal_inst.get_inst_coverage(), 100.0);

    // 4'b0??? on a signed 3-bit coverpoint contains 0..3, not -4..3.
    signed_data = -1;
    cg_cross_ignore_wildcard_narrow_inst.sample();
    `checkr(cg_cross_ignore_wildcard_narrow_inst.get_inst_coverage(), 60.0);
    signed_data = 0;
    cg_cross_ignore_wildcard_narrow_inst.sample();
    `checkr(cg_cross_ignore_wildcard_narrow_inst.get_inst_coverage(), 100.0);

    // The corresponding wildcard array has four elements; zero alone is excluded.
    signed_data = 0;
    cg_cross_ignore_wildcard_array_narrow_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_narrow_inst.get_inst_coverage(), 25.0);
    signed_data = 1;
    cg_cross_ignore_wildcard_array_narrow_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_narrow_inst.get_inst_coverage(), 50.0);
    signed_data = 2;
    cg_cross_ignore_wildcard_array_narrow_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_narrow_inst.get_inst_coverage(), 75.0);
    signed_data = 3;
    cg_cross_ignore_wildcard_array_narrow_inst.sample();
    `checkr(cg_cross_ignore_wildcard_array_narrow_inst.get_inst_coverage(), 100.0);

    // The scalar range [2:5] clips to [2:3], so value 3 hits its tuple.
    signed_data = 3;
    cg_cross_ignore_scalar_range_clipped_inst.sample();
    `checkr(cg_cross_ignore_scalar_range_clipped_inst.get_inst_coverage(), 100.0);

    // A 65-bit signed range containing -1 also hits without 64-bit conversion.
    huge_signed_data = -1;
    cg_cross_ignore_scalar_range_wide_inst.sample();
    `checkr(cg_cross_ignore_scalar_range_wide_inst.get_inst_coverage(), 100.0);

    // Sample range-bin cross
    // cg_range: 2+2+4=8 bins
    addr = 0;
    cmd = 0;
    cg_range_inst.sample();  // lo_range x read
    `checkr(cg_range_inst.get_inst_coverage(), 37.5);  // 3/8
    addr = 2;
    cmd = 1;
    cg_range_inst.sample();  // hi_range x write
    `checkr(cg_range_inst.get_inst_coverage(), 75.0);  // 6/8
    addr = 1;
    cmd = 1;
    cg_range_inst.sample();  // lo_range x write
    `checkr(cg_range_inst.get_inst_coverage(), 87.5);  // 7/8
    addr = 3;
    cmd = 0;
    cg_range_inst.sample();  // hi_range x read
    `checkr(cg_range_inst.get_inst_coverage(), 100.0);  // 8/8

    // Sample cg_at_least (option.at_least in cross body; Verilator uses at_least=1 for bins)
    // cg_at_least: 2+2+4=8 bins; 2 cross bins hit (count=1, at_least effectively 1) -> 6/8=75%
    addr = 0;
    cmd = 0;
    cg_at_least_inst.sample();  // addr0 x read
    addr = 1;
    cmd = 1;
    cg_at_least_inst.sample();  // addr1 x write
    `checkr(cg_at_least_inst.get_inst_coverage(), 75.0);

    // Sample cg_goal (option.goal in cross body; does not affect hit counting)
    // cg_goal: 2+2+4=8 bins; 2 cross bins hit -> 6/8=75%
    addr = 0;
    cmd = 0;
    cg_goal_inst.sample();  // addr0 x read
    addr = 1;
    cmd = 1;
    cg_goal_inst.sample();  // addr1 x write
    `checkr(cg_goal_inst.get_inst_coverage(), 75.0);

    // Sample cg_unsup_cross_opt
    // cg_unsup_cross_opt: 2+2+4=8 bins; 2 cross bins hit -> 6/8=75%
    addr = 0;
    cmd = 0;
    cg_unsup_cross_opt_inst.sample();  // addr0 x read
    addr = 1;
    cmd = 1;
    cg_unsup_cross_opt_inst.sample();  // addr1 x write
    `checkr(cg_unsup_cross_opt_inst.get_inst_coverage(), 75.0);

    // Sample cg_unnamed_cross
    // cg_unnamed_cross: 2+2+4=8 bins; 2 cross bins hit -> 6/8=75%
    addr = 0;
    cmd = 0;
    cg_unnamed_cross_inst.sample();  // a0 x read
    addr = 1;
    cmd = 1;
    cg_unnamed_cross_inst.sample();  // a1 x write
    `checkr(cg_unnamed_cross_inst.get_inst_coverage(), 75.0);

    // Sample cg_mixed: 10 bins total (cp_addr 2 + cp_cmd 2 + cp_solo 2 + cross ab 4)
    addr = 0; cmd = 0; mode = 0;
    cg_mixed_inst.sample();  // addr0, read, solo normal, ab(addr0_x_read)
    `checkr(cg_mixed_inst.get_inst_coverage(), 40.0);  // 4/10
    addr = 0; cmd = 1; mode = 1;
    cg_mixed_inst.sample();  // addr0, write, solo debug, ab(addr0_x_write)
    addr = 1; cmd = 0; mode = 0;
    cg_mixed_inst.sample();  // addr1, read, ab(addr1_x_read)
    addr = 1; cmd = 1; mode = 1;
    cg_mixed_inst.sample();  // addr1, write, ab(addr1_x_write)
    `checkr(cg_mixed_inst.get_inst_coverage(), 100.0);  // 10/10

    // Sample cg_def_cross (default bin in a crossed coverpoint, gated by iff)
    mode = 1;
    addr = 0; cmd = 0; cg_def_cross_inst.sample();  // a0, read
    addr = 2; cmd = 1; cg_def_cross_inst.sample();  // ad (default), write

    $write("*-* All Finished *-*\n");
    $finish;
  end

endmodule
