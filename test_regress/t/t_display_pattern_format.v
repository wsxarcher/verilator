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

interface Iface;
endinterface

interface class IfaceClass;
endclass

class Obj;
  int value;
endclass

// This class is formatted only as a function's return value.
class ReturnedObj;
  int value;
endclass

module t;
  bit clk = 0;
  always #5 clk = ~clk;

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

  typedef struct {
    logic [6:0] \a.b ;
  } escaped_unpacked_t;

  typedef union packed {
    logic [14:0] first;
    logic [14:0] second;
  } packed_union_t;

  typedef struct {
    mode_t mode;
    wide_mode_t wide_mode;
    signed_mode_t signed_mode;
    string text;
    packed_t packed_value;
    int numbers[2];
  } unpacked_t;

  typedef union {
    int first;
    int second;
  } unpacked_union_t;

  typedef struct {
    mode_t mode;
    string text;
  } param_unpacked_t;

  typedef chandle handle_t;
  typedef real real_t;
  typedef string text_t;
  typedef int pair_t[2];

`ifdef QUESTA
  // Questa 2025.2's chandle spelling violates 21.2.1.7; event spelling is unspecified.
  localparam string NULL_CHANDLE_TEXT = "null-pointer";
  localparam string EVENT_TEXT = "<no-events>";
`else
  localparam string NULL_CHANDLE_TEXT = "null";
  localparam string EVENT_TEXT = "triggered=false";
`endif

  localparam packed_t PACKED_PARAM = '{code: 3, mode: MODE_ON, tail: 17};
  localparam string PACKED_PARAM_STRING = $sformatf("%p", PACKED_PARAM);
  localparam param_unpacked_t UNPACKED_PARAM = '{mode: MODE_ON, text: "param"};
  localparam string UNPACKED_PARAM_STRING = $sformatf("%p", UNPACKED_PARAM);
  localparam signed_fields_t SIGNED_PARAM
      = '{-7'sd11, -33'sd4294967296, -65'sd18446744073709551616};
  localparam string SIGNED_PARAM_STRING = $sformatf("%p", SIGNED_PARAM);
  localparam escaped_packed_t ESCAPED_FIELD_PARAM = '{7'd7};
  localparam string ESCAPED_FIELD_PARAM_STRING = $sformatf("%p", ESCAPED_FIELD_PARAM);
  localparam string WIDE_PARAM_STRING = $sformatf("%p", WIDE_ON);
  localparam signed_mode_t INVALID_PARAM = signed_mode_t'(-33'sd2);
  localparam string INVALID_PARAM_STRING = $sformatf("%p", INVALID_PARAM);
  localparam text_t TEXT_PARAM = "quote=\" slash=\\ bell=\a form=\f vert=\v ctrl=\001";
  localparam string ESCAPED_PARAM_STRING = $sformatf("%p", TEXT_PARAM);

  Iface iface();
  virtual Iface vif;

  int cyc = 0;
  int calls;
  int enum_calls;
  int object_calls;

  function automatic packed_t make_packed();
    ++calls;
    return '{code: 3'(cyc + 3), mode: (cyc[0] ? MODE_OFF : MODE_ON), tail: 5'(cyc + 17)};
  endfunction

  function automatic ReturnedObj make_object();
    ReturnedObj object_value;
    object_value = new;
    object_value.value = cyc + 7;
    ++object_calls;
    return object_value;
  endfunction

  function automatic wide_mode_t make_enum();
    ++enum_calls;
    return cyc[0] ? WIDE_OFF : WIDE_ON;
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

  // IEEE 1800-2012 21.2.1.7 permits whitespace differences outside string literals.
  function automatic string normalize_pattern(input string value);
    string normalized = "";
    bit quoted = 0;
    bit escaped = 0;
    for (int i = 0; i < value.len(); ++i) begin
      byte ch = value[i];
      if (quoted || (ch != " " && ch != "\t" && ch != "\n" && ch != "\r"))
        normalized = {normalized, value.substr(i, i)};
      if (escaped) escaped = 0;
      else if (quoted && ch == "\\") escaped = 1;
      else if (ch == "\"") quoted = !quoted;
    end
    return normalized;
  endfunction

  initial begin
    `checks($sformatf("%p", 12), "12");
    `checks(PACKED_PARAM_STRING, "'{code:3, mode:MODE_ON, tail:17}");
    `checks(UNPACKED_PARAM_STRING, "'{mode:MODE_ON, text:\"param\"}");
    `checks(SIGNED_PARAM_STRING,
            "'{narrow:-11, middle:-4294967296, wide:-18446744073709551616}");
    `checks(ESCAPED_FIELD_PARAM_STRING, "'{\\a.b :7}");
    `checks(WIDE_PARAM_STRING, "WIDE_ON");
    `checks(INVALID_PARAM_STRING, "-2");
    `checks($sformatf("%p", MODE_ON), "MODE_ON");
    `checks($sformatf("%p", mode_t'(7)), "7");
    `checks($sformatf("%p", SIGNED7_OFF), "SIGNED7_OFF");
`ifdef QUESTA
    // %0p is implementation-specific (IEEE 1800-2012 21.2.1.7).
    `checks($sformatf("%0p", 12), "12");
    // Questa 2025.2 drops upper bits of an invalid wide enum, instead of using its base type.
    `checks($sformatf("%p", wide_mode_t'(95'h1_0000_0000_0000_0002)), "2");
    // It also zero-extends invalid signed 7-bit enums instead of using the signed base type.
    `checks($sformatf("%p", narrow_mode_t'(-7'sd2)), "126");
    // Questa 2025.2 does not escape string values.
    `checks(ESCAPED_PARAM_STRING, {"\"", TEXT_PARAM, "\""});
`else
    `checks($sformatf("%0p", 12), "'hc");
    `checks($sformatf("%p", wide_mode_t'(95'h1_0000_0000_0000_0002)),
            "18446744073709551618");
    `checks($sformatf("%p", narrow_mode_t'(-7'sd2)), "-2");
    `checks(ESCAPED_PARAM_STRING,
            "\"quote=\\\" slash=\\\\ bell=\\007 form=\\014 vert=\\013 ctrl=\\001\"");
`endif

  end

  always @(posedge clk) begin
    string escaped;
    string escaped_expected;
    string key_expected;
    string fmt;
    string hex_fmt;
    string binary_fmt;
    string octal_fmt;
    string result;
    string packed_expected;
    string compact_expected;
    string unpacked_expected;
    string wide_expected;
    string signed_expected;
    string enum_expected;
    string union_expected;
    string long_text;
    text_t plain;
    real_t real_value;
    mode_t fixed_modes[2];
    mode_t dynamic_modes[];
    mode_t mode_queue[$];
    mode_t mode_assoc[int];
    int enum_key_assoc[mode_t];
    int string_assoc[string];
    int descending[4:2];
    int ascending[2:4];
    int matrix[3:2][5:6];
    wide_mode_t wide_mode;
    wide_mode_t invalid_wide_mode;
    narrow_mode_t narrow_mode;
    narrow_mode_t invalid_narrow_mode;
    signed_mode_t signed_mode;
    packed_t packed_value;
    wide_packed_t wide_packed;
    masked_wide_packed_t masked_wide_packed;
    nested_packed_t nested_packed;
    signed_packed_t signed_packed;
    signed_fields_t signed_fields;
    escaped_packed_t escaped_packed;
    escaped_unpacked_t escaped_unpacked;
    packed_union_t packed_union;
    unpacked_t unpacked_value;
    unpacked_union_t unpacked_union;
    Obj object;
    Obj object_queue[$];
    IfaceClass interface_object;
    handle_t handle;
    event ev;

    plain = $sformatf("round %0d", cyc);
    escaped = {"quote=\" slash=\\ line=\n cr=\r tab=\t bell=\a form=\f vert=\v ctrl=\001 ", plain};
    real_value = 1.25 + cyc;
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
    escaped_unpacked = '{7'(cyc + 7)};
    packed_union.second = 15'h1234 + 15'(cyc);
    unpacked_union.second = 32'h1234 + cyc;
    wide_mode = cyc[0] ? WIDE_OFF : WIDE_ON;
    wide_expected = cyc[0] ? "WIDE_OFF" : "WIDE_ON";
    invalid_wide_mode = wide_mode_t'(95'h1_0000_0000_0000_0002 + 95'(cyc));
    narrow_mode = cyc[0] ? SIGNED7_OFF : SIGNED7_ON;
    invalid_narrow_mode = narrow_mode_t'(-7'(cyc + 8));
    signed_mode = cyc[0] ? SIGNED_ON : signed_mode_t'(-33'sd2);
    signed_expected = cyc[0] ? "SIGNED_ON" : "-2";

    fixed_modes = '{MODE_OFF, MODE_ON};
    dynamic_modes = new[2];
    dynamic_modes = '{MODE_ON, MODE_OFF};
    mode_queue = '{MODE_OFF, MODE_ON};
    mode_assoc[2] = MODE_OFF;
    mode_assoc[4] = MODE_ON;
    enum_key_assoc[MODE_OFF] = 2;
    enum_key_assoc[MODE_ON] = 4;
    string_assoc["a b\\c"] = cyc + 3;
    descending = '{cyc + 10, cyc + 20, cyc + 30};
    ascending = '{cyc + 40, cyc + 50, cyc + 60};
    matrix = '{'{cyc + 1, cyc + 2}, '{cyc + 3, cyc + 4}};
    object_queue = '{object};

    unpacked_value.mode = packed_value.mode;
    unpacked_value.wide_mode = wide_mode;
    unpacked_value.signed_mode = signed_mode;
    unpacked_value.text = escaped;
    unpacked_value.packed_value = packed_value;
    unpacked_value.numbers = '{cyc + 10, cyc + 11};

`ifdef QUESTA
    // Questa 2025.2 leaves special characters unescaped, violating the legal-pattern
    // requirement in IEEE 1800-2012 21.2.1.7. Keep checking its actual output.
    escaped_expected = {"\"", escaped, "\""};
    key_expected = "\"a b\\c\"";
    // %0p's compact representation is implementation-specific.
    compact_expected = $sformatf("%0d %s %0d",
                                packed_value.code, packed_value.mode.name(), packed_value.tail);
    // Questa 2025.2 prints both union fields; 21.2.1.7 requires only the first.
    union_expected = $sformatf("'{first:%0d, second:%0d}", cyc + 4660, cyc + 4660);
`else
    escaped_expected = {"\"quote=\\\" slash=\\\\ line=\\n cr=\\r tab=\\t bell=\\007 ",
                        "form=\\014 vert=\\013 ctrl=\\001 ", plain, "\""};
    key_expected = "\"a b\\\\c\"";
    compact_expected = {"'h", $sformatf("%0h", packed_value)};
    union_expected = $sformatf("'{first:%0d}", cyc + 4660);
`endif

    `checks($sformatf("%p", real_value), $sformatf("%0g", real_value));
    `checks($sformatf("%p", plain), {"\"", plain, "\""});
    `checks($sformatf("%p", escaped), escaped_expected);
    plain = "";
    `checks($sformatf("%p", plain), "\"\"");
    `checks($sformatf("%p", wide_mode), wide_expected);
    `checks($sformatf("%p", narrow_mode), narrow_mode.name());
`ifdef QUESTA
    // Same invalid-enum truncation and signedness bugs as in the constant cases above.
    `checks($sformatf("%p", invalid_wide_mode), $sformatf("%0d", cyc + 2));
    `checks($sformatf("%p", invalid_narrow_mode), $sformatf("%0d", 120 - cyc));
`else
    `checks($sformatf("%p", invalid_wide_mode), $sformatf("%0d", invalid_wide_mode));
    `checks($sformatf("%p", invalid_narrow_mode), $sformatf("%0d", -cyc - 8));
`endif
    `checks($sformatf("%p", signed_mode), signed_expected);

    `checks($sformatf("%p", packed_value), packed_expected);
    check_pattern_branches(packed_value, cyc[0]);
    `checks($sformatf("%P", packed_value), packed_expected);
    `checks($sformatf("%0p", packed_value), compact_expected);
    `checks($sformatf("%p", wide_packed),
            $sformatf("'{wide:%0d, mode:%s}", wide_packed.wide, wide_packed.mode.name()));
    `checks($sformatf("%p", masked_wide_packed),
            $sformatf("'{high:7, wide:%0d}", masked_wide_packed.wide));
    `checks($sformatf("%p", nested_packed),
            {"'{inner:", packed_expected, ", flag:1}"});
    `checks($sformatf("%p", signed_packed), $sformatf("'{high:15, low:%0d}", cyc + 1));
    `checks($sformatf("%p", signed_fields.narrow), $sformatf("%0d", signed_fields.narrow));
    `checks($sformatf("%p", signed_fields.middle), $sformatf("%0d", signed_fields.middle));
    `checks($sformatf("%p", signed_fields.wide), $sformatf("%0d", signed_fields.wide));
    result = $sformatf("'{narrow:%0d, middle:%0d, wide:%0d}",
                      signed_fields.narrow, signed_fields.middle, signed_fields.wide);
    `checks($sformatf("%p", signed_fields), result);
    result = $sformatf("'{\\a.b :%0d}", cyc + 7);
    `checks($sformatf("%p", escaped_packed), result);
    `checks($sformatf("%p", escaped_unpacked), result);
    `checks($sformatf("%p", packed_union), union_expected);
    `checks($sformatf("%p", unpacked_union), union_expected);
    `checks($sformatf("%p", param_unpacked_t'{MODE_ON, $sformatf("%0d", cyc)}),
            $sformatf("'{mode:MODE_ON, text:\"%0d\"}", cyc));
    `checks($sformatf("%p", pair_t'{cyc, cyc + 1}), $sformatf("'{%0d, %0d}", cyc, cyc + 1));

    `checks($sformatf("%p", fixed_modes), "'{MODE_OFF, MODE_ON}");
    `checks($sformatf("%p", dynamic_modes), "'{MODE_ON, MODE_OFF}");
    `checks($sformatf("%p", mode_queue), "'{MODE_OFF, MODE_ON}");
    `checks(normalize_pattern($sformatf("%p", mode_assoc)), "'{2:MODE_OFF,4:MODE_ON}");
    `checks(normalize_pattern($sformatf("%p", enum_key_assoc)), "'{MODE_OFF:2,MODE_ON:4}");
    result = {"'{", key_expected, $sformatf(":%0d}", cyc + 3)};
    `checks(normalize_pattern($sformatf("%p", string_assoc)), normalize_pattern(result));
    `checks($sformatf("%p", object_queue), "'{null}");
    fixed_modes[1] = mode_t'(7);
    `checks($sformatf("%p", fixed_modes), "'{MODE_OFF, 7}");
    dynamic_modes.delete();
    mode_queue.delete();
    `checks($sformatf("%p", dynamic_modes), "'{}");
    `checks($sformatf("%p", mode_queue), "'{}");
    `checks($sformatf("%p", descending),
            $sformatf("'{%0d, %0d, %0d}", cyc + 10, cyc + 20, cyc + 30));
    `checks($sformatf("%p", ascending),
            $sformatf("'{%0d, %0d, %0d}", cyc + 40, cyc + 50, cyc + 60));
    `checks($sformatf("%p", matrix),
            $sformatf("'{'{%0d, %0d}, '{%0d, %0d}}", cyc + 1, cyc + 2, cyc + 3, cyc + 4));

    unpacked_expected = {"'{mode:", packed_value.mode.name(), ", wide_mode:", wide_expected,
                         ", signed_mode:", signed_expected, ", text:", escaped_expected,
                         ", packed_value:", packed_expected,
                         $sformatf(", numbers:'{%0d, %0d}}", cyc + 10, cyc + 11)};
    `checks($sformatf("%p", unpacked_value), unpacked_expected);

    `checks($sformatf("%p", object), "null");
    `checks($sformatf("%0p", object), "null");
    `checks($sformatf("%p", interface_object), "null");
    `checks($sformatf("%0p", interface_object), "null");
    `checks($sformatf("%p", vif), "null");
    `checks($sformatf("%0p", vif), "null");
    `checks($sformatf("%p", handle), NULL_CHANDLE_TEXT);
    `checks($sformatf("%0p", handle), NULL_CHANDLE_TEXT);
    `checks($sformatf("%p", ev), EVENT_TEXT);

    // Vary the format itself, not only its arguments, to exercise runtime formatting.
    fmt = cyc[0] ? "%p" : "%P";
    `checks($sformatf(fmt, wide_mode), wide_expected);
    `checks($sformatf(fmt, narrow_mode), narrow_mode.name());
    `checks($sformatf(fmt, signed_mode), signed_expected);
    `checks($sformatf(fmt, escaped), escaped_expected);
`ifdef QUESTA
    // The invalid-enum truncation and signedness bugs also affect dynamic formats.
    `checks($sformatf(fmt, invalid_wide_mode), $sformatf("%0d", cyc + 2));
    `checks($sformatf(fmt, invalid_narrow_mode), $sformatf("%0d", 120 - cyc));
`else
    `checks($sformatf(fmt, invalid_wide_mode), $sformatf("%0d", invalid_wide_mode));
    `checks($sformatf(fmt, invalid_narrow_mode), $sformatf("%0d", -cyc - 8));
`endif

    // Both tools support enum-name %s as an extension; %h must still print the bits.
    fmt = cyc[0] ? "%s/%h" : "%h/%s";
    enum_expected = cyc[0] ? {wide_expected, "/", $sformatf("%h", wide_bits_t'(wide_mode))}
                          : {$sformatf("%h", wide_bits_t'(wide_mode)), "/", wide_expected};
    enum_calls = 0;
    result = $sformatf(fmt, make_enum(), make_enum());
    `checks(result, enum_expected);
    `checkd(enum_calls, 2);

    $swrite(result, "%p", packed_value);
    `checks(result, packed_expected);
    $swrite(result, "%p", unpacked_value);
    `checks(result, unpacked_expected);
    // Numeric leaves inherit the task's radix (21.2.1.2), using legal literals (21.2.1.7).
`ifdef QUESTA
    // Questa 2025.2 propagates the radix but omits base prefixes, producing patterns
    // that are either syntactically invalid or represent different numeric values.
    hex_fmt = "'{code:%h, mode:%s, tail:%h}|'{%h, %h, %h}|%h";
    binary_fmt = "'{code:%b, mode:%s, tail:%b}|'{%b, %b, %b}|%b";
    octal_fmt = "'{code:%o, mode:%s, tail:%o}|'{%o, %o, %o}|%o";
`else
    hex_fmt = "'{code:'h%0h, mode:%s, tail:'h%0h}|'{'h%0h, 'h%0h, 'h%0h}|'h%0h";
    binary_fmt = "'{code:'b%0b, mode:%s, tail:'b%0b}|'{'b%0b, 'b%0b, 'b%0b}|'b%0b";
    octal_fmt = "'{code:'o%0o, mode:%s, tail:'o%0o}|'{'o%0o, 'o%0o, 'o%0o}|'o%0o";
`endif
    $swriteh(result, "%p|%p|%p", packed_value, descending, cyc + 12);
    `checks(result, $sformatf(hex_fmt, packed_value.code, packed_value.mode.name(),
                             packed_value.tail, cyc + 10, cyc + 20, cyc + 30, cyc + 12));
    $swriteb(result, "%p|%p|%p", packed_value, descending, cyc + 12);
    `checks(result, $sformatf(binary_fmt, packed_value.code, packed_value.mode.name(),
                             packed_value.tail, cyc + 10, cyc + 20, cyc + 30, cyc + 12));
    $swriteo(result, "%p|%p|%p", packed_value, descending, cyc + 12);
    `checks(result, $sformatf(octal_fmt, packed_value.code, packed_value.mode.name(),
                             packed_value.tail, cyc + 10, cyc + 20, cyc + 30, cyc + 12));

    // An implicit packed argument uses ordinary decimal formatting, not %p (21.2.1.2).
    $swrite(result, packed_value);
    `checks(result, $sformatf("%d", packed_value));
    $swrite(result, signed_packed);
    `checks(result, $sformatf("%d", signed_packed));

    calls = 0;
    result = $sformatf("%p", make_packed());
    `checks(result, packed_expected);
    `checkd(calls, 1);

    object_calls = 0;
    result = $sformatf("%p", make_object());
    `checkd(result.len() > 0, 1);
    `checkd(object_calls, 1);
    fmt = cyc[0] ? "%p" : "%P";
    result = $sformatf(fmt, make_object());
    `checkd(result.len() > 0, 1);
    `checkd(object_calls, 2);

    long_text = {1100{cyc[0] ? "x" : "y"}};
    result = $sformatf("%p", long_text);
    `checkd(result.len(), 1102);
    `checks(result, {"\"", long_text, "\""});

    cyc <= cyc + 1;
    if (cyc == 3) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
