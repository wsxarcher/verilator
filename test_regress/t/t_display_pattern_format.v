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

  localparam text_t TEXT_PARAM = "quote=\" slash=\\ bell=\a form=\f vert=\v ctrl=\001";
  localparam string ESCAPED_PARAM_STRING = $sformatf("%p", TEXT_PARAM);
  localparam string WIDE_PARAM_STRING = $sformatf("%p", WIDE_ON);
  localparam signed_mode_t INVALID_PARAM = signed_mode_t'(-33'sd2);
  localparam string INVALID_PARAM_STRING = $sformatf("%p", INVALID_PARAM);
  int enum_calls;

  function automatic wide_mode_t make_enum();
    ++enum_calls;
    return cyc[0] ? WIDE_OFF : WIDE_ON;
  endfunction

  initial begin
    `checks(WIDE_PARAM_STRING, "WIDE_ON");
    `checks(INVALID_PARAM_STRING, "-2");
    `checks($sformatf("%p", MODE_ON), "MODE_ON");
    `checks($sformatf("%p", mode_t'(7)), "7");
    `checks($sformatf("%p", SIGNED7_OFF), "SIGNED7_OFF");
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

    cyc <= cyc + 1;
    if (cyc == 3) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule
