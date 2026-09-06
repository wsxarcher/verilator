// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
//
// Code available from: https://verilator.org
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2024-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=============================================================================
///
/// \file
/// \brief Verilated functional-coverage collection runtime implementation
///
/// Linked when covergroups are present.  The coverage-database registration
/// is compiled only with "verilator --coverage".
///
//=============================================================================

#include "verilatedos.h"

#include "verilated_covergroup.h"

#include "verilated.h"

// This file is compiled whenever covergroups are used, with or without
// "verilator --coverage" (see V3Global::verilatedCppFiles).  Bin counts are
// members of the covergroup objects themselves, so sampling, bin naming, and
// coverage queries such as get_inst_coverage() all work with no coverage
// database present.  VL_COVER_INSERT does not copy a count; it hands the
// database the address of a counter to read at write time.  Only that
// publication step needs verilated_cov.cpp, which is compiled solely under
// --coverage, so only the registerBins() bodies are gated on VM_COVERAGE.
#if VM_COVERAGE
#include "verilated_cov.h"
#endif

void VlCoverpoint::init(const char* hier, uint32_t atLeast, uint32_t nBins) {
    m_hier = hier;
    m_atLeast = atLeast;
    m_total = nBins;
    m_counts.assign(nBins, 0);
    m_crossIdx.assign(nBins, -1);
    m_crossToBin.clear();
}

void VlCoverpoint::addNamer(VlCovBinKind set, uint32_t count, VlCovBinNaming naming,
                            const char* name, const char* file, int line, int col) {
    m_namers.emplace_back(set, count, m_nextBase, naming, name, file, line, col);
    if (set == VlCovBinKind::KIND_NORMAL) {
        // Assign each Normal bin a cross index, and record the inverse map.
        for (uint32_t b = m_nextBase; b < m_nextBase + count; ++b) {
            m_crossIdx[b] = static_cast<int>(m_crossToBin.size());
            m_crossToBin.push_back(b);
        }
        m_normal += count;
    }
    m_nextBase += count;
}

std::string VlCoverpoint::normalBinName(uint32_t crossIdx) const {
    // Build the bin name based on the bin index
    return binName(m_crossToBin[crossIdx]);
}

const VlCovNamer& VlCoverpoint::namerFor(uint32_t i) const {
    // Namers are appended in ascending order covering [0, m_total),
    for (const VlCovNamer& nm : m_namers) {
        if (i < nm.base() + nm.count()) return nm;
    }
    VL_UNREACHABLE;  // LCOV_EXCL_LINE
}

std::string VlCoverpoint::binName(uint32_t i) const {
    const VlCovNamer& nm = namerFor(i);
    std::string name = nm.name();
    if (nm.naming() == VlCovBinNaming::Array) name += '[' + std::to_string(i - nm.base()) + ']';
    return name;
}

#if VM_COVERAGE
void VlCoverpoint::registerBins(VerilatedCovContext* covcontextp, const char* page) {
    for (uint32_t i = 0; i < binCount(); ++i) {
        const VlCovNamer& nm = namerFor(i);
        const VlCovBinKind kind = binKind(i);
        const std::string binp = binName(i);
        const std::string full = m_hier + "." + binp;
        const std::string lineStr = std::to_string(nm.line());
        const std::string colStr = std::to_string(nm.col());
        if (kind == VlCovBinKind::KIND_NORMAL) {
            VL_COVER_INSERT(covcontextp, full.c_str(), &m_counts[i], "page", page, "filename",
                            nm.file(), "lineno", lineStr.c_str(), "column", colStr.c_str(), "bin",
                            binp.c_str());
        } else {
            const char* const binType = kind == VlCovBinKind::KIND_IGNORE    ? "ignore"
                                        : kind == VlCovBinKind::KIND_ILLEGAL ? "illegal"
                                                                             : "default";
            VL_COVER_INSERT(covcontextp, full.c_str(), &m_counts[i], "page", page, "filename",
                            nm.file(), "lineno", lineStr.c_str(), "column", colStr.c_str(), "bin",
                            binp.c_str(), "bin_type", binType);
        }
    }
}
#endif  // VM_COVERAGE

//=============================================================================
// VlCoverCross

void VlCoverCross::init(const char* hier, uint32_t dims, VlCoverpoint* const* cps,
                        const char* file, int line, int col) {
    m_hier = hier;
    m_file = file;
    m_line = line;
    m_col = col;
    m_dims = dims;
    m_cps.assign(cps, cps + dims);
    m_cpBinCounts.resize(dims);
    // Accumulate in 64 bits so the overflow check itself cannot overflow.
    uint64_t product = 1;
    for (uint32_t d = 0; d < dims; ++d) {
        m_cpBinCounts[d] = cps[d]->normalBinCount();
        product *= m_cpBinCounts[d];
        if (VL_UNLIKELY(product > UINT32_MAX)) {  // LCOV_EXCL_START
            VL_FATAL_MT(file, line, "", "Cross has too many auto bins to represent");
        }  // LCOV_EXCL_STOP
    }
    m_numAutoBins = static_cast<uint32_t>(product);
    // stride[d] = product of the Normal bin counts of all dimensions after d.
    // Counts down with an offset so the unsigned index never wraps below zero.
    m_stride.assign(dims, 1);
    for (uint32_t d = dims; d > 1; --d) m_stride[d - 2] = m_stride[d - 1] * m_cpBinCounts[d - 1];
    m_flatCounts.assign(m_numAutoBins, 0);
}

void VlCoverCross::addBin(uint32_t dim, uint32_t first, uint32_t bins, const char* name,
                          const char* file, int line, int col) {
    if (!m_numAutoBins) return;  // An empty product creates no cross bin.
    if (m_bins.empty()) m_autoExcluded.assign(m_numAutoBins, false);
    m_bins.emplace_back(dim, first, bins, name, file, line, col);
    // Visit only selected tuples. Multiple explicit bins may select the same tuple.
    const uint64_t stride = m_stride[dim];
    const uint64_t period = stride * m_cpBinCounts[dim];
    for (uint64_t base = first * stride; base < m_numAutoBins; base += period) {
        for (uint64_t flat = base; flat < base + bins * stride; ++flat) {
            m_autoExcluded[flat] = true;
        }
    }
}

void VlCoverCross::finalizeBins() {
    for (uint32_t flat = 0; flat < m_numAutoBins; ++flat) {
        if (!m_autoExcluded[flat]) m_autoBins.push_back(flat);
    }
}

void VlCoverCross::iterateProduct(VlCoverpoint* const* cps, uint32_t dim, uint32_t baseIdx) {
    const uint32_t hits = cps[dim]->hitCount();
    const uint32_t* const list = cps[dim]->hitList();
    const bool last = (dim == m_dims - 1);
    const uint32_t stride = m_stride[dim];
    for (uint32_t hit = 0; hit < hits; ++hit) {
        const uint32_t idx = baseIdx + list[hit] * stride;
        if (last) {
            incrementTuple(idx);
        } else {
            iterateProduct(cps, dim + 1, idx);
        }
    }
}

void VlCoverCross::sample(VlCoverpoint* const* cps, const bool* binIffs) {
    // Fast path: if any dimension had no Normal-bin hit, the cross cannot hit.
    for (uint32_t d = 0; d < m_dims; ++d) {
        if (cps[d]->hitCount() == 0) return;
    }
    for (uint32_t b = 0; b < m_bins.size(); ++b) {
        if (binIffs && !binIffs[b]) continue;
        Bin& bin = m_bins[b];
        const VlCoverpoint* const cpp = cps[bin.dim];
        for (uint32_t hit = 0; hit < cpp->hitCount(); ++hit) {
            const uint32_t idx = cpp->hitList()[hit];
            if (idx >= bin.first && idx - bin.first < bin.bins) {
                if (bin.count++ == 0) ++m_numCovered;
                break;
            }
        }
    }
    iterateProduct(cps, 0, 0);
}

std::string VlCoverCross::binName(uint32_t i) const {
    if (i < m_bins.size()) return m_bins[i].name;
    return autoBinName(autoIndex(i - static_cast<uint32_t>(m_bins.size())));
}

std::string VlCoverCross::autoBinName(uint32_t flat) const {
    // Built on demand by concatenating each coverpoint's own bin name.
    std::string name;
    for (uint32_t d = 0; d < m_dims; ++d) {
        const uint32_t crossIdx = (flat / m_stride[d]) % m_cpBinCounts[d];
        if (d > 0) name += "_x_";
        name += m_cps[d]->normalBinName(crossIdx);
    }
    return name;
}

#if VM_COVERAGE
void VlCoverCross::registerBins(VerilatedCovContext* covcontextp, const char* page) {
    for (Bin& bin : m_bins) {
        const std::string full = m_hier + "." + bin.name;
        const std::string lineStr = std::to_string(bin.line);
        const std::string colStr = std::to_string(bin.col);
        VL_COVER_INSERT(covcontextp, full.c_str(), &bin.count, "page", page, "filename", bin.file,
                        "lineno", lineStr.c_str(), "column", colStr.c_str(), "bin", bin.name,
                        "cross", "1");
    }
    // Register retained automatic bins, including those with zero hits.
    const std::string lineStr = std::to_string(m_line);
    const std::string colStr = std::to_string(m_col);
    const uint32_t autoCount = binCount() - static_cast<uint32_t>(m_bins.size());
    for (uint32_t i = 0; i < autoCount; ++i) {
        const uint32_t flat = autoIndex(i);
        const std::string bin = autoBinName(flat);  // "b1_x_b2_x_..."
        // cross_bins metadata: the same components joined by ',' (not read by the report)
        std::string crossBins;
        for (uint32_t d = 0; d < m_dims; ++d) {
            const uint32_t crossIdx = (flat / m_stride[d]) % m_cpBinCounts[d];
            if (d > 0) crossBins += ",";
            crossBins += m_cps[d]->normalBinName(crossIdx);
        }
        const std::string full = m_hier + "." + bin;
        VL_COVER_INSERT(covcontextp, full.c_str(), &m_flatCounts[flat], "page", page, "filename",
                        m_file, "lineno", lineStr.c_str(), "column", colStr.c_str(), "bin",
                        bin.c_str(), "cross", "1", "cross_bins", crossBins.c_str());
    }
}
#endif  // VM_COVERAGE
