// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2009-2017 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "verilated_cov.h"
#include "verilated_covergroup.h"

#include "svdpi.h"

#include <cstdio>
#include <cstring>
#include <iostream>

// These require the above. Comment prevents clang-format moving them
#include "TestCheck.h"

#include VM_PREFIX_INCLUDE

//======================================================================

double sc_time_stamp() { return 0; }

int errors = 0;

//======================================================================

const char* name() { return "main"; }

void hier_insert(VerilatedCovContext* covContextp, uint64_t* countp, const char* hierp,
                 const char* peri) {
    // This needs to be a function at one line number so all of the
    // line numbers for coverage are constant, otherwise instances won't combine.
    VL_COVER_INSERT(covContextp, name(), countp, "hier", hierp, "per_instance", peri);
}

int main() {
    uint32_t covers[1];
    uint64_t coverw[6];

    VerilatedCovContext* covContextp = Verilated::defaultContextp()->coveragep();

    VL_COVER_INSERT(covContextp, name(), &covers[0], "comment", "kept_one");
    VL_COVER_INSERT(covContextp, name(), &coverw[0], "comment", "kept_two");
    VL_COVER_INSERT(covContextp, name(), &coverw[1], "comment", "lost_three");

    hier_insert(covContextp, &coverw[2], "top.a0.pi", "0");
    hier_insert(covContextp, &coverw[3], "top.a1.pi", "0");
    hier_insert(covContextp, &coverw[4], "top.a0.npi", "1");
    hier_insert(covContextp, &coverw[5], "top.a1.npi", "1");

    covers[0] = 100;
    coverw[0] = 210;
    coverw[1] = 220;

    coverw[2] = 200;
    coverw[3] = 300;
    coverw[4] = 200;
    coverw[5] = 300;

#ifdef T_COVER_LIB
    TEST_CHECK_EQ(covContextp->defaultFilename(), "coverage.dat");
    covContextp->write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage1.dat");
    covContextp->forcePerInstance(true);
    covContextp->write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage1_per_instance.dat");
    covContextp->forcePerInstance(false);
    covContextp->clearNonMatch("kept_");
    covContextp->write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage2.dat");
    covContextp->zero();
    covContextp->write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage3.dat");
    covContextp->clear();
    Verilated::defaultContextp()->coverageFilename(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage4.dat");
    TEST_CHECK_EQ(covContextp->defaultFilename(), VL_STRINGIFY(TEST_OBJ_DIR) "/coverage4.dat");
    covContextp->write();  // Uses defaultFilename()
#elif defined(T_COVER_LIB_LEGACY)
    TEST_CHECK_EQ(VerilatedCov::defaultFilename(), "coverage.dat");
    VerilatedCov::write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage1.dat");
    VerilatedCov::clearNonMatch("kept_");
    VerilatedCov::write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage2.dat");
    VerilatedCov::zero();
    VerilatedCov::write(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage3.dat");
    VerilatedCov::clear();
    Verilated::defaultContextp()->coverageFilename(VL_STRINGIFY(TEST_OBJ_DIR) "/coverage4.dat");
    TEST_CHECK_EQ(VerilatedCov::defaultFilename(), VL_STRINGIFY(TEST_OBJ_DIR) "/coverage4.dat");
    VerilatedCov::write();  // Uses defaultFilename()
#else
#error
#endif

    // Cross-bin names and coverage are also available through the runtime read interface.
    VlCoverpointT<1> a;
    VlCoverpointT<1> b;
    a.init("a", 1, 2);
    a.addArrayNamer(VlCovBinKind::KIND_NORMAL, 2, "a", __FILE__, __LINE__, 1);
    b.init("b", 1, 2);
    b.addArrayNamer(VlCovBinKind::KIND_NORMAL, 2, "b", __FILE__, __LINE__, 1);
    VlCoverpoint* const cps[] = {&a, &b};

    VlCoverCross automatic;
    automatic.init("automatic", 2, cps, __FILE__, __LINE__, 1);
    TEST_CHECK_EQ(automatic.binCount(), 4);
    TEST_CHECK_EQ(automatic.binName(0), "a[0]_x_b[0]");
    TEST_CHECK_EQ(automatic.binName(3), "a[1]_x_b[1]");

    VlCoverCross selected;
    selected.init("selected", 2, cps, __FILE__, __LINE__, 1);
    selected.addBin(0, 0, 1, "named", __FILE__, __LINE__, 1);
    selected.finalizeBins();
    const VlCoverpointIf& view = selected;
    TEST_CHECK_EQ(view.binCount(), 3);
    TEST_CHECK_EQ(view.binName(0), "named");
    TEST_CHECK_EQ(view.binName(1), "a[1]_x_b[0]");
    TEST_CHECK_EQ(view.binName(2), "a[1]_x_b[1]");

    a.incrementBin(0);
    b.incrementBin(0);
    selected.sample(cps);
    double covered = 0;
    double total = 0;
    view.coverageParts(covered, total);
    TEST_CHECK_EQ(covered, 1);
    TEST_CHECK_EQ(total, 3);

    a.clearHitList();
    a.incrementBin(1);
    selected.sample(cps);
    view.coverageParts(covered, total);
    TEST_CHECK_EQ(covered, 2);
    TEST_CHECK_EQ(total, 3);

    printf("*-* All Finished *-*\n");
    return (errors ? 10 : 0);
}
