// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Functional coverage implementation
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// FUNCTIONAL COVERAGE TRANSFORMATIONS:
//      For each covergroup (AstClass with isCovergroup()):
//          For each coverpoint (AstCoverpoint):
//              Generate member variable for VerilatedCoverpoint
//              Generate initialization in constructor
//              Generate sample code in sample() method
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Covergroup.h"

#include "V3Const.h"
#include "V3MemberMap.h"

#include <set>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Functional coverage visitor

class FunctionalCoverageVisitor final : public VNVisitor {
    // NODE STATE
    // Entire netlist:
    //  AstCoverpoint::user1p()  -> AstVar*.  Previous-value variable for transition bins
    const VNUser1InUse m_inuser1;

    // STATE
    AstClass* m_covergroupp = nullptr;  // Current covergroup being processed
    AstFunc* m_sampleFuncp = nullptr;  // Current sample() function
    AstFunc* m_constructorp = nullptr;  // Current constructor
    std::vector<AstCoverpoint*> m_coverpoints;  // Coverpoints in current covergroup
    std::map<std::string, AstCoverpoint*> m_coverpointMap;  // Name -> coverpoint for fast lookup
    std::vector<AstCoverCross*> m_coverCrosses;  // Cross coverage items in current covergroup

    // Structure to track bins with their variables and options
    struct BinInfo final {
        AstCoverBin* binp;
        AstVar* varp;
        int atLeast;  // Minimum hits required for coverage (from option.at_least)
        AstCoverpoint* coverpointp;  // Associated coverpoint (or nullptr for cross bins)
        AstCoverCross* crossp;  // Associated cross (or nullptr for coverpoint bins)
        string reportName;  // Coverage database bin name, defaults to binp->name()
        string crossBins;  // For cross bins: comma-separated individual bin names, in order
        BinInfo(AstCoverBin* b, AstVar* v, int al = 1, AstCoverpoint* cp = nullptr,
                AstCoverCross* cr = nullptr, const string& rn = "", const string& cb = "")
            : binp{b}
            , varp{v}
            , atLeast{al}
            , coverpointp{cp}
            , crossp{cr}
            , reportName{rn}
            , crossBins{cb} {}
    };
    std::vector<BinInfo> m_binInfos;  // All bins in current covergroup
    std::set<std::string> m_crossedCpNames;  // Coverpoints referenced by a cross (kept legacy)
    std::vector<AstVar*> m_convCpVars;  // VlCoverpoint members of converted coverpoints
    AstCDType* m_vlCoverpointDTypep = nullptr;  // Shared "VlCoverpoint" C++ member type

    VMemberMap m_memberMap;  // Member names cached for fast lookup

    // METHODS
    void clearBinInfos() {
        // Delete pseudo-bins created for cross coverage (they're never inserted into the AST)
        for (const BinInfo& bi : m_binInfos) {
            if (!bi.coverpointp) pushDeletep(bi.binp);
        }
        m_binInfos.clear();
    }

    void processCovergroup() {
        UINFO(4, "Processing covergroup: " << m_covergroupp->name() << " with "
                                           << m_coverpoints.size() << " coverpoints and "
                                           << m_coverCrosses.size() << " crosses");

        // Clear bin info for this covergroup (deleting any orphaned cross pseudo-bins)
        clearBinInfos();

        // Coverpoints referenced by a cross keep the legacy per-bin-member path (the cross
        // reads those members); collect their names before they are consumed by the cross.
        m_crossedCpNames.clear();
        m_convCpVars.clear();
        for (AstCoverCross* crossp : m_coverCrosses) {
            for (AstNode* itemp = crossp->itemsp(); itemp; itemp = itemp->nextp()) {
                if (const AstCoverpointRef* const refp = VN_CAST(itemp, CoverpointRef))
                    m_crossedCpNames.insert(refp->name());
            }
        }

        // For each coverpoint, generate sampling code
        for (AstCoverpoint* cpp : m_coverpoints) generateCoverpointCode(cpp);

        // For each cross, generate sampling code
        for (AstCoverCross* crossp : m_coverCrosses) generateCrossCode(crossp);

        // Generate coverage computation code (even for empty covergroups)
        generateCoverageComputationCode();

        // TODO: Generate instance registry infrastructure for static get_coverage()
        // This requires:
        // - Static registry members (t_instances, s_mutex)
        // - registerInstance() / unregisterInstance() methods
        // - Proper C++ emission in EmitC backend
        // For now, get_coverage() returns 0.0 (placeholder)

        // Generate coverage database registration if coverage is enabled
        if (v3Global.opt.coverage()) generateCoverageRegistration();

        // Clean up orphaned cross pseudo-bins now that we're done with them
        clearBinInfos();
    }

    static constexpr int COVER_BINS_LIMIT
        = 1000;  // Sanity limit to avoid hangs from e.g. signed underflow
    static constexpr uint64_t ARRAY_BIN_VALUE_LIMIT
        = 1ULL << 16;  // Maximum explicitly generated value bins from one array bin

    void expandAutomaticBins(AstCoverpoint* coverpointp, AstNodeExpr* exprp) {
        // Find and expand any automatic bins
        AstNode* prevBinp = nullptr;
        for (AstNode* binp = coverpointp->binsp(); binp;) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            AstNode* const nextBinp = binp->nextp();

            if (cbinp->binsType() == VCoverBinsType::BINS_AUTO) {
                UINFO(4, "  Expanding automatic bin: " << cbinp->name());

                // Get array size - must be a constant
                AstNodeExpr* const sizep = cbinp->arraySizep();

                // Evaluate as constant.  On any error the invalid AUTO bin is removed so the
                // generic array-bin size validation in fixedArrayBinCount() does not re-report it.
                const AstConst* constp = VN_CAST(sizep, Const);
                if (!constp) {
                    cbinp->v3error("Automatic bins array size must be a constant.");
                    VL_DO_DANGLING(pushDeletep(binp->unlinkFrBack()), binp);
                    binp = nextBinp;
                    continue;
                }

                const int numBins = constp->toSInt();
                if (numBins <= 0) {
                    cbinp->v3error("Automatic bins array size must be >= 1, got " << numBins
                                                                                  << ".");
                    VL_DO_DANGLING(pushDeletep(binp->unlinkFrBack()), binp);
                    binp = nextBinp;
                    continue;
                }
                if (numBins > COVER_BINS_LIMIT) {
                    cbinp->v3error("Automatic bins array size of "
                                   << numBins << " exceeds limit of " << COVER_BINS_LIMIT << ".");
                    VL_DO_DANGLING(pushDeletep(binp->unlinkFrBack()), binp);
                    binp = nextBinp;
                    continue;
                }

                // Calculate range division
                const int width = exprp->width();
                const uint64_t maxVal = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
                // For width >= 64: (maxVal+1) would overflow; compute binSize without overflow
                const uint64_t binSize
                    = (width < 64) ? ((maxVal + 1) / numBins) : (UINT64_MAX / numBins + 1);

                UINFO(4, "    Width=" << width << " maxVal=" << maxVal << " numBins=" << numBins
                                      << " binSize=" << binSize);

                // Create expanded bins
                for (int i = 0; i < numBins; i++) {
                    const uint64_t lo = static_cast<uint64_t>(i) * binSize;
                    const uint64_t hi = (i == numBins - 1) ? maxVal : ((i + 1) * binSize - 1);

                    // Create constants for range (use setQuad to handle values > 32-bit)
                    V3Number loNum{cbinp->fileline(), width, 0};
                    loNum.setQuad(lo);
                    AstConst* const loConstp = new AstConst{cbinp->fileline(), loNum};
                    V3Number hiNum{cbinp->fileline(), width, 0};
                    hiNum.setQuad(hi);
                    AstConst* const hiConstp = new AstConst{cbinp->fileline(), hiNum};

                    // Create InsideRange [lo:hi]
                    AstInsideRange* const rangep
                        = new AstInsideRange{cbinp->fileline(), loConstp, hiConstp};
                    rangep->dtypeFrom(exprp);  // Set dtype from coverpoint expression

                    // Create new bin
                    const string binName = cbinp->name() + "[" + std::to_string(i) + "]";
                    AstCoverBin* const newBinp
                        = new AstCoverBin{cbinp->fileline(), binName, rangep, false, false};

                    // Insert after previous bin
                    if (prevBinp) {
                        prevBinp->addNext(newBinp);
                    } else {
                        coverpointp->addBinsp(newBinp);
                    }
                    prevBinp = newBinp;
                }

                // Remove the AUTO bin from the list
                VL_DO_DANGLING(pushDeletep(binp->unlinkFrBack()), binp);
            } else {
                prevBinp = binp;
            }

            binp = nextBinp;
        }
    }

    // Extract all coverpoint option values in a single pass.
    // atLeastOut: option.at_least (default 1)
    // autoBinMaxOut: option.auto_bin_max (coverpoint overrides covergroup, default 64)
    void extractCoverpointOptions(AstCoverpoint* coverpointp, int& atLeastOut,
                                  int& autoBinMaxOut) {
        atLeastOut = 1;
        autoBinMaxOut = -1;  // -1 = not set at coverpoint level
        for (AstNode* optionp = coverpointp->optionsp(); optionp; optionp = optionp->nextp()) {
            AstCoverOption* const optp = VN_AS(optionp, CoverOption);
            AstConst* const constp = VN_CAST(optp->valuep(), Const);
            if (!constp) {
                optp->valuep()->v3warn(COVERIGN, "Ignoring unsupported: non-constant 'option."
                                                     << optp->optionType().ascii()
                                                     << "'; using default value");
                continue;
            }
            if (optp->optionType() == VCoverOptionType::AT_LEAST) {
                atLeastOut = constp->toSInt();
            } else {
                // V3LinkParse only converts at_least/auto_bin_max coverpoint options into
                // AstCoverOption (others are dropped there), so this is the only alternative.
                UASSERT_OBJ(optp->optionType() == VCoverOptionType::AUTO_BIN_MAX, optp,
                            "Unexpected coverpoint option type reaching V3Covergroup");
                autoBinMaxOut = constp->toSInt();
            }
        }
        // Fall back to covergroup-level auto_bin_max if not set at coverpoint level
        if (autoBinMaxOut < 0) {
            if (m_covergroupp->cgAutoBinMax() >= 0) {
                autoBinMaxOut = m_covergroupp->cgAutoBinMax();
            } else {
                autoBinMaxOut = 64;  // Default per IEEE 1800-2023 Table 19-1
            }
        }
    }

    // Extract individual values from a range expression list, used only to carve values
    // out of implicit auto-bins.  Iterates over all siblings (nextp) in the list, handling
    // AstConst (single value) and AstInsideRange ([lo:hi]); an open-ended bound ('$',
    // AstUnbounded) resolves to the coverpoint domain min (lower) or max (upper, == maxVal).
    void extractValuesFromRange(AstNode* nodep, std::set<uint64_t>& values, uint64_t maxVal) {
        // Cap enumeration so a '$'-bounded or otherwise huge range cannot blow up memory;
        // auto-bins are per-value only for small domains, so a partial set is harmless here.
        constexpr size_t maxEnumerate = 1ULL << 16;
        for (AstNode* np = nodep; np; np = np->nextp()) {
            if (AstConst* constp = VN_CAST(np, Const)) {
                if (constp->num().isFourState())
                    continue;  // wildcard patterns can't be enumerated
                values.insert(constp->toUQuad());
            } else if (AstInsideRange* rangep = VN_CAST(np, InsideRange)) {
                AstNodeExpr* const lhsp = V3Const::constifyEdit(rangep->lhsp());
                AstNodeExpr* const rhsp = V3Const::constifyEdit(rangep->rhsp());
                const bool loUnbounded = VN_IS(lhsp, Unbounded);
                const bool hiUnbounded = VN_IS(rhsp, Unbounded);
                AstConst* const loConstp = VN_CAST(lhsp, Const);
                AstConst* const hiConstp = VN_CAST(rhsp, Const);
                if ((!loConstp && !loUnbounded) || (!hiConstp && !hiUnbounded)) {
                    rangep->v3error("Non-constant expression in bin range; "
                                    "range bounds must be constants");
                    continue;
                }
                if ((loConstp && loConstp->num().isFourState())
                    || (hiConstp && hiConstp->num().isFourState()))
                    continue;
                const uint64_t lo = loUnbounded ? 0 : loConstp->toUQuad();
                const uint64_t hi = hiUnbounded ? maxVal : hiConstp->toUQuad();
                for (uint64_t v = lo; v <= hi; v++) {
                    if (values.size() >= maxEnumerate) break;
                    values.insert(v);
                }
            } else {
                np->v3error("Non-constant expression in bin value list; values must be constants");
            }
        }
    }

    // Single-pass categorization: determine whether any regular (non-ignore/illegal) bins exist
    // and collect the set of excluded values from ignore/illegal bins.
    void categorizeBins(AstCoverpoint* coverpointp, bool& hasRegularOut,
                        std::set<uint64_t>& excludedOut, uint64_t maxVal) {
        hasRegularOut = false;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            const VCoverBinsType btype = cbinp->binsType();
            if (btype == VCoverBinsType::BINS_IGNORE || btype == VCoverBinsType::BINS_ILLEGAL) {
                if (AstNode* rangep = cbinp->rangesp()) {
                    extractValuesFromRange(rangep, excludedOut, maxVal);
                }
            } else {
                hasRegularOut = true;
            }
        }
    }

    // Create implicit automatic bins when coverpoint has no explicit regular bins
    void createImplicitAutoBins(AstCoverpoint* coverpointp, AstNodeExpr* exprp, int autoBinMax) {
        const int width = exprp->width();
        const uint64_t maxVal = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);

        // Single pass: check for regular bins and collect excluded values simultaneously.
        // maxVal resolves any '$' (open-ended) bound in ignore_bins/illegal_bins ranges.
        bool hasRegular = false;
        std::set<uint64_t> excluded;
        categorizeBins(coverpointp, hasRegular, excluded, maxVal);

        // If already has regular bins, nothing to do
        if (hasRegular) return;

        UINFO(4, "  Creating implicit automatic bins for coverpoint: " << coverpointp->name());

        const uint64_t numTotalValues = (width >= 64) ? UINT64_MAX : (1ULL << width);
        const uint64_t numValidValues = numTotalValues - excluded.size();

        // Determine number of bins to create (based on non-excluded values)
        int numBins;
        if (numValidValues <= static_cast<uint64_t>(autoBinMax)) {
            // Create one bin per valid value
            numBins = numValidValues;
        } else {
            // Create autoBinMax bins, dividing range
            numBins = autoBinMax;
        }

        UINFO(4, "    Width=" << width << " numTotalValues=" << numTotalValues
                              << " numValidValues=" << numValidValues << " autoBinMax="
                              << autoBinMax << " creating " << numBins << " bins");

        // Strategy: Create bins for each value (if numValidValues <= autoBinMax)
        // or create range bins that avoid excluded values
        if (numValidValues <= static_cast<uint64_t>(autoBinMax)) {
            // Create one bin per valid value
            int binCount = 0;
            for (uint64_t v = 0; v <= maxVal && binCount < numBins; v++) {
                // Skip excluded values
                if (excluded.find(v) != excluded.end()) continue;

                // Create single-value bin
                AstConst* const valConstp = new AstConst{
                    coverpointp->fileline(), V3Number(coverpointp->fileline(), width, v)};
                AstConst* const valConstp2 = new AstConst{
                    coverpointp->fileline(), V3Number(coverpointp->fileline(), width, v)};

                AstInsideRange* const rangep
                    = new AstInsideRange{coverpointp->fileline(), valConstp, valConstp2};
                rangep->dtypeFrom(exprp);

                const string binName = "auto_" + std::to_string(binCount);
                AstCoverBin* const newBinp
                    = new AstCoverBin{coverpointp->fileline(), binName, rangep, false, false};

                coverpointp->addBinsp(newBinp);
                binCount++;
            }
            UINFO(4, "    Created " << binCount << " single-value automatic bins");
        } else {
            // Create range bins (more complex - need to handle excluded values in ranges)
            // For simplicity, create bins and let excluded values not match any bin
            const uint64_t binSize = (maxVal + 1) / numBins;

            for (int i = 0; i < numBins; i++) {
                const uint64_t lo = i * binSize;
                const uint64_t hi = (i == numBins - 1) ? maxVal : ((i + 1) * binSize - 1);

                // Create constants for range
                AstConst* const loConstp = new AstConst{
                    coverpointp->fileline(), V3Number(coverpointp->fileline(), width, lo)};
                AstConst* const hiConstp = new AstConst{
                    coverpointp->fileline(), V3Number(coverpointp->fileline(), width, hi)};

                // Create InsideRange [lo:hi]
                AstInsideRange* const rangep
                    = new AstInsideRange{coverpointp->fileline(), loConstp, hiConstp};
                rangep->dtypeFrom(exprp);

                // Create bin name
                const string binName = "auto_" + std::to_string(i);
                AstCoverBin* const newBinp
                    = new AstCoverBin{coverpointp->fileline(), binName, rangep, false, false};

                // Add to coverpoint
                coverpointp->addBinsp(newBinp);
            }

            UINFO(4, "    Created range-based automatic bins");
        }
    }

    // Sanitize generated names to be valid C++ identifiers
    static string sanitizeGeneratedName(string name) {
        std::replace(name.begin(), name.end(), '[', '_');
        std::replace(name.begin(), name.end(), ']', '_');
        std::replace(name.begin(), name.end(), '-', '_');
        std::replace(name.begin(), name.end(), ':', '_');
        std::replace(name.begin(), name.end(), '>', '_');
        std::replace(name.begin(), name.end(), '=', '_');
        std::replace(name.begin(), name.end(), ' ', '_');
        return name;
    }

    AstVar* createCoverageCounterVar(FileLine* fl, const string& varName, AstNodeDType* dtypep) {
        AstVar* const varp = new AstVar{fl, VVarType::MEMBER, varName, dtypep};
        varp->isStatic(false);
        varp->valuep(new AstConst{fl, AstConst::WidthedValue{}, 32, 0});
        m_covergroupp->addMembersp(varp);
        return varp;
    }

    AstVar* createTrackedCoverpointBinCounter(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                              const string& generatedBinName, int atLeastValue,
                                              const string& logPrefix,
                                              const string& logSuffix = "",
                                              const string& reportName = "") {
        const string varName = "__Vcov_" + coverpointp->name() + "_" + generatedBinName;
        AstVar* const varp
            = createCoverageCounterVar(binp->fileline(), varName, binp->findUInt32DType());
        UINFO(4, "    " << logPrefix << ": " << varName << logSuffix);
        m_binInfos.push_back(BinInfo(binp, varp, atLeastValue, coverpointp, nullptr, reportName));
        return varp;
    }

    AstNodeExpr* applyCoverpointIffCondition(AstCoverpoint* coverpointp, FileLine* fl,
                                             AstNodeExpr* condp) {
        if (AstNodeExpr* const iffp = coverpointp->iffp()) {
            UINFO(6, "      Adding iff condition");
            condp = new AstAnd{fl, iffp->cloneTree(false), condp};
        }
        return condp;
    }

    // AND a bin's own 'iff (expr)' guard (IEEE 1800-2023 19.5) into its hit condition.
    AstNodeExpr* applyBinIffCondition(AstCoverBin* binp, FileLine* fl, AstNodeExpr* condp) {
        if (AstNodeExpr* const iffp = binp->iffp()) {
            condp = new AstAnd{fl, iffp->cloneTree(false), condp};
        }
        return condp;
    }

    // AND both the coverpoint's and the bin's 'iff' guards (IEEE 1800-2023 19.5) into a hit
    // condition: iff(coverpoint) && iff(bin) && condp.
    AstNodeExpr* applyIffGuards(AstCoverpoint* coverpointp, AstCoverBin* binp, FileLine* fl,
                                AstNodeExpr* condp) {
        return applyBinIffCondition(binp, fl, applyCoverpointIffCondition(coverpointp, fl, condp));
    }

    void addCoverpointBinHitIf(AstCoverpoint* coverpointp, AstCoverBin* binp, AstVar* hitVarp,
                               AstNodeExpr* condp, const string& illegalErrMsg,
                               const char* assertMsg) {
        AstNode* stmtp = makeBinHitIncrement(binp->fileline(), hitVarp);
        if (binp->binsType() == VCoverBinsType::BINS_ILLEGAL) {
            stmtp = stmtp->addNext(makeIllegalBinAction(binp->fileline(), illegalErrMsg));
        }

        AstIf* const ifp = new AstIf{binp->fileline(),
                                     applyIffGuards(coverpointp, binp, binp->fileline(), condp),
                                     stmtp, nullptr};
        UASSERT_OBJ(m_sampleFuncp, binp, assertMsg);
        m_sampleFuncp->addStmtsp(ifp);
    }

    // Create previous value variable for transition tracking
    AstVar* createPrevValueVar(AstCoverpoint* coverpointp, AstNodeExpr* exprp) {
        // Check if already created
        if (AstVar* const prevVarp = VN_CAST(coverpointp->user1p(), Var)) return prevVarp;

        // Create variable to store previous sampled value
        const string varName = "__Vprev_" + coverpointp->name();
        AstVar* prevVarp
            = new AstVar{coverpointp->fileline(), VVarType::MEMBER, varName, exprp->dtypep()};
        prevVarp->isStatic(false);
        m_covergroupp->addMembersp(prevVarp);

        UINFO(4, "    Created previous value variable: " << varName);

        // Initialize to zero in constructor
        AstNodeExpr* const initExprp
            = new AstConst{prevVarp->fileline(), AstConst::WidthedValue{}, prevVarp->width(), 0};
        AstNodeStmt* const initStmtp = new AstAssign{
            prevVarp->fileline(), new AstVarRef{prevVarp->fileline(), prevVarp, VAccess::WRITE},
            initExprp};
        m_constructorp->addStmtsp(initStmtp);

        coverpointp->user1p(prevVarp);
        return prevVarp;
    }

    // Create state position variable for multi-value transition bins
    // Tracks position in sequence: 0=not started, 1=seen first item, etc.
    AstVar* createSequenceStateVar(AstCoverpoint* coverpointp, AstCoverBin* binp) {
        // Create variable to track sequence position
        const string varName = "__Vseqpos_" + coverpointp->name() + "_" + binp->name();
        // Use 8-bit integer for state position (sequences rarely > 255 items)
        AstVar* stateVarp
            = new AstVar{binp->fileline(), VVarType::MEMBER, varName, VFlagLogicPacked{}, 8};
        stateVarp->isStatic(false);
        m_covergroupp->addMembersp(stateVarp);

        UINFO(4, "    Created sequence state variable: " << varName);

        // Initialize to 0 (not started) in constructor
        AstNodeStmt* const initStmtp = new AstAssign{
            stateVarp->fileline(), new AstVarRef{stateVarp->fileline(), stateVarp, VAccess::WRITE},
            new AstConst{stateVarp->fileline(), AstConst::WidthedValue{}, 8, 0}};
        m_constructorp->addStmtsp(initStmtp);

        return stateVarp;
    }

    void generateCoverpointCode(AstCoverpoint* coverpointp) {
        UINFO(4, "  Generating code for coverpoint: " << coverpointp->name());

        // Get the coverpoint expression
        AstNodeExpr* const exprp = coverpointp->exprp();

        // Expand automatic bins before processing
        expandAutomaticBins(coverpointp, exprp);

        // Extract all coverpoint options in a single pass
        int atLeastValue;
        int autoBinMax;
        extractCoverpointOptions(coverpointp, atLeastValue, autoBinMax);
        UINFO(6, "    Coverpoint at_least = " << atLeastValue << " auto_bin_max = " << autoBinMax);

        // Create implicit automatic bins if no regular bins exist
        createImplicitAutoBins(coverpointp, exprp, autoBinMax);

        // Eligible coverpoints route through the VlCoverpoint runtime; the rest (cross-fed or
        // transition-bearing) keep the legacy per-bin-member path below.
        if (coverpointConvertible(coverpointp)) {
            generateConvertedCoverpoint(coverpointp, exprp, atLeastValue);
            return;
        }

        // Generate member variables and matching code for each bin
        // Process in two passes: first non-default bins, then default bins
        std::vector<AstCoverBin*> defaultBins;
        bool hasTransition = false;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);

            // Defer default bins to second pass
            if (cbinp->binsType() == VCoverBinsType::BINS_DEFAULT) {
                defaultBins.push_back(cbinp);
                continue;
            }

            // Handle array bins: create separate bin for each value/transition
            if (cbinp->isArray()) {
                if (cbinp->transp()) {  // transition bin (includes illegal_bins with transitions)
                    hasTransition = true;
                    generateTransitionArrayBins(coverpointp, cbinp, exprp, atLeastValue);
                } else {
                    generateArrayBins(coverpointp, cbinp, exprp, atLeastValue);
                }
                continue;
            }

            // Create a member variable to track hits for this bin
            // Sanitize bin name to make it a valid C++ identifier
            const string binName = sanitizeGeneratedName(cbinp->name());
            AstVar* const varp = createTrackedCoverpointBinCounter(
                coverpointp, cbinp, binName, atLeastValue, "Created member variable",
                " type=" + string{cbinp->binsType().ascii()});

            // Note: Coverage database registration happens later via VL_COVER_INSERT
            // (see generateCoverageDeclarations() method around line 1164)
            // Classes use "v_covergroup/" hier prefix vs modules

            // Generate bin matching code in sample()
            // Handle transition bins specially (includes illegal_bins with transition syntax)
            if (cbinp->transp()) {
                hasTransition = true;
                generateTransitionBinMatchCode(coverpointp, cbinp, exprp, varp);
            } else {
                generateBinMatchCode(coverpointp, cbinp, exprp, varp);
            }
        }

        // Second pass: Handle default bins
        // Default bin matches when value doesn't match any other explicit bin
        for (AstCoverBin* defBinp : defaultBins) {
            // Create member variable for default bin
            const string binName = sanitizeGeneratedName(defBinp->name());
            AstVar* const varp = createTrackedCoverpointBinCounter(
                coverpointp, defBinp, binName, atLeastValue, "Created default bin variable");

            // Generate matching code: if (NOT (bin1 OR bin2 OR ... OR binN))
            generateDefaultBinMatchCode(coverpointp, defBinp, exprp, varp);
        }

        // After all bins processed, if coverpoint has transition bins, update previous value
        if (hasTransition) {
            AstVar* const prevVarp = VN_AS(coverpointp->user1p(), Var);
            // Generate: __Vprev_cpname = current_value;
            AstNodeStmt* updateStmtp
                = new AstAssign{coverpointp->fileline(),
                                new AstVarRef{prevVarp->fileline(), prevVarp, VAccess::WRITE},
                                exprp->cloneTree(false)};
            m_sampleFuncp->addStmtsp(updateStmtp);
            UINFO(4, "    Added previous value update at end of sample()");
        }
    }

    void generateBinMatchCode(AstCoverpoint* coverpointp, AstCoverBin* binp, AstNodeExpr* exprp,
                              AstVar* hitVarp) {
        UINFO(4, "    Generating bin match for: " << binp->name());

        // Build the bin matching condition using the shared function
        AstNodeExpr* fullCondp = buildBinCondition(binp, exprp);

        if (!fullCondp
            && (binp->binsType() == VCoverBinsType::BINS_IGNORE
                || binp->binsType() == VCoverBinsType::BINS_ILLEGAL)
            && !binp->rangesp()) {
            fullCondp = buildDefaultCondition(coverpointp, exprp, binp->fileline());
        }

        if (!fullCondp) {
            // Reachable for empty bins after value resolution; they do not contribute to coverage.
            return;
        }

        UINFO(4, "      Adding bin match if statement to sample function");
        addCoverpointBinHitIf(coverpointp, binp, hitVarp, fullCondp,
                              "Illegal bin " + binp->prettyNameQ() + " hit in coverpoint "
                                  + coverpointp->prettyNameQ(),
                              "sample() CFunc not set when generating bin match code");
        UINFO(4, "      Successfully added if statement for bin: " << binp->name());
    }

    // Build the condition under which a default bin matches: NOT(OR of all normal bins).
    AstNodeExpr* buildDefaultCondition(AstCoverpoint* coverpointp, AstNodeExpr* exprp,
                                       FileLine* fl) {
        if (exprp->width() <= 16) {
            std::vector<AstNodeExpr*> usedNodes;
            for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
                AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
                if (cbinp->binsType() == VCoverBinsType::BINS_DEFAULT || cbinp->transp()
                    || !cbinp->rangesp())
                    continue;
                std::vector<ArrayValue> values = extractArrayValues(cbinp, exprp);
                std::set<std::string> seen;
                for (ArrayValue& value : values) {
                    if (seen.insert(value.label).second) {
                        usedNodes.push_back(value.nodep);
                        value.nodep = nullptr;
                    } else {
                        VL_DO_DANGLING(value.nodep->deleteTree(), value.nodep);
                    }
                }
            }
            if (usedNodes.empty()) return new AstConst{fl, AstConst::BitTrue{}};
            return new AstNot{fl, buildValueGroupCondition(fl, exprp, usedNodes)};
        }

        AstNodeExpr* anyBinMatchp = nullptr;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            if (cbinp->binsType() == VCoverBinsType::BINS_DEFAULT
                || cbinp->binsType() == VCoverBinsType::BINS_IGNORE
                || cbinp->binsType() == VCoverBinsType::BINS_ILLEGAL)
                continue;
            AstNodeExpr* const binCondp = buildBinCondition(cbinp, exprp);
            UASSERT_OBJ(binCondp, cbinp,
                        "buildBinCondition returned nullptr for non-ignore/non-illegal bin");
            anyBinMatchp = anyBinMatchp ? new AstOr{fl, anyBinMatchp, binCondp} : binCondp;
        }
        return anyBinMatchp ? static_cast<AstNodeExpr*>(new AstNot{fl, anyBinMatchp})
                            : static_cast<AstNodeExpr*>(new AstConst{fl, AstConst::BitTrue{}});
    }

    //====================================================================
    // VlCoverpoint conversion (eligible coverpoints)

    // True if a coverpoint routes through the VlCoverpoint runtime.  Cross-fed coverpoints
    // (the cross reads their per-bin members) and transition-bearing ones stay legacy.
    bool coverpointConvertible(AstCoverpoint* coverpointp) {
        if (m_crossedCpNames.count(coverpointp->name())) return false;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            if (VN_AS(binp, CoverBin)->transp()) return false;
        }
        return true;
    }

    // A 'this->m_member' reference for embedding in an AstCStmt
    AstVarRef* memberRef(FileLine* fl, AstVar* varp) {
        AstVarRef* const refp = new AstVarRef{fl, varp, VAccess::READ};
        refp->selfPointer(VSelfPointerText{VSelfPointerText::This{}});
        return refp;
    }

    // Domain extremes of the coverpoint's effective type, used to resolve an open-ended
    // '$' array-bins bound.  Per IEEE 1800-2023 11.4.13, '$' is the type's lowest (lower
    // bound) or highest (upper bound) value; per 19.5.7(a) the effective type is the
    // coverpoint type when one is declared (V3LinkParse casts the expression to it), so the
    // domain follows the coverpoint expression's width and sign.
    static int64_t signedDomainMin(int width) {
        return (width >= 64) ? INT64_MIN : -(1LL << (width - 1));
    }
    static int64_t signedDomainMax(int width) {
        return (width >= 64) ? INT64_MAX : ((1LL << (width - 1)) - 1);
    }
    static uint64_t unsignedDomainMax(int width) {
        return (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
    }

    // One equality target of an array bin: its decimal label (used to build the
    // 'name[value]' bin name for open arrays) and the coverpoint-width compare constant.
    struct ArrayValue final {
        std::string label;
        AstNodeExpr* nodep;
    };

    static bool constIsNegative(AstConst* constp) {
        return constp->isSigned() && constp->num().isNegative();
    }

    static uint64_t nonNegativeConstUQuad(AstConst* constp) {
        return constp->isSigned() ? static_cast<uint64_t>(constp->num().toSQuad())
                                  : constp->num().toUQuad();
    }

    static bool constIsAboveSigned64(AstConst* constp) {
        return !constp->isSigned() && constp->num().toUQuad() > static_cast<uint64_t>(INT64_MAX);
    }

    static int64_t constSQuad(AstConst* constp) {
        return constp->isSigned() ? constp->num().toSQuad()
                                  : static_cast<int64_t>(constp->num().toUQuad());
    }

    static bool highBitsMatch(const V3Number& num, int width, bool wantOnes) {
        for (int bit = width; bit < num.width(); ++bit) {
            if (num.bitIs1(bit) != wantOnes) return false;
        }
        return true;
    }

    static bool constFitsEffectiveType(AstConst* constp, int width, bool isSigned) {
        const V3Number& num = constp->num();
        if (!isSigned && constIsNegative(constp)) return false;
        if (num.width() <= width) return true;
        if (isSigned && constp->isSigned()) {
            return highBitsMatch(num, width, num.bitIs1(width - 1));
        }
        return highBitsMatch(num, width, false);
    }

    static V3Number castConstToEffective(FileLine* fl, AstConst* constp, int width,
                                         bool isSigned) {
        V3Number num{fl, width, 0};
        if (constp->isSigned()) {
            num.opExtendS(constp->num(), constp->num().width());
        } else {
            num.opAssign(constp->num());
        }
        num.isSigned(isSigned);
        return num;
    }

    static bool nonNegativeNumberFitsU64(const V3Number& num) {
        return !num.isNegative() && num.mostSetBitP1() <= 64;
    }

    bool appendArrayNumber(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values, FileLine* fl,
                           const std::string& label, const V3Number& num) {
        if (values.size() >= ARRAY_BIN_VALUE_LIMIT) {
            tooManyArrayValues(arrayBinp, values);
            return false;
        }
        values.push_back(ArrayValue{label, new AstConst{fl, num}});
        return true;
    }

    bool appendWildcardArraySingleton(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values,
                                      AstConst* constp, AstNodeExpr* exprp) {
        if (exprp->width() > 64 || constp->width() > 64) {
            arrayBinp->v3error("Unsupported: covergroup wildcard array bin "
                               << arrayBinp->prettyNameQ() << " on value wider than 64 bits.");
            discardArrayValues(values);
            return false;
        }
        std::vector<int> wildcardBits;
        uint64_t base = 0;
        for (int bit = 0; bit < constp->width(); ++bit) {
            if (constp->num().bitIs1(bit)) {
                base |= (1ULL << bit);
            } else if (!constp->num().bitIs0(bit)) {
                wildcardBits.push_back(bit);
            }
        }
        if (wildcardBits.size() >= 64
            || (1ULL << wildcardBits.size()) > ARRAY_BIN_VALUE_LIMIT - values.size()) {
            tooManyArrayValues(arrayBinp, values);
            return false;
        }
        const uint64_t combos = 1ULL << wildcardBits.size();
        for (uint64_t combo = 0; combo < combos; ++combo) {
            uint64_t value = base;
            for (size_t idx = 0; idx < wildcardBits.size(); ++idx) {
                if (combo & (1ULL << idx)) value |= (1ULL << wildcardBits[idx]);
            }
            V3Number num{constp->fileline(), exprp->width(), 0};
            num.setQuad(value);
            num.isSigned(exprp->isSigned());
            const std::string label = exprp->isSigned() ? num.toDecimalS() : num.toDecimalU();
            if (!appendArrayNumber(arrayBinp, values, constp->fileline(), label, num))
                return false;
        }
        return true;
    }

    std::vector<ArrayValue> discardArrayValues(std::vector<ArrayValue>& values) {
        for (const ArrayValue& value : values)
            VL_DO_DANGLING(value.nodep->deleteTree(), value.nodep);
        values.clear();
        return {};
    }

    std::vector<ArrayValue> tooManyArrayValues(AstCoverBin* arrayBinp,
                                               std::vector<ArrayValue>& values) {
        arrayBinp->v3error("Unsupported: covergroup array bin "
                           << arrayBinp->prettyNameQ() << " expands to more than "
                           << ARRAY_BIN_VALUE_LIMIT << " values.");
        return discardArrayValues(values);
    }

    // IEEE 1800-2023 19.5.7(b) requires a warning when bin value(s) are dropped or clamped
    // because they fall outside the coverpoint type domain.
    void warnArrayBinExcluded(AstCoverBin* arrayBinp) {
        arrayBinp->v3warn(COVERIGN, "Coverage array bin "
                                        << arrayBinp->prettyNameQ()
                                        << " has value(s) outside the coverpoint domain; the "
                                           "out-of-domain value(s) are excluded "
                                           "(IEEE 1800-2023 19.5.7).");
    }

    bool appendArrayValue(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values, FileLine* fl,
                          int width, const std::string& label, uint64_t value) {
        V3Number num{fl, width, 0};
        num.setQuad(value);
        return appendArrayNumber(arrayBinp, values, fl, label, num);
    }

    // Append every value of the inclusive unsigned span [lo, hi] as a coverpoint-width compare
    // constant, skipping any whose decimal label is already in 'skip'.  The 'val == hi' guard
    // avoids wrap-around when hi is the type's maximum value.  Returns false (values discarded,
    // error already reported) only when the array-bin value limit is exceeded.
    bool appendUnsignedSpan(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values, FileLine* fl,
                            int width, uint64_t lo, uint64_t hi,
                            const std::set<std::string>& skip = {}) {
        for (uint64_t val = lo; val <= hi; ++val) {
            const std::string label = std::to_string(val);
            if (!skip.count(label) && !appendArrayValue(arrayBinp, values, fl, width, label, val))
                return false;
            if (val == hi) break;
        }
        return true;
    }

    // Signed counterpart of appendUnsignedSpan: the compare constant is the two's-complement
    // value, but the bin label is the signed decimal (so '-1' is named '-1', not its unsigned
    // image).
    bool appendSignedSpan(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values, FileLine* fl,
                          int width, int64_t lo, int64_t hi,
                          const std::set<std::string>& skip = {}) {
        for (int64_t val = lo; val <= hi; ++val) {
            const std::string label = std::to_string(val);
            if (!skip.count(label)
                && !appendArrayValue(arrayBinp, values, fl, width, label,
                                     static_cast<uint64_t>(val)))
                return false;
            if (val == hi) break;
        }
        return true;
    }

    // A single 'bins b[...] = {..., value, ...}' entry.  Discrete values are kept at any
    // width; only the range expansion path is restricted to <=64 bits.
    bool appendArraySingleton(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values,
                              AstConst* constp, AstNodeExpr* exprp) {
        const int width = exprp->width();
        const bool isSigned = exprp->isSigned();
        if (constp->num().isFourState()) {
            if (arrayBinp->isWildcard())
                return appendWildcardArraySingleton(arrayBinp, values, constp, exprp);
            warnArrayBinExcluded(arrayBinp);
            return true;
        }
        if (!constFitsEffectiveType(constp, width, isSigned)) {
            warnArrayBinExcluded(arrayBinp);
            return true;
        }
        const V3Number num = castConstToEffective(constp->fileline(), constp, width, isSigned);
        const std::string label = isSigned ? num.toDecimalS() : num.toDecimalU();
        return appendArrayNumber(arrayBinp, values, constp->fileline(), label, num);
    }

    // A single 'bins b[...] = {..., [lo:hi], ...}' entry, expanded to its in-domain values.
    bool appendArrayRange(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values,
                          AstInsideRange* irp, AstNodeExpr* exprp) {
        const int width = exprp->width();
        AstNodeExpr* const lhsp = V3Const::constifyEdit(irp->lhsp());
        AstNodeExpr* const rhsp = V3Const::constifyEdit(irp->rhsp());
        const bool loUnbounded = VN_IS(lhsp, Unbounded);
        const bool hiUnbounded = VN_IS(rhsp, Unbounded);
        AstConst* const minp = VN_CAST(lhsp, Const);
        AstConst* const maxp = VN_CAST(rhsp, Const);
        if ((!minp && !loUnbounded) || (!maxp && !hiUnbounded)) {
            arrayBinp->v3error("Non-constant expression in array bins range; "
                               "range bounds must be constants.");
            discardArrayValues(values);
            return false;
        }
        if ((minp && minp->num().isFourState()) || (maxp && maxp->num().isFourState())) {
            warnArrayBinExcluded(arrayBinp);
            return true;
        }

        FileLine* const fl = irp->fileline();
        if (width > 64) {
            if (loUnbounded || hiUnbounded) {
                arrayBinp->v3error("Unsupported: covergroup array bin "
                                   << arrayBinp->prettyNameQ()
                                   << " '$' range on coverpoint wider than 64 bits.");
                discardArrayValues(values);
                return false;
            }
            if (!constFitsEffectiveType(minp, width, exprp->isSigned())
                || !constFitsEffectiveType(maxp, width, exprp->isSigned())) {
                warnArrayBinExcluded(arrayBinp);
                return true;
            }
            const V3Number loNum = castConstToEffective(fl, minp, width, exprp->isSigned());
            const V3Number hiNum = castConstToEffective(fl, maxp, width, exprp->isSigned());
            if (!nonNegativeNumberFitsU64(loNum) || !nonNegativeNumberFitsU64(hiNum)) {
                arrayBinp->v3error("Unsupported: covergroup array bin "
                                   << arrayBinp->prettyNameQ()
                                   << " range bounds outside 64-bit enumeration.");
                discardArrayValues(values);
                return false;
            }
            return appendUnsignedSpan(arrayBinp, values, fl, width, loNum.toUQuad(),
                                      hiNum.toUQuad());
        }
        if (exprp->isSigned()) {
            const int64_t domainLo = signedDomainMin(width);
            const int64_t domainHi = signedDomainMax(width);
            if (minp && constIsAboveSigned64(minp)) {  // Lower bound above representable range
                warnArrayBinExcluded(arrayBinp);
                return true;
            }
            bool clamped = false;
            const int64_t rawLo = loUnbounded ? domainLo : constSQuad(minp);
            int64_t rawHi;
            if (hiUnbounded) {
                rawHi = domainHi;
            } else if (constIsAboveSigned64(maxp)) {
                rawHi = domainHi;
                clamped = true;
            } else {
                rawHi = constSQuad(maxp);
            }
            if (rawLo > rawHi) return true;  // User-empty range (IEEE 11.4.13): no warning
            if (rawHi < domainLo || rawLo > domainHi) {
                warnArrayBinExcluded(arrayBinp);
                return true;
            }
            const int64_t lo = std::max(rawLo, domainLo);
            const int64_t hi = std::min(rawHi, domainHi);
            if (clamped || lo != rawLo || hi != rawHi) warnArrayBinExcluded(arrayBinp);
            if (!appendSignedSpan(arrayBinp, values, fl, width, lo, hi)) return false;
        } else {
            const uint64_t domainHi = unsignedDomainMax(width);
            const bool loNeg = (minp && constIsNegative(minp));
            if (maxp && constIsNegative(maxp)) {  // Whole range below the unsigned domain
                warnArrayBinExcluded(arrayBinp);
                return true;
            }
            const uint64_t rawLo = (loUnbounded || loNeg) ? 0 : nonNegativeConstUQuad(minp);
            const uint64_t rawHi = hiUnbounded ? domainHi : nonNegativeConstUQuad(maxp);
            if (rawLo > rawHi) return true;  // User-empty range (IEEE 11.4.13): no warning
            if (rawLo > domainHi) {
                warnArrayBinExcluded(arrayBinp);
                return true;
            }
            const uint64_t hi = std::min(rawHi, domainHi);
            if (loNeg || hi != rawHi) warnArrayBinExcluded(arrayBinp);
            if (!appendUnsignedSpan(arrayBinp, values, fl, width, rawLo, hi)) return false;
        }
        return true;
    }

    // Individual equality targets of an array bin (bins b[N] = {values/ranges}), in order.
    // An open-ended '$' range bound resolves to the coverpoint domain min/max.
    std::vector<ArrayValue> extractArrayValues(AstCoverBin* arrayBinp, AstNodeExpr* exprp) {
        std::vector<ArrayValue> values;
        for (AstNode* rangep = arrayBinp->rangesp(); rangep; rangep = rangep->nextp()) {
            AstInsideRange* const irp = VN_CAST(rangep, InsideRange);
            if (!irp) {
                AstConst* const constp = VN_CAST(rangep, Const);
                if (!constp) {
                    arrayBinp->v3error("Non-constant expression in array bins value list; "
                                       "values must be constants.");
                    return discardArrayValues(values);
                }
                if (!appendArraySingleton(arrayBinp, values, constp, exprp)) return {};
                continue;
            }
            if (!appendArrayRange(arrayBinp, values, irp, exprp)) return {};
        }
        return values;
    }

    // Array size N for 'bins b[N]'; -1 for an open 'bins b[]'; 0 on error (already reported).
    int fixedArrayBinCount(AstCoverBin* arrayBinp, std::vector<ArrayValue>& values) {
        AstNodeExpr* const sizep = arrayBinp->arraySizep();
        if (!sizep) return -1;
        AstConst* const constp = VN_CAST(V3Const::constifyEdit(sizep), Const);
        if (!constp) {
            arrayBinp->v3error("Covergroup array bins size must be a constant.");
            discardArrayValues(values);
            return 0;
        }
        if (constp->num().isFourState() || constIsNegative(constp)) {
            arrayBinp->v3error(
                "Covergroup array bins size must be a positive two-state constant.");
            discardArrayValues(values);
            return 0;
        }
        if (constp->num().mostSetBitP1() > 63 || constp->num().toUQuad() > ARRAY_BIN_VALUE_LIMIT) {
            arrayBinp->v3error("Unsupported: covergroup array bin "
                               << arrayBinp->prettyNameQ() << " has more than "
                               << ARRAY_BIN_VALUE_LIMIT << " bins.");
            discardArrayValues(values);
            return 0;
        }
        const int count = static_cast<int>(constp->num().toUQuad());
        if (count <= 0) {
            arrayBinp->v3error("Covergroup array bins size must be >= 1, got " << count << ".");
            discardArrayValues(values);
            return 0;
        }
        return count;
    }

    // Open array 'bins b[]': one bin per distinct value (IEEE 1800-2023 19.5.1 merges values
    // specified more than once), preserving first-occurrence order.  Duplicate nodes freed.
    std::vector<ArrayValue> dedupArrayValues(std::vector<ArrayValue>& values) {
        std::vector<ArrayValue> result;
        std::set<std::string> seen;
        result.reserve(values.size());
        for (ArrayValue& value : values) {
            if (seen.insert(value.label).second) {
                result.push_back(value);
                value.nodep = nullptr;
            } else {
                VL_DO_DANGLING(value.nodep->deleteTree(), value.nodep);
            }
        }
        values.clear();
        return result;
    }

    // Add the decimal labels of cbinp's resolved values to 'out', freeing the value nodes.
    // Collects the labels already covered by a sibling bin (used for ignore/illegal exclusion
    // and for subtracting explicit bins from a default bin's domain).
    void appendBinValueLabels(std::set<std::string>& out, AstCoverBin* cbinp, AstNodeExpr* exprp) {
        std::vector<ArrayValue> values = extractArrayValues(cbinp, exprp);
        for (const ArrayValue& value : values) {
            out.insert(value.label);
            VL_DO_DANGLING(value.nodep->deleteTree(), value.nodep);
        }
    }

    std::set<std::string> collectExcludedValueLabels(AstCoverpoint* coverpointp,
                                                     AstNodeExpr* exprp) {
        std::set<std::string> excluded;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            if (cbinp->transp()) continue;
            if (cbinp->binsType() != VCoverBinsType::BINS_IGNORE
                && cbinp->binsType() != VCoverBinsType::BINS_ILLEGAL)
                continue;
            if (!cbinp->rangesp()) continue;
            appendBinValueLabels(excluded, cbinp, exprp);
        }
        return excluded;
    }

    std::vector<AstNodeExpr*> takeFilteredNodes(std::vector<ArrayValue>& values,
                                                const std::set<std::string>& excluded) {
        std::vector<AstNodeExpr*> nodes;
        for (ArrayValue& value : values) {
            if (excluded.count(value.label)) {
                VL_DO_DANGLING(value.nodep->deleteTree(), value.nodep);
            } else {
                nodes.push_back(value.nodep);
                value.nodep = nullptr;
            }
        }
        values.clear();
        return nodes;
    }

    bool shouldEnumerateForScalarBin(AstCoverBin* binp, AstNodeExpr* exprp) {
        if (exprp->width() <= 16) return true;
        for (AstNode* rangep = binp->rangesp(); rangep; rangep = rangep->nextp()) {
            if (!VN_IS(rangep, Const)) return false;
        }
        return true;
    }

    std::vector<ArrayValue> defaultArrayValues(AstCoverpoint* coverpointp, AstCoverBin* defBinp,
                                               AstNodeExpr* exprp) {
        std::set<std::string> used;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            if (cbinp == defBinp || cbinp->binsType() == VCoverBinsType::BINS_DEFAULT
                || cbinp->transp() || !cbinp->rangesp())
                continue;
            appendBinValueLabels(used, cbinp, exprp);
        }

        std::vector<ArrayValue> result;
        const int width = exprp->width();
        if (width > 16) {
            defBinp->v3error("Unsupported: covergroup default array bin "
                             << defBinp->prettyNameQ() << " on coverpoint wider than 16 bits.");
            return result;
        }
        if (exprp->isSigned()) {
            if (!appendSignedSpan(defBinp, result, defBinp->fileline(), width,
                                  signedDomainMin(width), signedDomainMax(width), used))
                return {};
        } else {
            if (!appendUnsignedSpan(defBinp, result, defBinp->fileline(), width, 0,
                                    unsignedDomainMax(width), used))
                return {};
        }
        return result;
    }

    // Fixed array 'bins b[N]': distribute values into exactly N groups (IEEE 1800-2023 19.5.1).
    // B = floor(values/N) (>=1); the last non-empty bin takes any remainder; surplus bins stay
    // empty (and excluded from coverage).  Consumes the value nodes.
    std::vector<std::vector<AstNodeExpr*>>
    distributeFixedArrayValues(std::vector<ArrayValue>& values, int fixedCount) {
        std::vector<std::vector<AstNodeExpr*>> groups(static_cast<size_t>(fixedCount));
        const size_t valueCount = values.size();
        if (valueCount != 0) {
            const size_t perBin
                = std::max<size_t>(valueCount / static_cast<size_t>(fixedCount), 1);
            size_t cursor = 0;
            for (int bin = 0; bin < fixedCount && cursor < valueCount; ++bin) {
                const size_t count = (bin == fixedCount - 1)
                                         ? valueCount - cursor
                                         : std::min(perBin, valueCount - cursor);
                groups[bin].reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    groups[bin].push_back(values[cursor].nodep);
                    values[cursor].nodep = nullptr;
                    ++cursor;
                }
            }
        }
        values.clear();
        return groups;
    }

    AstNodeExpr* buildValueGroupCondition(FileLine* fl, AstNodeExpr* exprp,
                                          const std::vector<AstNodeExpr*>& values) {
        AstNodeExpr* condp = nullptr;
        for (AstNodeExpr* const valuep : values) {
            AstNodeExpr* const onep = new AstEq{fl, exprp->cloneTree(false), valuep};
            condp = condp ? static_cast<AstNodeExpr*>(new AstOr{fl, condp, onep}) : onep;
        }
        return condp;
    }

    // The '"file", line, col);' argument tail shared by the VlCoverpoint namer calls.
    static std::string namerLocArgs(const FileLine* fl) {
        return "\"" + std::string{fl->filename()} + "\", " + std::to_string(fl->lineno()) + ", "
               + std::to_string(fl->firstColumn()) + ");";
    }

    // Emit a 'this->m_cp.addSingleNamer(...)' statement with an explicit bin name.
    AstCStmt* makeSingleNamer(AstVar* cpVarp, AstCoverBin* binp, const std::string& fullName) {
        FileLine* const fl = binp->fileline();
        AstCStmt* const cs = new AstCStmt{fl};
        cs->add(memberRef(fl, cpVarp));
        cs->add(".addSingleNamer(" + std::string{binp->binsType().binSetEnum()} + ", \"" + fullName
                + "\", " + namerLocArgs(fl));
        return cs;
    }

    // Emit a 'this->m_cp.addSingleNamer/addArrayNamer(...)' statement for one bin
    AstCStmt* makeNamer(AstVar* cpVarp, AstCoverBin* binp, int count) {
        if (count < 0) return makeSingleNamer(cpVarp, binp, binp->name());  // single bin
        FileLine* const fl = binp->fileline();
        AstCStmt* const cs = new AstCStmt{fl};
        cs->add(memberRef(fl, cpVarp));
        cs->add(".addArrayNamer(" + std::string{binp->binsType().binSetEnum()} + ", "  // array
                + std::to_string(count) + ", \"" + binp->name() + "\", " + namerLocArgs(fl));
        return cs;
    }

    // Emit 'if (iff && cond) m_cp.incrementBin(idx);' (or recordHit, + illegal action) in sample()
    void emitConvHitIf(AstCoverpoint* coverpointp, AstCoverBin* binp, AstVar* cpVarp, int idx,
                       AstNodeExpr* condp) {
        FileLine* const fl = binp->fileline();
        AstCStmt* const hitp = new AstCStmt{fl};
        hitp->add(memberRef(fl, cpVarp));
        hitp->add((binp->binsType().binIsNormal() ? ".incrementBin(" : ".recordHit(")
                  + std::to_string(idx) + ");");
        AstNode* actionp = hitp;
        if (binp->binsType() == VCoverBinsType::BINS_ILLEGAL) {
            actionp->addNext(makeIllegalBinAction(fl, "Illegal bin " + binp->prettyNameQ()
                                                          + " hit in coverpoint "
                                                          + coverpointp->prettyNameQ()));
        }
        AstNodeExpr* const guardedp = applyIffGuards(coverpointp, binp, fl, condp);
        UASSERT_OBJ(m_sampleFuncp, binp, "sample() CFunc not set in converted coverpoint");
        m_sampleFuncp->addStmtsp(new AstIf{fl, guardedp, actionp, nullptr});
    }

    // Emit one value-array bin into a converted coverpoint from its already-resolved values:
    // append namer statement(s), emit the sample() increments, and advance idx past the
    // reserved bin slots.  Open 'bins b[]' yields one bin per distinct value named b[value];
    // fixed 'bins b[N]' yields N positional bins b[0]..b[N-1] (surplus empty bins are excluded
    // from coverage).  Consumes the value nodes.
    void emitConvArrayBinGroups(AstCoverpoint* coverpointp, AstCoverBin* cbinp, AstVar* cpVarp,
                                AstNodeExpr* exprp, int& idx, std::vector<AstCStmt*>& namerStmts,
                                std::vector<ArrayValue>& values) {
        const int fixedCount = fixedArrayBinCount(cbinp, values);
        if (fixedCount == 0) return;  // Error already reported; values discarded
        if (fixedCount < 0) {  // Open 'bins b[]': one bin per distinct value, named b[value]
            const std::vector<ArrayValue> distinct = dedupArrayValues(values);
            for (const ArrayValue& value : distinct) {
                namerStmts.push_back(
                    makeSingleNamer(cpVarp, cbinp, cbinp->name() + "[" + value.label + "]"));
                emitConvHitIf(coverpointp, cbinp, cpVarp, idx++,
                              new AstEq{cbinp->fileline(), exprp->cloneTree(false), value.nodep});
            }
            return;
        }
        const std::vector<std::vector<AstNodeExpr*>> groups
            = distributeFixedArrayValues(values, fixedCount);
        for (int bin = 0; bin < fixedCount; ++bin) {
            if (groups[bin].empty()) continue;
            namerStmts.push_back(
                makeSingleNamer(cpVarp, cbinp, cbinp->name() + "[" + std::to_string(bin) + "]"));
            emitConvHitIf(coverpointp, cbinp, cpVarp, idx++,
                          buildValueGroupCondition(cbinp->fileline(), exprp, groups[bin]));
        }
    }

    // Route an eligible coverpoint through a VlCoverpoint member: emit the member, its
    // sample() increments, the constructor configuration (init + namers), and registration.
    void generateConvertedCoverpoint(AstCoverpoint* coverpointp, AstNodeExpr* exprp,
                                     int atLeastValue) {
        FileLine* const fl = coverpointp->fileline();
        UINFO(4, "  Converting coverpoint to VlCoverpoint: " << coverpointp->name());

        if (!m_vlCoverpointDTypep) {
            m_vlCoverpointDTypep = new AstCDType{fl, "VlCoverpoint"};
            v3Global.rootp()->typeTablep()->addTypesp(m_vlCoverpointDTypep);
        }
        AstVar* const cpVarp = new AstVar{fl, VVarType::MEMBER, "__Vcp_" + coverpointp->name(),
                                          m_vlCoverpointDTypep};
        cpVarp->isStatic(false);
        m_covergroupp->addMembersp(cpVarp);
        m_convCpVars.push_back(cpVarp);

        // Walk bins (non-default, then default), assigning sequential indices that match the
        // namer append order; emit sample increments and collect namer statements.
        std::vector<AstCStmt*> namerStmts;
        std::vector<AstCoverBin*> defaultBins;
        const std::set<std::string> excludedLabels
            = collectExcludedValueLabels(coverpointp, exprp);
        int idx = 0;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            if (cbinp->binsType() == VCoverBinsType::BINS_DEFAULT) {
                defaultBins.push_back(cbinp);
                continue;
            }
            if (cbinp->isArray()) {  // value array: bins b[N] / b[] -> one bin per value
                std::vector<ArrayValue> values = extractArrayValues(cbinp, exprp);
                emitConvArrayBinGroups(coverpointp, cbinp, cpVarp, exprp, idx, namerStmts, values);
            } else {
                AstNodeExpr* condp = nullptr;
                bool enumerated = false;
                if (!cbinp->rangesp()
                    && (cbinp->binsType() == VCoverBinsType::BINS_IGNORE
                        || cbinp->binsType() == VCoverBinsType::BINS_ILLEGAL)) {
                    condp = buildDefaultCondition(coverpointp, exprp, cbinp->fileline());
                } else if (!cbinp->transp() && shouldEnumerateForScalarBin(cbinp, exprp)) {
                    enumerated = true;
                    std::vector<ArrayValue> values = extractArrayValues(cbinp, exprp);
                    const std::vector<AstNodeExpr*> nodes
                        = cbinp->binsType().binIsNormal()
                              ? takeFilteredNodes(values, excludedLabels)
                              : takeFilteredNodes(values, {});
                    if (!nodes.empty())
                        condp = buildValueGroupCondition(cbinp->fileline(), exprp, nodes);
                }
                if (!condp) {
                    if (cbinp->binsType().binIsNormal() && enumerated) continue;
                    condp = buildBinCondition(cbinp, exprp);
                    if (!condp) continue;
                }
                namerStmts.push_back(makeNamer(cpVarp, cbinp, -1));
                emitConvHitIf(coverpointp, cbinp, cpVarp, idx, condp);
                ++idx;
            }
        }
        for (AstCoverBin* const defBinp : defaultBins) {
            if (defBinp->isArray()) {
                std::vector<ArrayValue> values = defaultArrayValues(coverpointp, defBinp, exprp);
                emitConvArrayBinGroups(coverpointp, defBinp, cpVarp, exprp, idx, namerStmts,
                                       values);
            } else {
                namerStmts.push_back(makeNamer(cpVarp, defBinp, -1));
                emitConvHitIf(coverpointp, defBinp, cpVarp, idx++,
                              buildDefaultCondition(coverpointp, exprp, defBinp->fileline()));
            }
        }

        // Constructor: init (allocates), namers, then registration (under --coverage)
        const std::string hier = m_covergroupp->name() + "." + coverpointp->name();
        AstCStmt* const initp = new AstCStmt{fl};
        initp->add(memberRef(fl, cpVarp));
        initp->add(".init(\"" + hier + "\", " + std::to_string(atLeastValue) + ", "
                   + std::to_string(idx) + ");");
        m_constructorp->addStmtsp(initp);
        for (AstCStmt* const ns : namerStmts) m_constructorp->addStmtsp(ns);
        if (v3Global.opt.coverage()) {
            AstCStmt* const regp = new AstCStmt{fl};
            regp->add(memberRef(fl, cpVarp));
            regp->add(".registerBins(vlSymsp->_vm_contextp__->coveragep(), \"v_covergroup/"
                      + m_covergroupp->name() + "\");");
            m_constructorp->addStmtsp(regp);
        }
    }

    // Generate matching code for default bins
    // Default bins match when value doesn't match any other explicit bin
    void generateDefaultBinMatchCode(AstCoverpoint* coverpointp, AstCoverBin* defBinp,
                                     AstNodeExpr* exprp, AstVar* hitVarp) {
        UINFO(4, "    Generating default bin match for: " << defBinp->name());

        AstNodeExpr* const defaultCondp
            = applyIffGuards(coverpointp, defBinp, defBinp->fileline(),
                             buildDefaultCondition(coverpointp, exprp, defBinp->fileline()));

        // Create increment statement
        AstNode* const stmtp = makeBinHitIncrement(defBinp->fileline(), hitVarp);

        // Create if statement
        AstIf* const ifp = new AstIf{defBinp->fileline(), defaultCondp, stmtp, nullptr};

        UASSERT_OBJ(m_sampleFuncp, defBinp,
                    "sample() CFunc not set when generating default bin code");
        m_sampleFuncp->addStmtsp(ifp);
        UINFO(4, "      Successfully added default bin if statement");
    }

    // Generate matching code for transition bins
    // Transition bins match sequences like: (val1 => val2 => val3)
    void generateTransitionBinMatchCode(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                        AstNodeExpr* exprp, AstVar* hitVarp) {
        UINFO(4, "    Generating transition bin match for: " << binp->name());

        // Get the (single) transition set
        AstCoverTransSet* const transSetp = binp->transp();

        // Use the helper function to generate code for this transition
        generateSingleTransitionCode(coverpointp, binp, exprp, hitVarp, transSetp);
    }

    // Generate state machine code for multi-value transition sequences
    // Handles transitions like (1 => 2 => 3 => 4)
    void generateMultiValueTransitionCode(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                          AstNodeExpr* exprp, AstVar* hitVarp,
                                          const std::vector<AstCoverTransItem*>& items) {
        UINFO(4, "    Generating multi-value transition state machine for: " << binp->name());
        UINFO(4, "      Sequence length: " << items.size() << " items");

        // Create state position variable
        AstVar* const stateVarp = createSequenceStateVar(coverpointp, binp);

        // Build case statement with N cases (one for each state 0 to N-1)
        // State 0: Not started, looking for first item
        // State 1 to N-1: In progress, looking for next item

        AstCase* const casep
            = new AstCase{binp->fileline(), VCaseType::CT_CASE,
                          new AstVarRef{stateVarp->fileline(), stateVarp, VAccess::READ}, nullptr};

        // Generate each case item in the switch statement
        for (size_t state = 0; state < items.size(); ++state) {
            AstCaseItem* caseItemp = generateTransitionStateCase(coverpointp, binp, exprp, hitVarp,
                                                                 stateVarp, items, state);
            casep->addItemsp(caseItemp);
        }

        // Add default case (reset to state 0) to prevent CASEINCOMPLETE warnings,
        // since the state variable is wider than the number of valid states.
        AstCaseItem* const defaultItemp = new AstCaseItem{
            binp->fileline(), nullptr,
            new AstAssign{binp->fileline(),
                          new AstVarRef{binp->fileline(), stateVarp, VAccess::WRITE},
                          new AstConst{binp->fileline(), AstConst::WidthedValue{}, 8, 0}}};
        casep->addItemsp(defaultItemp);

        m_sampleFuncp->addStmtsp(casep);
        UINFO(4, "      Successfully added multi-value transition state machine");
    }

    // Generate code for a single state in the transition state machine
    // Returns the case item for this state
    AstCaseItem* generateTransitionStateCase(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                             AstNodeExpr* exprp, AstVar* hitVarp,
                                             AstVar* stateVarp,
                                             const std::vector<AstCoverTransItem*>& items,
                                             size_t state) {
        FileLine* const fl = binp->fileline();

        // Build condition for current value matching expected item at this state
        AstNodeExpr* matchCondp = buildTransitionItemCondition(items[state], exprp);

        matchCondp = applyIffGuards(coverpointp, binp, fl, matchCondp);

        AstNodeStmt* matchActionp = nullptr;

        if (state == items.size() - 1) {
            // Last state: sequence complete!
            // Increment bin counter
            matchActionp = makeBinHitIncrement(fl, hitVarp);

            // For illegal_bins, add error message
            if (binp->binsType() == VCoverBinsType::BINS_ILLEGAL) {
                const string errMsg = "Illegal transition bin " + binp->prettyNameQ()
                                      + " hit in coverpoint " + coverpointp->prettyNameQ();
                matchActionp = matchActionp->addNext(makeIllegalBinAction(fl, errMsg));
            }

            // Reset state to 0
            matchActionp = matchActionp->addNext(
                new AstAssign{fl, new AstVarRef{fl, stateVarp, VAccess::WRITE},
                              new AstConst{fl, AstConst::WidthedValue{}, 8, 0}});
        } else {
            // Intermediate state: advance to next state
            matchActionp = new AstAssign{
                fl, new AstVarRef{fl, stateVarp, VAccess::WRITE},
                new AstConst{fl, AstConst::WidthedValue{}, 8, static_cast<uint32_t>(state + 1)}};
        }

        // Build restart logic: check if current value matches first item
        // If so, restart sequence from state 1 (even if we're in middle of sequence)
        AstNodeStmt* noMatchActionp = nullptr;
        if (state > 0) {
            // Check if current value matches first item (restart condition)
            AstNodeExpr* restartCondp = buildTransitionItemCondition(items[0], exprp);

            UASSERT_OBJ(restartCondp, items[0],
                        "buildTransitionItemCondition returned nullptr for restart");
            restartCondp = applyIffGuards(coverpointp, binp, fl, restartCondp);

            // Restart to state 1
            AstNodeStmt* restartActionp
                = new AstAssign{fl, new AstVarRef{fl, stateVarp, VAccess::WRITE},
                                new AstConst{fl, AstConst::WidthedValue{}, 8, 1}};

            // Reset to state 0 (else branch)
            AstNodeStmt* resetActionp
                = new AstAssign{fl, new AstVarRef{fl, stateVarp, VAccess::WRITE},
                                new AstConst{fl, AstConst::WidthedValue{}, 8, 0}};

            noMatchActionp = new AstIf{fl, restartCondp, restartActionp, resetActionp};
        }
        // For state 0, no action needed if no match (stay in state 0)

        // Combine into if-else
        AstNodeStmt* const stmtp = new AstIf{fl, matchCondp, matchActionp, noMatchActionp};

        // Create case item for this state value
        AstCaseItem* const caseItemp = new AstCaseItem{
            fl, new AstConst{fl, AstConst::WidthedValue{}, 8, static_cast<uint32_t>(state)},
            stmtp};

        return caseItemp;
    }

    // Create: $error(msg); $stop;  Used when an illegal bin is hit.
    AstNodeStmt* makeIllegalBinAction(FileLine* fl, const string& errMsg) {
        AstDisplay* const errorp
            = new AstDisplay{fl, VDisplayType::DT_ERROR, errMsg, nullptr, nullptr};
        errorp->fmtp()->timeunit(m_covergroupp->timeunit());
        static_cast<AstNode*>(errorp)->addNext(new AstStop{fl, true});
        return errorp;
    }

    // Create: hitVarp = hitVarp + 1
    AstAssign* makeBinHitIncrement(FileLine* fl, AstVar* hitVarp) {
        return new AstAssign{fl, new AstVarRef{fl, hitVarp, VAccess::WRITE},
                             new AstAdd{fl, new AstVarRef{fl, hitVarp, VAccess::READ},
                                        new AstConst{fl, AstConst::WidthedValue{}, 32, 1}}};
    }

    // Clone a constant node, widening to targetWidth if needed (zero-extend).
    // Used to ensure comparisons use matching widths after V3Width has run.
    static AstConst* widenConst(FileLine* fl, AstConst* constp, int targetWidth) {
        if (constp->width() == targetWidth) return constp->cloneTree(false);
        V3Number num{fl, targetWidth, 0};
        num.opAssign(constp->num());
        return new AstConst{fl, num};
    }

    // Build a range condition: minp <= exprp <= maxp.
    // Uses signed comparisons if exprp is signed; omits trivially-true bounds for unsigned.
    // All arguments are non-owning; clones exprp/minp/maxp as needed.
    AstNodeExpr* makeRangeCondition(FileLine* fl, AstNodeExpr* exprp, AstNodeExpr* minp,
                                    AstNodeExpr* maxp) {
        const int exprWidth = exprp->widthMin();
        AstConst* const minConstp = VN_AS(minp, Const);
        AstConst* const maxConstp = VN_AS(maxp, Const);
        // Widen constants to match expression width so post-V3Width nodes use correct macros
        AstConst* const minWidep = widenConst(fl, minConstp, exprWidth);
        AstConst* const maxWidep = widenConst(fl, maxConstp, exprWidth);
        if (exprp->isSigned()) {
            return new AstAnd{fl, new AstGteS{fl, exprp->cloneTree(false), minWidep},
                              new AstLteS{fl, exprp->cloneTree(false), maxWidep}};
        }
        // Unsigned: skip bounds that are trivially satisfied for the expression width
        const bool skipLowerCheck = (minConstp->toUQuad() == 0);
        bool skipUpperCheck = false;
        if (exprWidth <= 64) {
            const uint64_t maxVal
                = (exprWidth == 64) ? ~static_cast<uint64_t>(0) : ((1ULL << exprWidth) - 1ULL);
            skipUpperCheck = (maxConstp->toUQuad() == maxVal);
        }
        if (skipLowerCheck && skipUpperCheck) {
            VL_DO_DANGLING(pushDeletep(minWidep), minWidep);
            VL_DO_DANGLING(pushDeletep(maxWidep), maxWidep);
            return new AstConst{fl, AstConst::BitTrue{}};
        } else if (skipLowerCheck) {
            VL_DO_DANGLING(pushDeletep(minWidep), minWidep);
            return new AstLte{fl, exprp->cloneTree(false), maxWidep};
        } else if (skipUpperCheck) {
            VL_DO_DANGLING(pushDeletep(maxWidep), maxWidep);
            return new AstGte{fl, exprp->cloneTree(false), minWidep};
        } else {
            return new AstAnd{fl, new AstGte{fl, exprp->cloneTree(false), minWidep},
                              new AstLte{fl, exprp->cloneTree(false), maxWidep}};
        }
    }

    // Build a one-sided comparison for an open-ended bin range whose other bound is '$'.
    // '$' denotes the coverpoint domain extreme, so {[lo:$]} == (expr >= lo) and
    // {[$:hi]} == (expr <= hi).
    AstNodeExpr* makeOpenRangeCondition(FileLine* fl, AstNodeExpr* exprp, AstConst* boundp,
                                        bool isLowerBound) {
        AstConst* const widep = widenConst(fl, boundp, exprp->widthMin());
        if (isLowerBound) {
            if (exprp->isSigned()) return new AstGteS{fl, exprp->cloneTree(false), widep};
            return new AstGte{fl, exprp->cloneTree(false), widep};
        }
        if (exprp->isSigned()) return new AstLteS{fl, exprp->cloneTree(false), widep};
        return new AstLte{fl, exprp->cloneTree(false), widep};
    }

    // Build condition for a single transition item.
    // Returns expression that checks if exprp matches the item's value/range list.
    // Overload for when the expression is a variable read -- creates and manages the VarRef
    // internally, so callers don't need to construct a temporary node.
    AstNodeExpr* buildTransitionItemCondition(AstCoverTransItem* itemp, AstVar* varp) {
        AstNodeExpr* varRefp = new AstVarRef{varp->fileline(), varp, VAccess::READ};
        AstNodeExpr* const condp = buildTransitionItemCondition(itemp, varRefp);
        VL_DO_DANGLING(pushDeletep(varRefp), varRefp);
        return condp;
    }

    // Non-owning: exprp is cloned internally; caller retains ownership of exprp.
    AstNodeExpr* buildTransitionItemCondition(AstCoverTransItem* itemp, AstNodeExpr* exprp) {
        AstNodeExpr* condp = nullptr;

        for (AstNode* valp = itemp->valuesp(); valp; valp = valp->nextp()) {
            AstNodeExpr* singleCondp = nullptr;

            AstConst* const constp = VN_AS(valp, Const);
            singleCondp
                = new AstEq{constp->fileline(), exprp->cloneTree(false), constp->cloneTree(false)};

            if (condp) {
                condp = new AstOr{itemp->fileline(), condp, singleCondp};
            } else {
                condp = singleCondp;
            }
        }

        return condp;
    }

    // Generate multiple bins for array bins
    // Array bins create one bin per value in the range list
    void generateArrayBins(AstCoverpoint* coverpointp, AstCoverBin* arrayBinp, AstNodeExpr* exprp,
                           int atLeastValue) {
        UINFO(4, "    Generating array bins for: " << arrayBinp->name());

        // Extract all values from the range list (open-ended '$' bounds resolved, out-of-domain
        // values dropped/clamped per IEEE 1800-2023 19.5.7).
        std::vector<ArrayValue> values = extractArrayValues(arrayBinp, exprp);
        const int fixedCount = fixedArrayBinCount(arrayBinp, values);
        if (fixedCount == 0) return;  // Error already reported; values discarded

        if (fixedCount < 0) {  // Open 'bins b[]': one bin per distinct value, named b[value]
            const std::vector<ArrayValue> distinct = dedupArrayValues(values);
            for (const ArrayValue& value : distinct) {
                const string reportName = arrayBinp->name() + "[" + value.label + "]";
                AstVar* const varp = createTrackedCoverpointBinCounter(
                    coverpointp, arrayBinp,
                    sanitizeGeneratedName(arrayBinp->name() + "_" + value.label), atLeastValue,
                    "Created array bin [" + value.label + "]", "", reportName);
                generateArrayBinMatchCode(coverpointp, arrayBinp, exprp, varp, {value.nodep});
            }
            return;
        }

        // Fixed 'bins b[N]': N positional bins b[0]..b[N-1]; surplus bins stay empty (counter
        // created so they count toward coverage, but never incremented).
        const std::vector<std::vector<AstNodeExpr*>> groups
            = distributeFixedArrayValues(values, fixedCount);
        for (int bin = 0; bin < fixedCount; ++bin) {
            if (groups[bin].empty()) continue;
            const string reportName = arrayBinp->name() + "[" + std::to_string(bin) + "]";
            AstVar* const varp = createTrackedCoverpointBinCounter(
                coverpointp, arrayBinp, arrayBinp->name() + "_" + std::to_string(bin),
                atLeastValue, "Created array bin [" + std::to_string(bin) + "]", "", reportName);
            generateArrayBinMatchCode(coverpointp, arrayBinp, exprp, varp, groups[bin]);
        }
    }

    // Generate matching code for a single array bin element
    void generateArrayBinMatchCode(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                   AstNodeExpr* exprp, AstVar* hitVarp,
                                   const std::vector<AstNodeExpr*>& values) {
        AstNodeExpr* const condp = buildValueGroupCondition(binp->fileline(), exprp, values);

        addCoverpointBinHitIf(coverpointp, binp, hitVarp, condp,
                              "Illegal bin " + binp->prettyNameQ() + " hit in coverpoint "
                                  + coverpointp->prettyNameQ(),
                              "sample() CFunc not set when generating array bin code");
    }

    // Generate multiple bins for transition array bins
    // Array bins with transitions create one bin per transition sequence
    void generateTransitionArrayBins(AstCoverpoint* coverpointp, AstCoverBin* arrayBinp,
                                     AstNodeExpr* exprp, int atLeastValue) {
        UINFO(4, "    Generating transition array bins for: " << arrayBinp->name());

        // Extract all transition sets
        std::vector<AstCoverTransSet*> transSets;
        for (AstNode* transSetp = arrayBinp->transp(); transSetp; transSetp = transSetp->nextp())
            transSets.push_back(VN_AS(transSetp, CoverTransSet));

        UINFO(4, "      Found " << transSets.size() << " transition sets");
        int index = 0;
        for (AstCoverTransSet* transSetp : transSets) {
            std::vector<AstCoverTransItem*> items;
            for (AstNode* itemp = transSetp->itemsp(); itemp; itemp = itemp->nextp())
                items.push_back(VN_AS(itemp, CoverTransItem));
            if (items.size() != 2) {
                const string sanitizedName = arrayBinp->name() + "_" + std::to_string(index);
                AstVar* const varp = createTrackedCoverpointBinCounter(
                    coverpointp, arrayBinp, sanitizedName, atLeastValue,
                    "Created transition array bin [" + std::to_string(index) + "]", "",
                    arrayBinp->name() + "[" + std::to_string(index) + "]");
                generateSingleTransitionCode(coverpointp, arrayBinp, exprp, varp, transSetp);
                ++index;
                continue;
            }
            for (AstNode* fromp = items[0]->valuesp(); fromp; fromp = fromp->nextp()) {
                AstConst* const fromConstp = VN_CAST(fromp, Const);
                if (!fromConstp) continue;
                for (AstNode* top = items[1]->valuesp(); top; top = top->nextp()) {
                    AstConst* const toConstp = VN_CAST(top, Const);
                    if (!toConstp) continue;
                    const string label
                        = fromConstp->num().toDecimalU() + "=>" + toConstp->num().toDecimalU();
                    const string reportName = arrayBinp->name() + "[" + label + "]";
                    AstVar* const varp = createTrackedCoverpointBinCounter(
                        coverpointp, arrayBinp,
                        sanitizeGeneratedName(arrayBinp->name() + "_" + label), atLeastValue,
                        "Created transition array bin [" + label + "]", "", reportName);
                    AstVar* const prevVarp = createPrevValueVar(coverpointp, exprp);
                    AstNodeExpr* const condp = new AstAnd{
                        arrayBinp->fileline(),
                        new AstEq{arrayBinp->fileline(),
                                  new AstVarRef{arrayBinp->fileline(), prevVarp, VAccess::READ},
                                  fromConstp->cloneTree(false)},
                        new AstEq{arrayBinp->fileline(), exprp->cloneTree(false),
                                  toConstp->cloneTree(false)}};
                    addCoverpointBinHitIf(
                        coverpointp, arrayBinp, varp, condp,
                        "Illegal transition bin " + arrayBinp->prettyNameQ()
                            + " hit in coverpoint " + coverpointp->prettyNameQ(),
                        "sample() CFunc not set when generating transition array bin code");
                    ++index;
                }
            }
        }

        UINFO(4, "    Generated " << index << " transition array bins");
    }

    // Generate code for a single transition sequence (used by both regular and array bins)
    void generateSingleTransitionCode(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                      AstNodeExpr* exprp, AstVar* hitVarp,
                                      AstCoverTransSet* transSetp) {
        UINFO(4, "      Generating code for transition sequence");

        // Get or create previous value variable
        AstVar* const prevVarp = createPrevValueVar(coverpointp, exprp);

        UASSERT_OBJ(
            transSetp, binp,
            "Transition bin has no transition set (transp() was checked before calling this)");

        // Get transition items (the sequence: item1 => item2 => item3)
        std::vector<AstCoverTransItem*> items;
        for (AstNode* itemp = transSetp->itemsp(); itemp; itemp = itemp->nextp())
            items.push_back(VN_AS(itemp, CoverTransItem));

        if (items.empty()) {
            binp->v3error("Transition set without items");
            return;
        }

        if (items.size() == 1) {
            // Single item transition not valid (need at least 2 values for =>)
            binp->v3error("Transition requires at least two values");
            return;
        } else if (items.size() == 2) {
            // Simple two-value transition: (val1 => val2)
            // Use optimized direct comparison (no state machine needed)
            AstNodeExpr* const cond1p = buildTransitionItemCondition(items[0], prevVarp);
            AstNodeExpr* const cond2p = buildTransitionItemCondition(items[1], exprp);

            // Combine: prev matches val1 AND current matches val2
            AstNodeExpr* fullCondp = new AstAnd{binp->fileline(), cond1p, cond2p};

            addCoverpointBinHitIf(coverpointp, binp, hitVarp, fullCondp,
                                  "Illegal transition bin " + binp->prettyNameQ()
                                      + " hit in coverpoint " + coverpointp->prettyNameQ(),
                                  "sample() CFunc not set when generating transition bin code");

            UINFO(4, "        Successfully added 2-value transition if statement");
        } else {
            // Multi-value sequence (a => b => c => ...)
            // Use state machine to track position in sequence
            generateMultiValueTransitionCode(coverpointp, binp, exprp, hitVarp, items);
        }
    }

    // Recursive helper to generate Cartesian product of cross bins
    void generateCrossBinsRecursive(AstCoverCross* crossp,
                                    const std::vector<AstCoverpoint*>& coverpointRefs,
                                    const std::vector<std::vector<AstCoverBin*>>& allCpBins,
                                    std::vector<AstCoverBin*> currentCombination,
                                    size_t dimension) {
        if (dimension == allCpBins.size()) {
            // Base case: we have a complete combination, generate the cross bin
            generateOneCrossBin(crossp, coverpointRefs, currentCombination);
            return;
        }

        // Recursive case: iterate through bins at current dimension
        for (AstCoverBin* binp : allCpBins[dimension]) {
            currentCombination.push_back(binp);
            generateCrossBinsRecursive(crossp, coverpointRefs, allCpBins, currentCombination,
                                       dimension + 1);
            currentCombination.pop_back();
        }
    }

    // Generate a single cross bin for a specific combination of bins
    void generateOneCrossBin(AstCoverCross* crossp,
                             const std::vector<AstCoverpoint*>& coverpointRefs,
                             const std::vector<AstCoverBin*>& bins) {
        // Build sanitized name from all bins
        string binName;
        string varName = "__Vcov_" + crossp->name();
        string crossBins;  // Comma-separated individual bin names (one per coverpoint dimension)

        for (size_t i = 0; i < bins.size(); ++i) {
            const string sanitized = sanitizeGeneratedName(bins[i]->name());

            if (i > 0) {
                binName += "_x_";
                varName += "_x_";
                crossBins += ",";
            }
            binName += sanitized;
            varName += "_" + sanitized;
            crossBins += sanitized;
        }

        // Create member variable for this cross bin
        AstVar* const varp
            = createCoverageCounterVar(crossp->fileline(), varName, bins[0]->findUInt32DType());

        UINFO(4, "      Created cross bin variable: " << varName);

        // Track this for coverage computation
        AstCoverBin* const pseudoBinp = new AstCoverBin{
            crossp->fileline(), binName, static_cast<AstNode*>(nullptr), false, false};
        m_binInfos.push_back(BinInfo(pseudoBinp, varp, 1, nullptr, crossp, "", crossBins));

        // Generate matching code: if (bin1 && bin2 && ... && binN) varName++;
        generateNWayCrossBinMatchCode(crossp, coverpointRefs, bins, varp);
    }

    // Generate matching code for N-way cross bin
    void generateNWayCrossBinMatchCode(AstCoverCross* crossp,
                                       const std::vector<AstCoverpoint*>& coverpointRefs,
                                       const std::vector<AstCoverBin*>& bins, AstVar* hitVarp) {
        UINFO(4, "      Generating " << bins.size() << "-way cross bin match");

        // Build combined condition by ANDing all bin conditions
        AstNodeExpr* fullCondp = nullptr;

        for (size_t i = 0; i < bins.size(); ++i) {
            AstNodeExpr* const exprp = coverpointRefs[i]->exprp();
            AstNodeExpr* const condp = buildBinCondition(bins[i], exprp);

            if (fullCondp) {
                fullCondp = new AstAnd{crossp->fileline(), fullCondp, condp};
            } else {
                fullCondp = condp;
            }
        }

        // Generate: if (cond1 && cond2 && ... && condN) { ++varName; }
        AstNodeStmt* const incrp = makeBinHitIncrement(crossp->fileline(), hitVarp);

        AstIf* const ifp = new AstIf{crossp->fileline(), fullCondp, incrp};
        m_sampleFuncp->addStmtsp(ifp);
    }

    void generateCrossCode(AstCoverCross* crossp) {
        UINFO(4, "  Generating code for cross: " << crossp->name());

        // Resolve coverpoint references and build list
        std::vector<AstCoverpoint*> coverpointRefs;
        AstNode* itemp = crossp->itemsp();
        while (itemp) {
            AstNode* const nextp = itemp->nextp();
            AstCoverpointRef* const refp = VN_AS(itemp, CoverpointRef);
            // Find the referenced coverpoint via name map (O(log n) vs O(n) linear scan)
            const auto it = m_coverpointMap.find(refp->name());
            AstCoverpoint* const foundCpp = (it != m_coverpointMap.end()) ? it->second : nullptr;

            if (!foundCpp) {
                // Name not found as an explicit coverpoint - it's a direct variable
                // reference (implicit coverpoint), which Verilator does not support.
                // Warn and drop the whole cross.
                refp->v3warn(COVERIGN, "Unsupported: cross of '"
                                           << refp->prettyName()
                                           << "' which is not a coverpoint (implicit coverpoint)");
                return;
            }

            coverpointRefs.push_back(foundCpp);

            // Delete the reference node - it's no longer needed
            VL_DO_DANGLING(pushDeletep(refp->unlinkFrBack()), refp);
            itemp = nextp;
        }  // LCOV_EXCL_BR_LINE

        UINFO(4, "    Generating " << coverpointRefs.size() << "-way cross");

        // Collect bins from all coverpoints (excluding ignore/illegal bins)
        std::vector<std::vector<AstCoverBin*>> allCpBins;
        std::vector<AstCoverBin*> tempCrossBinps;
        for (AstCoverpoint* cpp : coverpointRefs) {
            std::vector<AstCoverBin*> cpBins;
            for (AstNode* binp = cpp->binsp(); binp; binp = binp->nextp()) {
                AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
                if (cbinp->binsType() != VCoverBinsType::BINS_USER) continue;
                if (cbinp->isArray() && !cbinp->transp()) {
                    std::vector<ArrayValue> values = extractArrayValues(cbinp, cpp->exprp());
                    const int fixedCount = fixedArrayBinCount(cbinp, values);
                    if (fixedCount < 0) {
                        std::vector<ArrayValue> distinct = dedupArrayValues(values);
                        for (ArrayValue& value : distinct) {
                            AstCoverBin* const pseudoBinp
                                = new AstCoverBin{cbinp->fileline(),
                                                  cbinp->name() + "[" + value.label + "]",
                                                  value.nodep,
                                                  false,
                                                  false,
                                                  cbinp->isWildcard()};
                            value.nodep = nullptr;
                            cpBins.push_back(pseudoBinp);
                            tempCrossBinps.push_back(pseudoBinp);
                        }
                    } else if (fixedCount > 0) {
                        const std::vector<std::vector<AstNodeExpr*>> groups
                            = distributeFixedArrayValues(values, fixedCount);
                        for (int bin = 0; bin < fixedCount; ++bin) {
                            if (groups[bin].empty()) continue;
                            AstNodeExpr* rangesp = nullptr;
                            AstNodeExpr* tailp = nullptr;
                            for (AstNodeExpr* const valuep : groups[bin]) {
                                if (tailp) {
                                    tailp->addNext(valuep);
                                } else {
                                    rangesp = valuep;
                                }
                                tailp = valuep;
                            }
                            AstCoverBin* const pseudoBinp
                                = new AstCoverBin{cbinp->fileline(),
                                                  cbinp->name() + "[" + std::to_string(bin) + "]",
                                                  rangesp,
                                                  false,
                                                  false,
                                                  cbinp->isWildcard()};
                            cpBins.push_back(pseudoBinp);
                            tempCrossBinps.push_back(pseudoBinp);
                        }
                    }
                    continue;
                }
                cpBins.push_back(cbinp);
            }
            UINFO(4, "      Found " << cpBins.size() << " bins in " << cpp->name());
            allCpBins.push_back(cpBins);
        }

        // Generate cross bins using Cartesian product
        generateCrossBinsRecursive(crossp, coverpointRefs, allCpBins, {}, 0);
        for (AstCoverBin* const binp : tempCrossBinps) VL_DO_DANGLING(binp->deleteTree(), binp);
    }

    AstNodeExpr* buildBinCondition(AstCoverBin* binp, AstNodeExpr* exprp) {
        // Get the range list from the bin
        AstNode* const rangep = binp->rangesp();
        if (!rangep) return nullptr;

        // Check if this is a wildcard bin
        const bool isWildcard = binp->isWildcard();

        // Build condition by OR-ing all ranges together
        AstNodeExpr* fullCondp = nullptr;

        for (AstNode* currRangep = rangep; currRangep; currRangep = currRangep->nextp()) {
            AstNodeExpr* rangeCondp = nullptr;

            if (AstInsideRange* irp = VN_CAST(currRangep, InsideRange)) {
                AstNodeExpr* const minExprp = irp->lhsp();
                AstNodeExpr* const maxExprp = irp->rhsp();
                AstConst* const minConstp = VN_CAST(minExprp, Const);
                AstConst* const maxConstp = VN_CAST(maxExprp, Const);
                const bool loUnbounded = VN_IS(minExprp, Unbounded);
                const bool hiUnbounded = VN_IS(maxExprp, Unbounded);
                if (loUnbounded || hiUnbounded) {
                    // Open-ended range: '$' is the coverpoint domain min/max, so the
                    // range reduces to a single inequality (e.g. {[10:$]} -> expr >= 10).
                    AstConst* const boundp = hiUnbounded ? minConstp : maxConstp;
                    if (loUnbounded && hiUnbounded) {
                        rangeCondp = new AstConst{irp->fileline(), AstConst::BitTrue{}};
                    } else if (!boundp) {
                        irp->v3error("Non-constant expression in bin range; "
                                     "range bounds must be constants");
                        return nullptr;
                    } else if (boundp->num().isFourState()) {
                        irp->v3error("Four-state (x/z) value in bin range bound; "
                                     "range bounds must be two-state constants");
                        return nullptr;
                    } else {
                        rangeCondp = makeOpenRangeCondition(irp->fileline(), exprp, boundp,
                                                            /*isLowerBound=*/hiUnbounded);
                    }
                } else if (!minConstp || !maxConstp) {
                    irp->v3error("Non-constant expression in bin range; "
                                 "range bounds must be constants");
                    return nullptr;
                } else if (minConstp->num().isFourState() || maxConstp->num().isFourState()) {
                    irp->v3error("Four-state (x/z) value in bin range bound; "
                                 "range bounds must be two-state constants");
                    return nullptr;
                } else if (minConstp->toUQuad() == maxConstp->toUQuad()) {
                    // Single value
                    if (isWildcard) {
                        rangeCondp = buildWildcardCondition(binp, exprp, minConstp);
                    } else {
                        rangeCondp = new AstEq{binp->fileline(), exprp->cloneTree(false),
                                               minExprp->cloneTree(false)};
                    }
                } else {
                    rangeCondp = makeRangeCondition(irp->fileline(), exprp, minExprp, maxExprp);
                }
            } else if (AstConst* constp = VN_CAST(currRangep, Const)) {
                if (isWildcard) {
                    rangeCondp = buildWildcardCondition(binp, exprp, constp);
                } else {
                    rangeCondp = new AstEq{binp->fileline(), exprp->cloneTree(false),
                                           constp->cloneTree(false)};
                }
            } else {
                currRangep->v3error(
                    "Non-constant expression in bin range; values must be constants");
                return nullptr;
            }

            UASSERT_OBJ(rangeCondp, binp, "rangeCondp is null after building range condition");
            fullCondp
                = fullCondp ? new AstOr{binp->fileline(), fullCondp, rangeCondp} : rangeCondp;
        }

        return fullCondp;
    }

    // Build a wildcard condition: (expr & mask) == (value & mask)
    // where mask has 1s for defined bits and 0s for wildcard bits
    // Non-owning: exprp is cloned internally; caller retains ownership.
    AstNodeExpr* buildWildcardCondition(AstCoverBin* binp, AstNodeExpr* exprp, AstConst* constp) {
        FileLine* const fl = binp->fileline();

        // Extract mask from constant (bits that are not X/Z)
        V3Number mask{constp, constp->width()};
        V3Number value{constp, constp->width()};

        for (int bit = 0; bit < constp->width(); ++bit) {
            if (constp->num().bitIs0(bit) || constp->num().bitIs1(bit)) {
                mask.setBit(bit, 1);
                value.setBit(bit, constp->num().bitIs1(bit) ? 1 : 0);
            } else {
                mask.setBit(bit, 0);
                value.setBit(bit, 0);
            }
        }

        // Generate: (expr & mask) == (value & mask)
        AstConst* const maskConstp = new AstConst{fl, mask};
        AstConst* const valueConstp = new AstConst{fl, value};

        AstNodeExpr* const exprMasked = new AstAnd{fl, exprp->cloneTree(false), maskConstp};
        AstNodeExpr* const valueMasked = new AstAnd{fl, valueConstp, maskConstp->cloneTree(false)};

        return new AstEq{fl, exprMasked, valueMasked};
    }

    void generateCoverageComputationCode() {
        UINFO(4, "  Generating coverage computation code");

        // Invalidate cache: addMembersp() calls in generateCoverpointCode/generateCrossCode
        // have added new members since the last scan, so clear before re-querying.
        m_memberMap.clear();

        // Find get_coverage() and get_inst_coverage() methods
        AstFunc* const getCoveragep
            = VN_CAST(m_memberMap.findMember(m_covergroupp, "get_coverage"), Func);
        AstFunc* const getInstCoveragep
            = VN_CAST(m_memberMap.findMember(m_covergroupp, "get_inst_coverage"), Func);

        // Even if there are no bins, we still need to generate the coverage methods
        // Empty covergroups should return 100% coverage
        if (m_binInfos.empty()) {
            UINFO(4, "    No bins found, will generate method to return 100%");
        } else {
            UINFO(6, "    Found " << m_binInfos.size() << " bins for coverage");
        }

        // Generate code for get_inst_coverage()
        generateCoverageMethodBody(getInstCoveragep);

        // Generate code for get_coverage() (type-level)
        // NOTE: Full type-level coverage requires instance tracking infrastructure
        // For now, return 0.0 as a placeholder
        AstVar* const coverageReturnVarp = VN_AS(getCoveragep->fvarp(), Var);
        // TODO: Implement proper type-level coverage aggregation
        // This requires tracking all instances and averaging their coverage
        // For now, return 0.0
        getCoveragep->addStmtsp(new AstAssign{
            getCoveragep->fileline(),
            new AstVarRef{getCoveragep->fileline(), coverageReturnVarp, VAccess::WRITE},
            new AstConst{getCoveragep->fileline(), AstConst::RealDouble{}, 0.0}});
        UINFO(4, "    Added placeholder get_coverage() (returns 0.0)");
    }

    void generateCoverageMethodBody(AstFunc* funcp) {
        FileLine* const fl = funcp->fileline();
        AstVar* const returnVarp = VN_AS(funcp->fvarp(), Var);

        // Converted coverpoints hold their bins in VlCoverpoint.  Combine their contributions
        // (via coverageParts) with any remaining legacy cross/cross-fed bins as the same flat
        // covered/total ratio the all-legacy path below computes.  Normal bins only: ignore,
        // illegal, and default are excluded (LRM 19.5).
        if (!m_convCpVars.empty()) {
            AstCStmt* const headp = new AstCStmt{fl};
            headp->add("double __Vcov = 0.0; double __Vtot = 0.0;");
            funcp->addStmtsp(headp);
            for (AstVar* const cpVarp : m_convCpVars) {
                AstCStmt* const cs = new AstCStmt{fl};
                cs->add("{ double __Vc = 0.0; double __Vt = 0.0; ");
                cs->add(memberRef(fl, cpVarp));
                cs->add(".coverageParts(__Vc, __Vt); __Vcov += __Vc; __Vtot += __Vt; }");
                funcp->addStmtsp(cs);
            }
            int legacyRegular = 0;
            for (const BinInfo& bi : m_binInfos) {
                if (!bi.binp->binsType().binIsNormal()) continue;
                ++legacyRegular;
                AstCStmt* const cs = new AstCStmt{fl};
                cs->add("if (");
                cs->add(memberRef(fl, bi.varp));
                cs->add(" >= " + std::to_string(bi.atLeast) + ") __Vcov += 1.0;");
                funcp->addStmtsp(cs);
            }
            if (legacyRegular) {
                AstCStmt* const cs = new AstCStmt{fl};
                cs->add("__Vtot += " + std::to_string(legacyRegular) + ".0;");
                funcp->addStmtsp(cs);
            }
            AstCStmt* const retp = new AstCStmt{fl};
            retp->add(new AstVarRef{fl, returnVarp, VAccess::WRITE});
            retp->add(" = (__Vtot != 0.0) ? (100.0 * __Vcov / __Vtot) : 0.0;");
            funcp->addStmtsp(retp);
            return;
        }

        // Count total bins (Normal only: excludes ignore/illegal/default)
        int totalBins = 0;
        for (const BinInfo& bi : m_binInfos) {
            UINFO(6, "      Bin: " << bi.binp->name() << " type=" << bi.binp->binsType().ascii());
            if (bi.binp->binsType().binIsNormal()) totalBins++;
        }

        UINFO(4, "    Total regular bins: " << totalBins << " of " << m_binInfos.size());

        if (totalBins == 0) {
            // No coverpoints/crosses preserves the historical empty-covergroup result.  A
            // non-empty covergroup with no contributing bins has a zero denominator.
            // Any parser-generated initialization of returnVar is overridden by our assignment.
            const double result = (m_coverpoints.empty() && m_coverCrosses.empty()) ? 100.0 : 0.0;
            UINFO(4, "    No contributing bins, returning " << result);
            funcp->addStmtsp(new AstAssign{fl, new AstVarRef{fl, returnVarp, VAccess::WRITE},
                                           new AstConst{fl, AstConst::RealDouble{}, result}});
            UINFO(4, "    Added zero-denominator assignment");
            return;
        }

        // Create local variable to count covered bins
        AstVar* const coveredCountp
            = new AstVar{fl, VVarType::BLOCKTEMP, "__Vcovered_count", funcp->findUInt32DType()};
        coveredCountp->funcLocal(true);
        funcp->addStmtsp(coveredCountp);

        // Initialize: covered_count = 0
        funcp->addStmtsp(new AstAssign{fl, new AstVarRef{fl, coveredCountp, VAccess::WRITE},
                                       new AstConst{fl, AstConst::WidthedValue{}, 32, 0}});

        // For each regular bin, if count > 0, increment covered_count
        for (const BinInfo& bi : m_binInfos) {
            // Skip ignore/illegal/default bins in coverage calculation
            if (!bi.binp->binsType().binIsNormal()) continue;

            // if (bin_count >= at_least) covered_count++;
            AstIf* ifp = new AstIf{
                fl,
                new AstGte{fl, new AstVarRef{fl, bi.varp, VAccess::READ},
                           new AstConst{fl, AstConst::WidthedValue{}, 32,
                                        static_cast<uint32_t>(bi.atLeast)}},
                new AstAssign{fl, new AstVarRef{fl, coveredCountp, VAccess::WRITE},
                              new AstAdd{fl, new AstVarRef{fl, coveredCountp, VAccess::READ},
                                         new AstConst{fl, AstConst::WidthedValue{}, 32, 1}}},
                nullptr};
            funcp->addStmtsp(ifp);
        }

        // Calculate coverage: (covered_count / total_bins) * 100.0
        // return_var = (double)covered_count / (double)total_bins * 100.0

        // Cast covered_count to real/double
        AstNodeExpr* const coveredReal
            = new AstIToRD{fl, new AstVarRef{fl, coveredCountp, VAccess::READ}};

        // Create total bins as a double constant
        AstNodeExpr* const totalReal
            = new AstConst{fl, AstConst::RealDouble{}, static_cast<double>(totalBins)};

        // Divide using AstDivD (double division that emits native /)
        AstNodeExpr* const divExpr = new AstDivD{fl, coveredReal, totalReal};

        // Multiply by 100 using AstMulD (double multiplication that emits native *)
        AstNodeExpr* const hundredConst = new AstConst{fl, AstConst::RealDouble{}, 100.0};
        AstNodeExpr* const coverageExpr = new AstMulD{fl, hundredConst, divExpr};

        // Assign to return variable
        funcp->addStmtsp(
            new AstAssign{fl, new AstVarRef{fl, returnVarp, VAccess::WRITE}, coverageExpr});

        UINFO(6, "    Added coverage computation to " << funcp->name() << " with " << totalBins
                                                      << " bins (excluding ignore/illegal)");
    }

    void generateCoverageRegistration() {
        // Generate VL_COVER_INSERT calls for each bin in the covergroup
        // This registers the bins with the coverage database so they can be reported

        UINFO(4,
              "  Generating coverage database registration for " << m_binInfos.size() << " bins");

        if (m_binInfos.empty()) return;

        // For each bin, generate a VL_COVER_INSERT call
        // The calls use CCall nodes to invoke VL_COVER_INSERT macro
        for (const BinInfo& binInfo : m_binInfos) {
            AstVar* const varp = binInfo.varp;
            AstCoverBin* const binp = binInfo.binp;
            AstCoverpoint* const coverpointp = binInfo.coverpointp;
            AstCoverCross* const crossp = binInfo.crossp;

            FileLine* const fl = binp->fileline();

            // Build hierarchical name: covergroup.coverpoint.bin or covergroup.cross.bin
            std::string hierName = m_covergroupp->name();
            const std::string binName
                = binInfo.reportName.empty() ? binp->name() : binInfo.reportName;

            if (coverpointp) {
                // Coverpoint bin: V3LinkParse guarantees a non-empty name for every
                // coverpoint (user label, single-variable name, or synthesized __Vcoverpoint<N>).
                UASSERT_OBJ(!coverpointp->name().empty(), coverpointp,
                            "Coverpoint without a name (should be set in V3LinkParse)");
                hierName += "." + coverpointp->name();
            } else {
                // Cross bin: grammar always provides a name (user label or auto "__crossN")
                hierName += "." + crossp->name();
            }
            hierName += "." + binName;

            // Generate: VL_COVER_INSERT(contextp, hier, &binVar, "page", "v_covergroup/...", ...)

            UINFO(6, "    Registering bin: " << hierName << " -> " << varp->name());

            // Build the coverage insert as a C statement mixing literal text with a proper
            // AstVarRef for the bin variable.  Using AstVarRef (with selfPointer=This) lets
            // V3Name apply __PVT__ mangling and the emitter apply nameProtect(), which also
            // handles --protect-ids correctly.  The vlSymsp->_vm_contextp__ path is the
            // established convention used by the existing __vlCoverInsert helper.
            // Use "page" field with v_covergroup prefix so the coverage type is identified
            // correctly (consistent with code coverage).
            const std::string pageName = "v_covergroup/" + m_covergroupp->name();
            AstCStmt* const cstmtp = new AstCStmt{fl};
            cstmtp->add("VL_COVER_INSERT(vlSymsp->_vm_contextp__->coveragep(), "
                        "\""
                        + hierName + "\", &(");
            AstVarRef* const binVarRefp = new AstVarRef{fl, varp, VAccess::READ};
            binVarRefp->selfPointer(VSelfPointerText{VSelfPointerText::This{}});
            cstmtp->add(binVarRefp);
            cstmtp->add("), \"page\", \"" + pageName
                        + "\", "
                          "\"filename\", \""
                        + fl->filename()
                        + "\", "
                          "\"lineno\", \""
                        + std::to_string(fl->lineno())
                        + "\", "
                          "\"column\", \""
                        + std::to_string(fl->firstColumn()) + "\", ");
            const std::string crossSuffix
                = crossp ? (", \"cross\", \"1\", \"cross_bins\", \"" + binInfo.crossBins + "\"")
                         : "";
            if (binp->binsType() == VCoverBinsType::BINS_IGNORE) {
                cstmtp->add("\"bin\", \"" + binName + "\", \"bin_type\", \"ignore\"" + crossSuffix
                            + ");");
            } else if (binp->binsType() == VCoverBinsType::BINS_ILLEGAL) {
                cstmtp->add("\"bin\", \"" + binName + "\", \"bin_type\", \"illegal\"" + crossSuffix
                            + ");");
            } else {
                cstmtp->add("\"bin\", \"" + binName + "\"" + crossSuffix + ");");
            }

            // Add to constructor
            m_constructorp->addStmtsp(cstmtp);

            UINFO(6, "      Added VL_COVER_INSERT call to constructor");
        }
    }

    // VISITORS
    AstNode* findEnclosingMemberRef(AstClass* cgClassp) {
        // An embedded covergroup is lowered into a sibling AstClass that has no handle to
        // the enclosing object.  A coverpoint/iff/cross expression that references a
        // (non-static) member of the enclosing class therefore emits C++ that accesses the
        // member as if it were static ("invalid use of non-static data member").  Detect
        // such references so the caller can skip lowering with a clean warning instead of
        // producing uncompilable code.  Returns the first offending node, or nullptr.
        // Collect the covergroup class's own member variables (sample/constructor args);
        // references to those are legitimate.
        std::set<const AstVar*> ownVars;
        for (AstNode* itemp = cgClassp->membersp(); itemp; itemp = itemp->nextp()) {
            if (const AstVar* const varp = VN_CAST(itemp, Var)) ownVars.insert(varp);
        }
        AstNode* offenderp = nullptr;
        const auto scan = [&](AstNode* rootp) {
            rootp->foreach([&](AstVarRef* refp) {
                if (offenderp) return;
                const AstVar* const varp = refp->varp();  // Always set post-LinkDot
                // A member of another class (the enclosing class) reached with no handle.
                // Members of the covergroup class itself (sample/constructor args) are
                // legitimate and excluded via ownVars.
                if (varp->isClassMember() && !ownVars.count(varp)) offenderp = refp;
            });
        };
        for (AstCoverpoint* cpp : m_coverpoints) scan(cpp);
        for (AstCoverCross* crossp : m_coverCrosses) scan(crossp);
        return offenderp;
    }

    void visit(AstClass* nodep) override {
        UINFO(9, "Visiting class: " << nodep->name() << " isCovergroup=" << nodep->isCovergroup());
        if (nodep->isCovergroup()) {
            VL_RESTORER(m_covergroupp);
            VL_RESTORER(m_sampleFuncp);
            VL_RESTORER(m_constructorp);
            VL_RESTORER(m_coverpoints);
            VL_RESTORER(m_coverpointMap);
            VL_RESTORER(m_coverCrosses);
            m_covergroupp = nodep;
            m_sampleFuncp = nullptr;
            m_constructorp = nullptr;
            m_coverpoints.clear();
            m_coverpointMap.clear();
            m_coverCrosses.clear();

            // Extract and store the clocking event from AstCovergroup node
            // The parser creates this node to preserve the event information
            bool hasUnsupportedEvent = false;
            for (AstNode* itemp = nodep->membersp(); itemp;) {
                AstNode* const nextp = itemp->nextp();
                if (AstCovergroup* const cgp = VN_CAST(itemp, Covergroup)) {
                    // Store the event in the global map for V3Active to retrieve later
                    // V3LinkParse only creates this sentinel AstCovergroup node when a clocking
                    // event exists, so cgp->eventp() is always non-null here.
                    UASSERT_OBJ(cgp->eventp(), cgp,
                                "Sentinel AstCovergroup in class must have non-null eventp");
                    // Check if the clocking event references a member variable (unsupported)
                    // Clocking events should be on signals/nets, not class members
                    bool eventUnsupported = false;
                    for (AstNode* senp = cgp->eventp()->sensesp(); senp; senp = senp->nextp()) {
                        AstSenItem* const senItemp = VN_AS(senp, SenItem);
                        if (AstVarRef* const varrefp  // LCOV_EXCL_BR_LINE
                            = VN_CAST(senItemp->sensp(), VarRef)) {
                            if (varrefp->varp()->isClassMember()) {
                                cgp->v3warn(COVERIGN, "Unsupported: 'covergroup' clocking event "
                                                      "on member variable");
                                eventUnsupported = true;
                                hasUnsupportedEvent = true;
                                break;
                            }
                        }
                    }

                    if (!eventUnsupported) {
                        // Leave cgp in the class membersp so the SenTree stays
                        // linked in the AST. V3Active will find it via membersp,
                        // use the event, then delete the AstCovergroup itself.
                        UINFO(4, "Keeping covergroup event node for V3Active: " << nodep->name());
                        itemp = nextp;
                        continue;
                    }
                    // Remove the AstCovergroup node - either unsupported event or no event
                    VL_DO_DANGLING(pushDeletep(cgp->unlinkFrBack()), cgp);
                }
                itemp = nextp;
            }

            // If covergroup has unsupported clocking event, skip processing it
            // but still clean up coverpoints so they don't reach downstream passes
            if (hasUnsupportedEvent) {
                iterateChildren(nodep);
                for (AstCoverpoint* cpp : m_coverpoints) {
                    VL_DO_DANGLING(pushDeletep(cpp->unlinkFrBack()), cpp);
                }
                for (AstCoverCross* crossp : m_coverCrosses) {
                    VL_DO_DANGLING(pushDeletep(crossp->unlinkFrBack()), crossp);
                }
                return;
            }

            // Find the sample() method and constructor
            m_sampleFuncp = VN_CAST(m_memberMap.findMember(nodep, "sample"), Func);
            // V3LinkParse always synthesizes a sample() method for every covergroup, and the
            // sampling-code generation below dereferences m_sampleFuncp unconditionally.
            UASSERT_OBJ(m_sampleFuncp, nodep, "Covergroup missing synthesized sample() method");
            m_sampleFuncp->isCovergroupSample(true);
            m_constructorp = VN_CAST(m_memberMap.findMember(nodep, "new"), Func);
            UINFO(9, "Found sample() method: " << (m_sampleFuncp ? "yes" : "no"));
            UINFO(9, "Found constructor: " << (m_constructorp ? "yes" : "no"));

            iterateChildren(nodep);

            // Option B safety net for embedded covergroups: if a coverpoint/iff/cross
            // references a member of the enclosing class, lowering would emit uncompilable
            // C++ (no handle to the enclosing instance).  Skip this covergroup with a clean
            // warning rather than crashing the C++ compile.  (Full support - an enclosing
            // back-pointer - is the planned follow-up.)
            if (AstNode* const offenderp = findEnclosingMemberRef(nodep)) {
                offenderp->v3warn(COVERIGN,
                                  "Unsupported: 'covergroup' coverpoint referencing enclosing "
                                  "class member; ignoring covergroup "
                                      << nodep->prettyNameQ());
                for (AstCoverpoint* cpp : m_coverpoints) {
                    VL_DO_DANGLING(pushDeletep(cpp->unlinkFrBack()), cpp);
                }
                for (AstCoverCross* crossp : m_coverCrosses) {
                    VL_DO_DANGLING(pushDeletep(crossp->unlinkFrBack()), crossp);
                }
                return;
            }

            processCovergroup();
            // Remove lowered coverpoints/crosses from the class - they have been
            // fully translated into C++ code and must not reach downstream passes
            for (AstCoverpoint* cpp : m_coverpoints) {
                VL_DO_DANGLING(pushDeletep(cpp->unlinkFrBack()), cpp);
            }
            for (AstCoverCross* crossp : m_coverCrosses) {
                VL_DO_DANGLING(pushDeletep(crossp->unlinkFrBack()), crossp);
            }
        } else {
            iterateChildren(nodep);
        }
    }

    void visit(AstCoverpoint* nodep) override {
        UINFO(9, "Found coverpoint: " << nodep->name());
        m_coverpoints.push_back(nodep);
        m_coverpointMap.emplace(nodep->name(), nodep);
        iterateChildren(nodep);
    }

    void visit(AstCoverCross* nodep) override {
        UINFO(9, "Found cross: " << nodep->name());
        m_coverCrosses.push_back(nodep);
        iterateChildren(nodep);
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit FunctionalCoverageVisitor(AstNetlist* nodep) { iterate(nodep); }
    ~FunctionalCoverageVisitor() override = default;
};

//######################################################################
// Functional coverage class functions

void V3Covergroup::covergroup(AstNetlist* nodep) {
    UINFO(4, __FUNCTION__ << ": ");
    { FunctionalCoverageVisitor{nodep}; }  // Destruct before checking
    V3Global::dumpCheckGlobalTree("coveragefunc", 0, dumpTreeEitherLevel() >= 3);
}
