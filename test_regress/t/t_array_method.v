// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2019 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got='h%x exp='h%x\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
`define checkp(gotv,expv_s) do begin string gotv_s; gotv_s = $sformatf("%p", gotv); if ((gotv_s) != (expv_s)) begin $write("%%Error: %s:%0d:  got='%s' exp='%s'\n", `__FILE__,`__LINE__, (gotv_s), (expv_s)); `stop; end end while(0);
// verilog_format: on

module t;
  initial begin
    int q[5];
    int qv[$];  // Value returns
    int qi[$];  // Index returns
    int i;
    string v;

    q = '{1, 2, 2, 4, 3};
    `checkp(q, "'{1, 2, 2, 4, 3}");

    // NOT tested: with ... selectors

    q.sort;
    `checkp(q, "'{1, 2, 2, 3, 4}");
    q.sort with (item == 2);
    `checkp(q, "'{1, 3, 4, 2, 2}");
    q.sort(x) with (x == 3);
    `checkp(q, "'{1, 4, 2, 2, 3}");

    q.rsort;
    `checkp(q, "'{4, 3, 2, 2, 1}");
    q.rsort with (item == 2);
    `checkp(q, "'{2, 2, 4, 3, 1}");

    qv = q.unique;
    `checkp(qv, "'{2, 4, 3, 1}");
    qi = q.unique_index;
    qi.sort;
    `checkp(qi, "'{0, 2, 3, 4}");
    q.reverse;
    `checkp(q, "'{1, 3, 4, 2, 2}");
    q.shuffle();
    q.sort;
    `checkp(q, "'{1, 2, 2, 3, 4}");

    // These require an with clause or are illegal
    // TODO add a lint check that with clause is provided
    qv = q.find with (item == 2);
    `checkp(qv, "'{2, 2}");
    qv = q.find_first with (item == 2);
    `checkp(qv, "'{2}");
    qv = q.find_last with (item == 2);
    `checkp(qv, "'{2}");

    qv = q.find with (item == 20);
    `checkp(qv, "'{}");
    qv = q.find_first with (item == 20);
    `checkp(qv, "'{}");
    qv = q.find_last with (item == 20);
    `checkp(qv, "'{}");

    qi = q.find_index with (item == 2);
    qi.sort;
    `checkp(qi, "'{1, 2}");
    qi = q.find_first_index with (item == 2);
    `checkp(qi, "'{1}");
    qi = q.find_last_index with (item == 2);
    `checkp(qi, "'{2}");

    qi = q.find_index with (item == 20);
    qi.sort;
    `checkp(qi, "'{}");
    qi = q.find_first_index with (item == 20);
    `checkp(qi, "'{}");
    qi = q.find_last_index with (item == 20);
    `checkp(qi, "'{}");

    qv = q.min;
    `checkp(qv, "'{1}");
    qv = q.max;
    `checkp(qv, "'{4}");

    // Reduction methods

    i = q.sum;
    `checkh(i, 32'hc);
    i = q.product;
    `checkh(i, 32'h30);

    q = '{32'b1100, 32'b1010, 32'b1100, 32'b1010, 32'b1010};
    i = q.and;
    `checkh(i, 32'b1000);
    i = q.or;
    `checkh(i, 32'b1110);
    i = q.xor;
    `checkh(i, 32'ha);

    q = '{1, 2, 2, 4, 3};
    // `checkp(q, "'{1, 2, 2, 4, 3}");

    i = q.sum with (item + 1);
    `checkh(i, 32'h11);
    i = q.product with (item + 1);
    `checkh(i, 32'h168);

    q = '{32'b1100, 32'b1010, 32'b1100, 32'b1010, 32'b1010};
    i = q.and with (item + 1);
    `checkh(i, 32'b1001);
    i = q.or with (item + 1);
    `checkh(i, 32'b1111);
    i = q.xor with (item + 1);
    `checkh(i, 32'hb);

    // map method
    q = '{1, 2, 3, 4, 5};
    qv = q.map() with (item * 2);
    `checkp(qv, "'{2, 4, 6, 8, 10}");
    qv = q.map(x) with (x + x.index);
    `checkp(qv, "'{1, 3, 5, 7, 9}");

    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
