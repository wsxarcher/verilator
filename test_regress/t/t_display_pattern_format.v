// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkd(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0d exp=%0d\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
`define checks(gotv,expv) do if ((gotv) != (expv)) begin $write("%%Error: %s:%0d:  got='%s' exp='%s'\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
// verilog_format: on

module t;
  bit clk = 0;
  always #5 clk = ~clk;
  int cyc = 0;
  typedef string text_t;

  typedef enum logic [2:0] {
    MODE_OFF = 1,
    MODE_ON = 5
  } mode_t;
  typedef enum logic [94:0] {
    WIDE_OFF = 95'h1,
    WIDE_ON = 95'h4_0000_0000_0000_0001
  } wide_mode_t;
  typedef logic [94:0] wide_bits_t;
  typedef enum logic signed [6:0] {
    SIGNED7_OFF = -7'sd3,
    SIGNED7_ON = 7'sd7
  } narrow_mode_t;
  typedef enum logic signed [32:0] {
    SIGNED_OFF = -33'sd3,
    SIGNED_ON = 33'sd7
  } signed_mode_t;
  typedef struct packed {
    logic [2:0] code;
    mode_t mode;
    logic [4:0] tail;
  } packed_t;
  typedef struct packed {
    logic [10:0] bits;
  } raw_packed_t;
  typedef struct packed {
    logic [64:0] wide;
    mode_t mode;
  } wide_packed_t;
  typedef struct packed {
    logic [2:0] high;
    logic [64:0] wide;
  } masked_wide_packed_t;
  typedef struct packed {
    packed_t inner;
    logic flag;
  } nested_packed_t;
  typedef struct packed signed {
    logic [3:0] high;
    logic [3:0] low;
  } signed_packed_t;
  typedef struct packed {
    logic signed [6:0] narrow;
    logic signed [32:0] middle;
    logic signed [64:0] wide;
  } signed_fields_t;
  typedef struct packed {
    logic [6:0] \a.b ;
  } escaped_packed_t;
  typedef union packed {
    logic [14:0] first;
    logic [14:0] second;
  } packed_union_t;

  localparam text_t TEXT_PARAM = "quote=\" slash=\\ bell=\a form=\f vert=\v ctrl=\001";
  localparam string ESCAPED_PARAM_STRING = $sformatf("%p", TEXT_PARAM);
  localparam string WIDE_PARAM_STRING = $sformatf("%p", WIDE_ON);
  localparam signed_mode_t INVALID_PARAM = signed_mode_t'(-33'sd2);
  localparam string INVALID_PARAM_STRING = $sformatf("%p", INVALID_PARAM);
  localparam packed_t PACKED_PARAM = '{code: 3, mode: MODE_ON, tail: 17};
  localparam string PACKED_PARAM_STRING = $sformatf("%p", PACKED_PARAM);
  localparam signed_fields_t SIGNED_PARAM
      = '{-7'sd11, -33'sd4294967296, -65'sd18446744073709551616};
  localparam string SIGNED_PARAM_STRING = $sformatf("%p", SIGNED_PARAM);
  localparam escaped_packed_t ESCAPED_FIELD_PARAM = '{7'd7};
  localparam string ESCAPED_FIELD_PARAM_STRING = $sformatf("%p", ESCAPED_FIELD_PARAM);
  int enum_calls;
  int calls;

  function automatic wide_mode_t make_enum();
    ++enum_calls;
    return cyc[0] ? WIDE_OFF : WIDE_ON;
  endfunction

  function automatic packed_t make_packed();
    ++calls;
    return '{code: 3'(cyc + 3), mode: (cyc[0] ? MODE_OFF : MODE_ON), tail: 5'(cyc + 17)};
  endfunction

  task automatic check_pattern_branches(input packed_t value, input bit odd);
    string result;
    string expected;
    int branch;

    expected = $sformatf("'{code:%0d, mode:%s, tail:%0d}",
                        value.code, value.mode.name(), value.tail);
    // Common branch tails may merge only when both the type and radix agree.
    if (odd) begin
      branch = 1;
      result = $sformatf("%p", value);
    end else begin
      branch = 2;
      result = $sformatf("%p", value);
    end
    `checkd(branch, odd ? 1 : 2);
    `checks(result, expected);
    if (odd) begin
      branch = 1;
      result = $sformatf("%p", value);
    end else begin
      branch = 2;
      result = $sformatf("%p", raw_packed_t'(value));
    end
    `checkd(branch, odd ? 1 : 2);
    `checks(result, odd ? expected : $sformatf("'{bits:%0d}", value));

    if (odd) begin
      branch = 1;
      $swriteh(result, "%p", value);
    end else begin
      branch = 2;
      $swriteb(result, "%p", value);
    end
    `checkd(branch, odd ? 1 : 2);
`ifdef QUESTA
    // As below, Questa 2025.2 omits the required base prefixes.
    expected = odd ? "'{code:%h, mode:%s, tail:%h}" : "'{code:%b, mode:%s, tail:%b}";
`else
    expected = odd ? "'{code:'h%0h, mode:%s, tail:'h%0h}"
                   : "'{code:'b%0b, mode:%s, tail:'b%0b}";
`endif
    `checks(result, $sformatf(expected, value.code, value.mode.name(), value.tail));
  endtask

  initial begin
    `checks(WIDE_PARAM_STRING, "WIDE_ON");
    `checks(INVALID_PARAM_STRING, "-2");
    `checks($sformatf("%p", MODE_ON), "MODE_ON");
    `checks($sformatf("%p", mode_t'(7)), "7");
    `checks($sformatf("%p", SIGNED7_OFF), "SIGNED7_OFF");
    `checks(PACKED_PARAM_STRING, "'{code:3, mode:MODE_ON, tail:17}");
    `checks(SIGNED_PARAM_STRING,
            "'{narrow:-11, middle:-4294967296, wide:-18446744073709551616}");
    `checks(ESCAPED_FIELD_PARAM_STRING, "'{\\a.b :7}");
`ifdef QUESTA
    // Questa 2025.2 does not escape strings as required by IEEE 1800-2012 21.2.1.7.
    `checks(ESCAPED_PARAM_STRING, {"\"", TEXT_PARAM, "\""});
    // Questa drops upper bits of invalid wide enums and zero-extends invalid signed enums.
    `checks($sformatf("%p", wide_mode_t'(95'h1_0000_0000_0000_0002)), "2");
    `checks($sformatf("%p", narrow_mode_t'(-7'sd2)), "126");
`else
    `checks(ESCAPED_PARAM_STRING,
            "\"quote=\\\" slash=\\\\ bell=\\007 form=\\014 vert=\\013 ctrl=\\001\"");
    `checks($sformatf("%p", wide_mode_t'(95'h1_0000_0000_0000_0002)),
            "18446744073709551618");
    `checks($sformatf("%p", narrow_mode_t'(-7'sd2)), "-2");
`endif
  end

  always @(posedge clk) begin
    text_t plain;
    string escaped;
    string escaped_expected;
    string fmt;
    string result;
    string wide_expected;
    string signed_expected;
    string enum_expected;
    wide_mode_t wide_mode;
    wide_mode_t invalid_wide_mode;
    narrow_mode_t narrow_mode;
    narrow_mode_t invalid_narrow_mode;
    signed_mode_t signed_mode;
    string packed_expected;
    string compact_expected;
    string union_expected;
    string hex_fmt;
    string binary_fmt;
    string octal_fmt;
    packed_t packed_value;
    wide_packed_t wide_packed;
    masked_wide_packed_t masked_wide_packed;
    nested_packed_t nested_packed;
    signed_packed_t signed_packed;
    signed_fields_t signed_fields;
    escaped_packed_t escaped_packed;
    packed_union_t packed_union;

    plain = $sformatf("round %0d", cyc);
    escaped = {"quote=\" slash=\\ line=\n cr=\r tab=\t bell=\a form=\f vert=\v ctrl=\001 ", plain};
`ifdef QUESTA
    escaped_expected = {"\"", escaped, "\""};
`else
    escaped_expected = {"\"quote=\\\" slash=\\\\ line=\\n cr=\\r tab=\\t bell=\\007 ",
                        "form=\\014 vert=\\013 ctrl=\\001 ", plain, "\""};
`endif
    `checks($sformatf("%p", plain), {"\"", plain, "\""});
    `checks($sformatf("%p", escaped), escaped_expected);
    `checks($sformatf("%s", escaped), escaped);
    fmt = cyc[0] ? "%p" : "%P";
    `checks($sformatf(fmt, escaped), escaped_expected);
    plain = "";
    `checks($sformatf("%p", plain), "\"\"");

    wide_mode = cyc[0] ? WIDE_OFF : WIDE_ON;
    wide_expected = cyc[0] ? "WIDE_OFF" : "WIDE_ON";
    invalid_wide_mode = wide_mode_t'(95'h1_0000_0000_0000_0002 + 95'(cyc));
    narrow_mode = cyc[0] ? SIGNED7_OFF : SIGNED7_ON;
    invalid_narrow_mode = narrow_mode_t'(-7'(cyc + 8));
    signed_mode = cyc[0] ? SIGNED_ON : signed_mode_t'(-33'sd2);
    signed_expected = cyc[0] ? "SIGNED_ON" : "-2";
    `checks($sformatf("%p", wide_mode), wide_expected);
    `checks($sformatf("%p", narrow_mode), narrow_mode.name());
    `checks($sformatf("%p", signed_mode), signed_expected);
    `checks($sformatf(fmt, wide_mode), wide_expected);
    `checks($sformatf(fmt, narrow_mode), narrow_mode.name());
    `checks($sformatf(fmt, signed_mode), signed_expected);
`ifdef QUESTA
    `checks($sformatf("%p", invalid_wide_mode), $sformatf("%0d", cyc + 2));
    `checks($sformatf("%p", invalid_narrow_mode), $sformatf("%0d", 120 - cyc));
    `checks($sformatf(fmt, invalid_wide_mode), $sformatf("%0d", cyc + 2));
    `checks($sformatf(fmt, invalid_narrow_mode), $sformatf("%0d", 120 - cyc));
`else
    `checks($sformatf("%p", invalid_wide_mode), $sformatf("%0d", invalid_wide_mode));
    `checks($sformatf("%p", invalid_narrow_mode), $sformatf("%0d", -cyc - 8));
    `checks($sformatf(fmt, invalid_wide_mode), $sformatf("%0d", invalid_wide_mode));
    `checks($sformatf(fmt, invalid_narrow_mode), $sformatf("%0d", -cyc - 8));
`endif
    fmt = cyc[0] ? "%s/%h" : "%h/%s";
    enum_expected = cyc[0] ? {wide_expected, "/", $sformatf("%h", wide_bits_t'(wide_mode))}
                          : {$sformatf("%h", wide_bits_t'(wide_mode)), "/", wide_expected};
    enum_calls = 0;
    result = $sformatf(fmt, make_enum(), make_enum());
    `checks(result, enum_expected);
    `checkd(enum_calls, 2);

    packed_value = '{code: 3'(cyc + 3), mode: (cyc[0] ? MODE_OFF : MODE_ON),
                     tail: 5'(cyc + 17)};
    packed_expected = $sformatf("'{code:%0d, mode:%s, tail:%0d}",
                               packed_value.code, packed_value.mode.name(), packed_value.tail);
    wide_packed = '{wide: 65'h1_0000_0000_0000_0001 + 65'(cyc), mode: packed_value.mode};
    masked_wide_packed = '{high: 3'h7, wide: wide_packed.wide};
    nested_packed = '{inner: packed_value, flag: 1'b1};
    signed_packed = 8'hf1 + 8'(cyc);
    signed_fields = '{narrow: SIGNED_PARAM.narrow + 7'(cyc),
                      middle: SIGNED_PARAM.middle + 33'(cyc),
                      wide: SIGNED_PARAM.wide + 65'(cyc)};
    escaped_packed = '{7'(cyc + 7)};
    packed_union.second = 15'h1234 + 15'(cyc);
`ifdef QUESTA
    // Compact output is unspecified; untagged unions should display only the first member.
    compact_expected = $sformatf("%0d %s %0d",
                                packed_value.code, packed_value.mode.name(), packed_value.tail);
    union_expected = $sformatf("'{first:%0d, second:%0d}", cyc + 4660, cyc + 4660);
    // Questa propagates the task radix but omits base prefixes.
    hex_fmt = "'{code:%h, mode:%s, tail:%h}|%h";
    binary_fmt = "'{code:%b, mode:%s, tail:%b}|%b";
    octal_fmt = "'{code:%o, mode:%s, tail:%o}|%o";
`else
    compact_expected = {"'h", $sformatf("%0h", packed_value)};
    union_expected = $sformatf("'{first:%0d}", cyc + 4660);
    hex_fmt = "'{code:'h%0h, mode:%s, tail:'h%0h}|'h%0h";
    binary_fmt = "'{code:'b%0b, mode:%s, tail:'b%0b}|'b%0b";
    octal_fmt = "'{code:'o%0o, mode:%s, tail:'o%0o}|'o%0o";
`endif
    `checks($sformatf("%p", packed_value), packed_expected);
    check_pattern_branches(packed_value, cyc[0]);
    `checks($sformatf("%P", packed_value), packed_expected);
    `checks($sformatf("%0p", packed_value), compact_expected);
    `checks($sformatf("%p", wide_packed),
            $sformatf("'{wide:%0d, mode:%s}", wide_packed.wide, wide_packed.mode.name()));
    `checks($sformatf("%p", masked_wide_packed),
            $sformatf("'{high:7, wide:%0d}", masked_wide_packed.wide));
    `checks($sformatf("%p", nested_packed), {"'{inner:", packed_expected, ", flag:1}"});
    `checks($sformatf("%p", signed_packed), $sformatf("'{high:15, low:%0d}", cyc + 1));
    `checks($sformatf("%p", signed_fields),
            $sformatf("'{narrow:%0d, middle:%0d, wide:%0d}",
                      signed_fields.narrow, signed_fields.middle, signed_fields.wide));
    `checks($sformatf("%p", escaped_packed), $sformatf("'{\\a.b :%0d}", cyc + 7));
    `checks($sformatf("%p", packed_union), union_expected);
    $swriteh(result, "%p|%p", packed_value, cyc + 12);
    `checks(result, $sformatf(hex_fmt, packed_value.code, packed_value.mode.name(),
                             packed_value.tail, cyc + 12));
    $swriteb(result, "%p|%p", packed_value, cyc + 12);
    `checks(result, $sformatf(binary_fmt, packed_value.code, packed_value.mode.name(),
                             packed_value.tail, cyc + 12));
    $swriteo(result, "%p|%p", packed_value, cyc + 12);
    `checks(result, $sformatf(octal_fmt, packed_value.code, packed_value.mode.name(),
                             packed_value.tail, cyc + 12));
    $swrite(result, packed_value);
    `checks(result, $sformatf("%d", packed_value));
    $swrite(result, signed_packed);
    `checks(result, $sformatf("%d", signed_packed));
    calls = 0;
    result = $sformatf("%p", make_packed());
    `checks(result, packed_expected);
    `checkd(calls, 1);

    cyc <= cyc + 1;
    if (cyc == 3) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
