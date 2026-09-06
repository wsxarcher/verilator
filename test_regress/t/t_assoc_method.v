// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2024 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got='h%x exp='h%x\n", `__FILE__,`__LINE__, (gotv), (expv)); `stop; end while(0);
`define checkp(gotv,expv_s) do begin string gotv_s; gotv_s = $sformatf("%p", gotv); if ((gotv_s) != (expv_s)) begin $write("%%Error: %s:%0d:  got='%s' exp='%s'\n", `__FILE__,`__LINE__, (gotv_s), (expv_s)); `stop; end end while(0);
// verilog_format: on

module t;
  typedef struct {int x, y;} point;

  function automatic int vec_len_squared(point p);
    return p.x * p.x + p.y * p.y;
  endfunction

  initial begin
    int q[int];
    int qe[int];  // Empty
    int qv[$];  // Value returns
    int qi[$];  // Index returns
    bit[229:0] qw[int]; // Wide values
    bit[229:0] qwe[int]; // Wide values - empty
    bit[229:0] qwv[$];  // Wide values - Value returns
    int qwi[$];  // Wide values - Index returns
    point points_q[int];
    point points_qe[int]; // Empty points
    point points_qv[$];
    int i;
    bit b;
    bit[229:0] w;

    q = '{10: 1, 11: 2, 12: 2, 13: 4, 14: 3};
    `checkp(q, "'{10:1, 11:2, 12:2, 13:4, 14:3}");

    qw = '{10: 1, 11: 2, 12: 2, 13: 4, 14: 3};
    `checkp(qw, "'{10:1, 11:2, 12:2, 13:4, 14:3}");

    // NOT tested: with ... selectors

    //q.sort;  // Not legal on assoc - see t_assoc_meth_bad
    //q.rsort;  // Not legal on assoc - see t_assoc_meth_bad
    //q.reverse;  // Not legal on assoc - see t_assoc_meth_bad
    //q.shuffle;  // Not legal on assoc - see t_assoc_meth_bad

    `checkp(qe, "'{}");
    qv = q.unique;
    `checkp(qv, "'{1, 2, 4, 3}");
    qv = qe.unique;
    `checkp(qv, "'{}");

    qwv = qw.unique;
    `checkp(qwv, "'{1, 2, 4, 3}");
    qwv = qwe.unique;
    `checkp(qwv, "'{}");

    qi = q.unique_index;
    qi.sort;
    `checkp(qi, "'{10, 11, 13, 14}");
    qi = qe.unique_index;
    `checkp(qi, "'{}");

    qwi = qw.unique_index;
    qwi.sort;
    `checkp(qwi, "'{10, 11, 13, 14}");
    qwi = qwe.unique_index;
    `checkp(qwi, "'{}");

    points_q[0] = point'{1, 2};
    points_q[1] = point'{2, 4};
    points_q[5] = point'{1, 4};

    qi = points_qe.unique_index();
    `checkp(qi, "'{}");

    qi = points_q.unique_index();
    `checkh(qi.size, 3);

    points_qv = points_q.unique(p) with (p.x);
    `checkh(points_qv.size, 2);
    qi = points_q.unique_index (p) with (p.x + p.y);
    qi.sort;
    `checkp(qi, "'{0, 1, 5}");

    qi = points_qe.unique_index();
    `checkp(qi, "'{}");

    qi = points_q.unique_index();
    `checkh(qi.size, 3);

    qi = points_q.find_first_index with (item.x == 1);
    `checkp(qi, "'{0}");
    qi = points_q.find_first_index with (item.x == 10);
    `checkp(qi, "'{}");
    qi = points_q.find_last_index with (item.x == 1);
    `checkp(qi, "'{5}");
    qi = points_q.find_last_index with (item.x == 12);
    `checkp(qi, "'{}");

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
    `checkp(qi, "'{11, 12}");
    qi = q.find_first_index with (item == 2);
    `checkp(qi, "'{11}");
    qi = q.find_last_index with (item == 2);
    `checkp(qi, "'{12}");

    qi = q.find_index with (item == 20);
    qi.sort;
    `checkp(qi, "'{}");
    qi = q.find_first_index with (item == 20);
    `checkp(qi, "'{}");
    qi = q.find_last_index with (item == 20);
    `checkp(qi, "'{}");

    qi = q.find_index with (item.index == 12);
    `checkp(qi, "'{12}");
    qi = q.find with (item.index == 12);
    `checkp(qi, "'{2}");

    qv = q.min;
    `checkp(qv, "'{1}");

    qwv = qw.min;
    `checkp(qwv, "'{1}");

    points_qv = points_q.min(p) with (p.x + p.y);
    if (points_qv[0].x != 1 || points_qv[0].y != 2) $stop;

    qv = q.max;
    `checkp(qv, "'{4}");
    points_qv = points_q.max(p) with (p.x + p.y);
    if (points_qv[0].x != 2 || points_qv[0].y != 4) $stop;

    qv = qe.min;
    `checkp(qv, "'{}");
    qv = qe.min(x) with (x + 1);
    `checkp(qv, "'{}");
    qv = qe.max;
    `checkp(qv, "'{}");
    qv = qe.max(x) with (x + 1);
    `checkp(qv, "'{}");

    // Wide
    qwv = qwe.min;
    `checkp(qwv, "'{}");
    qwv = qwe.max;
    `checkp(qwv, "'{}");

    // Reduction methods
    i = q.sum;
    `checkh(i, 32'hc);
    i = q.sum with (item + 1);
    `checkh(i, 32'h11);
    i = q.product;
    `checkh(i, 32'h30);
    i = q.product with (item + 1);
    `checkh(i, 32'h168);

    // Wide
    w = qw.sum;
    `checkh(w, 230'hc);
    w = qw.product;
    `checkh(w, 230'h30);

    i = qe.sum;
    `checkh(i, 32'h0);
    i = qe.sum with (item + 1);
    `checkh(i, 32'h0);
    i = qe.product;
    `checkh(i, 32'h0);
    i = qe.product with (item + 1);
    `checkh(i, 32'h0);

    // Wide
    w = qwe.sum;
    `checkh(w, 230'h0);
    w = qwe.product;
    `checkh(w, 230'h0);

    q = '{10: 32'b1100, 11: 32'b1010};
    i = q.and;
    `checkh(i, 32'b1000);
    i = q.and with (item + 1);
    `checkh(i, 32'b1001);
    i = q.or;
    `checkh(i, 32'b1110);
    i = q.or with (item + 1);
    `checkh(i, 32'b1111);
    i = q.xor;
    `checkh(i, 32'b0110);
    i = q.xor with (item + 1);
    `checkh(i, 32'b0110);

    qw = '{10: 230'b1100, 11: 230'b1010};
    w = qw.and;
    `checkh(w, 230'b1000);
    w = qw.or;
    `checkh(w, 230'b1110);
    w = qw.xor;
    `checkh(w, 230'b0110);

    i = qe.and;
    `checkh(i, 32'b0);
    i = qe.and with (item + 1);
    `checkh(i, 32'h0);
    i = qe.or;
    `checkh(i, 32'b0);
    i = qe.or with (item + 1);
    `checkh(i, 32'b0);
    i = qe.xor;
    `checkh(i, 32'b0);
    i = qe.xor with (item + 1);
    `checkh(i, 32'b0);

    // Wide
    w = qwe.and;
    `checkh(w, 230'b0);
    w = qwe.or;
    `checkh(w, 230'b0);
    w = qwe.xor;
    `checkh(w, 230'b0);

    i = q.and();
    `checkh(i, 32'b1000);
    i = q.and() with (item + 1);
    `checkh(i, 32'b1001);
    i = q.or();
    `checkh(i, 32'b1110);
    i = q.or() with (item + 1);
    `checkh(i, 32'b1111);
    i = q.xor();
    `checkh(i, 32'b0110);
    i = q.xor() with (item + 1);
    `checkh(i, 32'b0110);

    // Wide
    w = qw.and();
    `checkh(w, 230'b1000);
    w = qw.or();
    `checkh(w, 230'b1110);
    w = qw.xor();
    `checkh(w, 230'b0110);

    i = qe.and();
    `checkh(i, 32'b0);
    i = qe.or();
    `checkh(i, 32'b0);
    i = qe.xor();
    `checkh(i, 32'b0);

    // Wide
    w = qwe.and();
    `checkh(w, 230'b0);
    w = qwe.or();
    `checkh(w, 230'b0);
    w = qwe.xor();
    `checkh(w, 230'b0);

    q = '{10: 1, 11: 2};
    qe = '{10: 1, 11: 2};
    `checkh(q == qe, 1'b1);
    `checkh(q != qe, 1'b0);

    i = points_q.sum with (vec_len_squared(item));
    `checkh(i, 32'h2a);
    i = points_q.product with (vec_len_squared(item));
    `checkh(i, 32'h6a4);
    b = points_q.sum with (vec_len_squared(item) == 5);
    `checkh(b, 1'b1);
    b = points_q.sum with (vec_len_squared(item) == 0);
    `checkh(b, 1'b0);
    b = points_q.product with (vec_len_squared(item) inside {5, 17});
    `checkh(b, 1'b0);
    b = points_q.sum with (vec_len_squared(item) inside {5, 17, 20});
    `checkh(b, 1'b1);

    // Map method (IEEE 1800-2023 7.12.5)
    q = '{1: 100, 2: 200, 3: 300};
    qv = q.map(el) with (el / 100);
    `checkp(qv, "'{1, 2, 3}");
    qv = q.map(el) with (el.index * 10);
    `checkp(qv, "'{10, 20, 30}");

    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule
