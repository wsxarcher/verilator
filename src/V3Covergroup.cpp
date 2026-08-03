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
#include <vector>

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
    AstClass* m_enclosingClassp = nullptr;  // Class lexically enclosing the covergroup (if any)
    AstFunc* m_sampleFuncp = nullptr;  // Current sample() function
    AstFunc* m_constructorp = nullptr;  // Current constructor
    std::vector<AstCoverpoint*> m_coverpoints;  // Coverpoints in current covergroup
    std::map<std::string, AstCoverpoint*> m_coverpointMap;  // Name -> coverpoint for fast lookup
    std::vector<AstCoverCross*> m_coverCrosses;  // Cross coverage items in current covergroup
    uint32_t m_crossBinId = 0;  // Collision-proof internal ID for generated cross counters

    // Structure to track bins with their variables and options
    struct BinInfo final {
        AstCoverBin* binp;
        AstVar* varp;
        int atLeast;  // Minimum hits required for coverage (from option.at_least)
        AstCoverpoint* coverpointp;  // Associated coverpoint (or nullptr for cross bins)
        AstCoverCross* crossp;  // Associated cross (or nullptr for coverpoint bins)
        string crossBins;  // For cross bins: comma-separated individual bin names, in order
        BinInfo(AstCoverBin* b, AstVar* v, int al = 1, AstCoverpoint* cp = nullptr,
                AstCoverCross* cr = nullptr, const string& cb = "")
            : binp{b}
            , varp{v}
            , atLeast{al}
            , coverpointp{cp}
            , crossp{cr}
            , crossBins{cb} {}
    };
    std::vector<BinInfo> m_binInfos;  // All bins in current covergroup

    struct CrossRange final {
        V3Number lo;
        V3Number hi;
        CrossRange(const V3Number& loValue, const V3Number& hiValue)
            : lo{loValue}
            , hi{hiValue} {}
    };
    struct CrossBinInfo final {
        AstCoverBin* binp;
        string sourceName;
        CrossBinInfo(AstCoverBin* binValuep, const string& sourceNameValue)
            : binp{binValuep}
            , sourceName{sourceNameValue} {}
    };
    struct CrossSelectorLeafInfo final {
        size_t dimension;
        std::set<const AstCoverBin*> bins;
        CrossSelectorLeafInfo(size_t dimensionValue, const std::set<const AstCoverBin*>& binValues)
            : dimension{dimensionValue}
            , bins{binValues} {}
    };
    struct CrossIgnorePlan final {
        std::vector<const AstCoverCrossBinSel*> selectors;
        std::map<const AstCoverCrossBinSel*, CrossSelectorLeafInfo> leaves;
    };

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
        m_crossBinId = 0;
        for (AstCoverCross* crossp : m_coverCrosses) {
            for (AstNode* itemp = crossp->itemsp(); itemp; itemp = itemp->nextp()) {
                if (const AstCoverpointRef* const refp = VN_CAST(itemp, CoverpointRef))
                    if (!refp->exprp()) m_crossedCpNames.insert(refp->name());
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

                // Evaluate as constant
                const AstConst* constp = VN_CAST(sizep, Const);
                if (!constp) {
                    cbinp->v3error("Automatic bins array size must be a constant");
                    binp = nextBinp;
                    continue;
                }

                const int numBins = constp->toSInt();
                if (numBins <= 0) {
                    cbinp->v3error("Automatic bins array size must be >= 1, got " << numBins);
                    binp = nextBinp;
                    continue;
                }
                if (numBins > COVER_BINS_LIMIT) {
                    cbinp->v3error("Automatic bins array size of "
                                   << numBins << " exceeds limit of " << COVER_BINS_LIMIT);
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
                                              const string& logSuffix = "") {
        const string varName = "__Vcov_" + coverpointp->name() + "_" + generatedBinName;
        AstVar* const varp
            = createCoverageCounterVar(binp->fileline(), varName, binp->findUInt32DType());
        UINFO(4, "    " << logPrefix << ": " << varName << logSuffix);
        m_binInfos.push_back(BinInfo(binp, varp, atLeastValue, coverpointp));
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

    void addCoverpointBinHitIf(AstCoverpoint* coverpointp, AstCoverBin* binp, AstVar* hitVarp,
                               AstNodeExpr* condp, const string& illegalErrMsg,
                               const char* assertMsg) {
        AstNode* stmtp = makeBinHitIncrement(binp->fileline(), hitVarp);
        if (binp->binsType() == VCoverBinsType::BINS_ILLEGAL) {
            stmtp = stmtp->addNext(makeIllegalBinAction(binp->fileline(), illegalErrMsg));
        }

        AstIf* const ifp = new AstIf{
            binp->fileline(), applyCoverpointIffCondition(coverpointp, binp->fileline(), condp),
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

            if (!cbinp->isArray() && !cbinp->transp()
                && (cbinp->binsType() == VCoverBinsType::BINS_USER
                    || cbinp->binsType() == VCoverBinsType::BINS_WILDCARD)
                && !coverBinHasValues(cbinp, exprp))
                continue;

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

        if (!fullCondp) {
            // Reachable: e.g. 'ignore_bins ib = default' creates a BINS_IGNORE bin
            // with null rangesp. Skipping match code generation is correct in that case.
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

    static bool coverageAutoExtend(AstConst* valuep) {
        return valuep->num().autoExtend() || (!valuep->num().sized() && valuep->width() == 1);
    }

    static V3Number canonicalCoverageValue(AstConst* valuep, AstNodeExpr* exprp) {
        if (coverageAutoExtend(valuep)) {
            V3Number resolved{valuep, exprp->width(), 0};
            resolved.opExtendS(valuep->num(), valuep->width());
            V3Number value{valuep, exprp->width() + 1, 0};
            if (exprp->isSigned()) {
                value.opExtendS(resolved, exprp->width());
            } else {
                value.opAssign(resolved);
            }
            return value;
        }
        V3Number value{valuep, valuep->width() + 1, 0};
        if (valuep->isSigned()) {
            value.opExtendS(valuep->num(), valuep->width());
        } else {
            value.opAssign(valuep->num());
        }
        return value;
    }

    static V3Number coverageDomainBound(AstNode* nodep, int width, bool isSigned, bool upper) {
        V3Number rawValue{nodep, width, 0};
        if (upper) {
            rawValue.setAllBits1();
            if (isSigned) rawValue.setBit(width - 1, 0);
        } else {
            rawValue.setAllBits0();
            if (isSigned) rawValue.setBit(width - 1, 1);
        }
        V3Number value{nodep, width + 1, 0};
        if (isSigned) {
            value.opExtendS(rawValue, width);
        } else {
            value.opAssign(rawValue);
        }
        return value;
    }

    static V3Number coverpointDomainBound(AstNode* nodep, AstNodeExpr* exprp, bool upper) {
        return coverageDomainBound(nodep, exprp->width(), exprp->isSigned(), upper);
    }

    static bool coverageValueLte(AstNode* nodep, const V3Number& lhs, const V3Number& rhs) {
        const int width = std::max(lhs.width(), rhs.width());
        V3Number extendedLhs{nodep, width, 0};
        V3Number extendedRhs{nodep, width, 0};
        extendedLhs.opExtendS(lhs, lhs.width());
        extendedRhs.opExtendS(rhs, rhs.width());
        V3Number result{nodep, 1, 0};
        result.opLteS(extendedLhs, extendedRhs);
        return result.isNeqZero();
    }

    static bool coverageValuesEqual(AstNode* nodep, const V3Number& lhs, const V3Number& rhs) {
        const int width = std::max(lhs.width(), rhs.width());
        V3Number extendedLhs{nodep, width, 0};
        V3Number extendedRhs{nodep, width, 0};
        extendedLhs.opExtendS(lhs, lhs.width());
        extendedRhs.opExtendS(rhs, rhs.width());
        V3Number result{nodep, 1, 0};
        result.opEq(extendedLhs, extendedRhs);
        return result.isNeqZero();
    }

    static bool coverageValueFits(AstConst* valuep, AstNodeExpr* exprp) {
        if (valuep->num().isOpaque() || valuep->num().isFourState()) return false;
        const V3Number value = canonicalCoverageValue(valuep, exprp);
        const V3Number domainLo = coverpointDomainBound(valuep, exprp, false);
        const V3Number domainHi = coverpointDomainBound(valuep, exprp, true);
        return coverageValueLte(valuep, domainLo, value)
               && coverageValueLte(valuep, value, domainHi);
    }

    static V3Number resizedCoverageValue(AstNode* nodep, AstConst* valuep, int width) {
        V3Number value{nodep, width, 0};
        if (coverageAutoExtend(valuep) || valuep->isSigned()) {
            value.opExtendS(valuep->num(), valuep->width());
        } else {
            value.opAssign(valuep->num());
        }
        return value;
    }

    static void deleteArrayValues(std::vector<AstNodeExpr*>& values) {
        for (AstNodeExpr* const valuep : values) VL_DO_DANGLING(valuep->deleteTree(), valuep);
        values.clear();
    }

    bool arrayValuesLimitExceeded(AstCoverBin* arrayBinp, std::vector<AstNodeExpr*>& values) {
        if (values.size() < static_cast<size_t>(COVER_BINS_LIMIT)) return false;
        arrayBinp->v3warn(COVERIGN, "Unsupported: array 'bins' covering more than "
                                        << COVER_BINS_LIMIT
                                        << " values (e.g. an open '[lo:$]' range over "
                                           "a wide coverpoint); bin ignored");
        deleteArrayValues(values);
        return true;
    }

    bool appendWildcardArrayValues(AstCoverBin* arrayBinp, AstConst* patternp, AstNodeExpr* exprp,
                                   std::vector<AstNodeExpr*>& values) {
        const int width = exprp->width();
        const bool autoExtend = coverageAutoExtend(patternp);
        const int patternWidth = autoExtend ? width : patternp->width();
        const bool patternSigned = autoExtend ? exprp->isSigned() : patternp->isSigned();
        V3Number pattern{patternp, patternWidth, 0};
        if (autoExtend || patternp->isSigned()) {
            pattern.opExtendS(patternp->num(), patternp->width());
        } else {
            pattern.opAssign(patternp->num());
        }

        std::vector<int> wildcardBits;
        for (int bit = 0; bit < patternWidth; ++bit) {
            if (pattern.bitIsXZ(bit)) wildcardBits.push_back(bit);
        }
        size_t combinations = 1;
        for (size_t bit = 0; bit < wildcardBits.size(); ++bit) {
            if (combinations > static_cast<size_t>(COVER_BINS_LIMIT) / 2) {
                arrayBinp->v3warn(COVERIGN, "Unsupported: array 'bins' covering more than "
                                                << COVER_BINS_LIMIT
                                                << " values (e.g. an open '[lo:$]' range over "
                                                   "a wide coverpoint); bin ignored");
                deleteArrayValues(values);
                return false;
            }
            combinations *= 2;
        }
        for (size_t combination = 0; combination < combinations; ++combination) {
            V3Number value{patternp, patternWidth, 0};
            value.opAssign(pattern);
            for (size_t index = 0; index < wildcardBits.size(); ++index) {
                value.setBit(wildcardBits[index], (combination >> index) & 1);
            }
            V3Number canonical{patternp, patternWidth + 1, 0};
            if (patternSigned) {
                canonical.opExtendS(value, patternWidth);
            } else {
                canonical.opAssign(value);
            }
            const V3Number domainLo = coverpointDomainBound(arrayBinp, exprp, false);
            const V3Number domainHi = coverpointDomainBound(arrayBinp, exprp, true);
            if (!coverageValueLte(patternp, domainLo, canonical)
                || !coverageValueLte(patternp, canonical, domainHi))
                continue;
            if (arrayValuesLimitExceeded(arrayBinp, values)) return false;
            V3Number resized{patternp, width, 0};
            if (patternSigned) {
                resized.opExtendS(value, patternWidth);
            } else {
                resized.opAssign(value);
            }
            AstConst* const valuep = new AstConst{patternp->fileline(), resized};
            valuep->dtypeFrom(exprp);
            values.push_back(valuep);
        }
        return true;
    }

    // Individual equality targets of an array bin (bins b[] = {values/ranges}), in order.
    // Bounds are clipped to the coverpoint domain before enumeration. One target is produced per
    // value; declarations exceeding COVER_BINS_LIMIT are ignored with COVERIGN.
    std::vector<AstNodeExpr*> extractArrayValues(AstCoverBin* arrayBinp, AstNodeExpr* exprp,
                                                 bool& unsupportedOut) {
        unsupportedOut = false;
        const int width = exprp->width();
        const V3Number domainLo = coverpointDomainBound(arrayBinp, exprp, false);
        const V3Number domainHi = coverpointDomainBound(arrayBinp, exprp, true);
        std::vector<AstNodeExpr*> values;
        for (AstNode* rangep = arrayBinp->rangesp(); rangep; rangep = rangep->nextp()) {
            if (AstInsideRange* const irp = VN_CAST(rangep, InsideRange)) {
                AstNodeExpr* const lhsp = V3Const::constifyEdit(irp->lhsp());
                AstNodeExpr* const rhsp = V3Const::constifyEdit(irp->rhsp());
                const bool loUnb = VN_IS(lhsp, Unbounded);
                const bool hiUnb = VN_IS(rhsp, Unbounded);
                AstConst* const minp = VN_CAST(lhsp, Const);
                AstConst* const maxp = VN_CAST(rhsp, Const);
                if ((!minp && !loUnb) || (!maxp && !hiUnb)) {
                    arrayBinp->v3error("Non-constant expression in array bins range; "
                                       "range bounds must be constants");
                    return values;
                }
                if ((minp && minp->num().isFourState()) || (maxp && maxp->num().isFourState())) {
                    arrayBinp->v3error("Four-state (x/z) value in array bins range bound; "
                                       "range bounds must be two-state constants");
                    return values;
                }
                if ((minp && minp->num().isOpaque()) || (maxp && maxp->num().isOpaque())) {
                    arrayBinp->v3warn(COVERIGN, "Unsupported: array 'bins' with non-integral "
                                                "range bound; bin ignored");
                    unsupportedOut = true;
                    deleteArrayValues(values);
                    return values;
                }

                V3Number lo = loUnb ? domainLo : canonicalCoverageValue(minp, exprp);
                V3Number hi = hiUnb ? domainHi : canonicalCoverageValue(maxp, exprp);
                if (!coverageValueLte(irp, lo, hi) || !coverageValueLte(irp, lo, domainHi)
                    || !coverageValueLte(irp, domainLo, hi))
                    continue;
                if (coverageValueLte(irp, lo, domainLo)) lo = domainLo;
                if (coverageValueLte(irp, domainHi, hi)) hi = domainHi;

                const int iterationWidth = std::max(lo.width(), hi.width());
                V3Number value{irp, iterationWidth, 0};
                V3Number finalValue{irp, iterationWidth, 0};
                value.opExtendS(lo, lo.width());
                finalValue.opExtendS(hi, hi.width());
                V3Number one{irp, iterationWidth, 1};
                while (true) {
                    if (arrayValuesLimitExceeded(arrayBinp, values)) {
                        unsupportedOut = true;
                        return values;
                    }
                    V3Number resized{irp, width, 0};
                    resized.opExtendS(value, value.width());
                    AstConst* const valuep = new AstConst{irp->fileline(), resized};
                    valuep->dtypeFrom(exprp);
                    values.push_back(valuep);
                    if (coverageValuesEqual(irp, value, finalValue)) break;
                    V3Number nextValue{irp, iterationWidth, 0};
                    nextValue.opAdd(value, one);
                    value = nextValue;
                }
            } else if (AstConst* constp = VN_CAST(rangep, Const)) {
                if (arrayBinp->isWildcard() && constp->num().isFourState()) {
                    if (!appendWildcardArrayValues(arrayBinp, constp, exprp, values)) {
                        unsupportedOut = true;
                        return values;
                    }
                } else if (coverageValueFits(constp, exprp)) {
                    const V3Number value = resizedCoverageValue(constp, constp, width);
                    AstConst* const valuep = new AstConst{constp->fileline(), value};
                    valuep->dtypeFrom(exprp);
                    values.push_back(valuep);
                }
            } else {
                values.push_back(VN_AS(rangep->cloneTree(false), NodeExpr));
            }
        }
        return values;
    }

    // Emit a 'this->m_cp.addSingleNamer/addArrayNamer(...)' statement for one bin
    AstCStmt* makeNamer(AstVar* cpVarp, AstCoverBin* binp, int count) {
        FileLine* const fl = binp->fileline();
        AstCStmt* const cs = new AstCStmt{fl};
        cs->add(memberRef(fl, cpVarp));
        const std::string loc = "\"" + std::string{fl->filename()} + "\", "
                                + std::to_string(fl->lineno()) + ", "
                                + std::to_string(fl->firstColumn()) + ");";
        if (count < 0) {  // single bin
            cs->add(".addSingleNamer(" + std::string{binp->binsType().binSetEnum()} + ", \""
                    + binp->name() + "\", " + loc);
        } else {  // value array bin
            cs->add(".addArrayNamer(" + std::string{binp->binsType().binSetEnum()} + ", "
                    + std::to_string(count) + ", \"" + binp->name() + "\", " + loc);
        }
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
        AstNodeExpr* const guardedp = applyCoverpointIffCondition(coverpointp, fl, condp);
        UASSERT_OBJ(m_sampleFuncp, binp, "sample() CFunc not set in converted coverpoint");
        m_sampleFuncp->addStmtsp(new AstIf{fl, guardedp, actionp, nullptr});
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
        int idx = 0;
        for (AstNode* binp = coverpointp->binsp(); binp; binp = binp->nextp()) {
            AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
            if (cbinp->binsType() == VCoverBinsType::BINS_DEFAULT) {
                defaultBins.push_back(cbinp);
                continue;
            }
            if (!cbinp->isArray()
                && (cbinp->binsType() == VCoverBinsType::BINS_USER
                    || cbinp->binsType() == VCoverBinsType::BINS_WILDCARD)
                && !coverBinHasValues(cbinp, exprp))
                continue;
            if (cbinp->isArray()) {  // value array: bins b[N] = {...} -> b[0]..b[N-1]
                bool unsupported = false;
                std::vector<AstNodeExpr*> values = extractArrayValues(cbinp, exprp, unsupported);
                if (unsupported) continue;  // bin ignored (COVERIGN emitted); reserve no slot
                namerStmts.push_back(makeNamer(cpVarp, cbinp, static_cast<int>(values.size())));
                for (AstNodeExpr* valuep : values) {
                    emitConvHitIf(coverpointp, cbinp, cpVarp, idx++,
                                  new AstEq{cbinp->fileline(), exprp->cloneTree(false), valuep});
                }
            } else {
                namerStmts.push_back(makeNamer(cpVarp, cbinp, -1));
                // buildBinCondition is null for 'ignore_bins = default' (no ranges); the bin
                // still gets a reserved slot (recorded, never incremented).
                if (AstNodeExpr* const condp = buildBinCondition(cbinp, exprp))
                    emitConvHitIf(coverpointp, cbinp, cpVarp, idx, condp);
                ++idx;
            }
        }
        for (AstCoverBin* const defBinp : defaultBins) {
            namerStmts.push_back(makeNamer(cpVarp, defBinp, -1));
            emitConvHitIf(coverpointp, defBinp, cpVarp, idx++,
                          buildDefaultCondition(coverpointp, exprp, defBinp->fileline()));
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

        AstNodeExpr* defaultCondp = buildDefaultCondition(coverpointp, exprp, defBinp->fileline());

        // Apply iff condition if present
        if (AstNodeExpr* iffp = coverpointp->iffp()) {
            defaultCondp = new AstAnd{defBinp->fileline(), iffp->cloneTree(false), defaultCondp};
        }

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

        // Apply iff condition if present
        if (AstNodeExpr* iffp = coverpointp->iffp()) {
            matchCondp = new AstAnd{fl, iffp->cloneTree(false), matchCondp};
        }

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
            // Apply iff condition
            if (AstNodeExpr* iffp = coverpointp->iffp()) {
                restartCondp = new AstAnd{fl, iffp->cloneTree(false), restartCondp};
            }

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

    // Clone a constant node, resizing to targetWidth using its self-determined signedness.
    // Used to ensure comparisons use matching widths after V3Width has run.
    static AstConst* widenConst(FileLine* fl, AstConst* constp, int targetWidth) {
        if (constp->width() == targetWidth) return constp->cloneTree(false);
        const V3Number num = resizedCoverageValue(constp, constp, targetWidth);
        return new AstConst{fl, num};
    }

    static AstNodeExpr* makeValueCondition(FileLine* fl, AstNodeExpr* exprp, AstConst* valuep) {
        if (!coverageValueFits(valuep, exprp)) return new AstConst{fl, AstConst::BitFalse{}};
        return new AstEq{fl, exprp->cloneTree(false), widenConst(fl, valuep, exprp->widthMin())};
    }

    static AstConst* newCoverageConst(FileLine* fl, AstNodeExpr* exprp,
                                      const V3Number& canonical) {
        V3Number resized{fl, exprp->widthMin(), 0};
        resized.opExtendS(canonical, canonical.width());
        AstConst* const constp = new AstConst{fl, resized};
        constp->dtypeFrom(exprp);
        return constp;
    }

    AstNodeExpr* makeCanonicalRangeCondition(FileLine* fl, AstNodeExpr* exprp, V3Number lo,
                                             V3Number hi) {
        const V3Number domainLo = coverpointDomainBound(exprp, exprp, false);
        const V3Number domainHi = coverpointDomainBound(exprp, exprp, true);
        if (!coverageValueLte(exprp, lo, hi) || !coverageValueLte(exprp, lo, domainHi)
            || !coverageValueLte(exprp, domainLo, hi))
            return new AstConst{fl, AstConst::BitFalse{}};
        if (coverageValueLte(exprp, lo, domainLo)) lo = domainLo;
        if (coverageValueLte(exprp, domainHi, hi)) hi = domainHi;
        if (coverageValuesEqual(exprp, lo, hi))
            return new AstEq{fl, exprp->cloneTree(false), newCoverageConst(fl, exprp, lo)};

        const bool skipLowerCheck = coverageValuesEqual(exprp, lo, domainLo);
        const bool skipUpperCheck = coverageValuesEqual(exprp, hi, domainHi);
        if (skipLowerCheck && skipUpperCheck) {
            return new AstConst{fl, AstConst::BitTrue{}};
        } else if (skipLowerCheck) {
            AstConst* const hip = newCoverageConst(fl, exprp, hi);
            if (exprp->isSigned()) return new AstLteS{fl, exprp->cloneTree(false), hip};
            return new AstLte{fl, exprp->cloneTree(false), hip};
        } else if (skipUpperCheck) {
            AstConst* const lop = newCoverageConst(fl, exprp, lo);
            if (exprp->isSigned()) return new AstGteS{fl, exprp->cloneTree(false), lop};
            return new AstGte{fl, exprp->cloneTree(false), lop};
        }
        AstConst* const lop = newCoverageConst(fl, exprp, lo);
        AstConst* const hip = newCoverageConst(fl, exprp, hi);
        if (exprp->isSigned()) {
            return new AstAnd{fl, new AstGteS{fl, exprp->cloneTree(false), lop},
                              new AstLteS{fl, exprp->cloneTree(false), hip}};
        }
        return new AstAnd{fl, new AstGte{fl, exprp->cloneTree(false), lop},
                          new AstLte{fl, exprp->cloneTree(false), hip}};
    }

    // Build a range condition: minp <= exprp <= maxp.
    // Bounds are clipped to the expression domain before creating width-matched comparisons.
    AstNodeExpr* makeRangeCondition(FileLine* fl, AstNodeExpr* exprp, AstNodeExpr* minp,
                                    AstNodeExpr* maxp) {
        AstConst* const minConstp = VN_AS(minp, Const);
        AstConst* const maxConstp = VN_AS(maxp, Const);
        return makeCanonicalRangeCondition(fl, exprp, canonicalCoverageValue(minConstp, exprp),
                                           canonicalCoverageValue(maxConstp, exprp));
    }

    // Build a one-sided comparison for an open-ended bin range whose other bound is '$'.
    // '$' denotes the coverpoint domain extreme, so {[lo:$]} == (expr >= lo) and
    // {[$:hi]} == (expr <= hi).
    AstNodeExpr* makeOpenRangeCondition(FileLine* fl, AstNodeExpr* exprp, AstConst* boundp,
                                        bool isLowerBound) {
        const V3Number bound = canonicalCoverageValue(boundp, exprp);
        const V3Number domainLo = coverpointDomainBound(exprp, exprp, false);
        const V3Number domainHi = coverpointDomainBound(exprp, exprp, true);
        if (isLowerBound) {
            if (!coverageValueLte(exprp, bound, domainHi))
                return new AstConst{fl, AstConst::BitFalse{}};
            if (coverageValueLte(exprp, bound, domainLo))
                return new AstConst{fl, AstConst::BitTrue{}};
            AstConst* const widep = newCoverageConst(fl, exprp, bound);
            if (exprp->isSigned()) return new AstGteS{fl, exprp->cloneTree(false), widep};
            return new AstGte{fl, exprp->cloneTree(false), widep};
        }
        if (!coverageValueLte(exprp, domainLo, bound))
            return new AstConst{fl, AstConst::BitFalse{}};
        if (coverageValueLte(exprp, domainHi, bound)) return new AstConst{fl, AstConst::BitTrue{}};
        AstConst* const widep = newCoverageConst(fl, exprp, bound);
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

        // Extract all values from the range list (resolves '$', caps/ignores huge ranges)
        bool unsupported = false;
        std::vector<AstNodeExpr*> values = extractArrayValues(arrayBinp, exprp, unsupported);
        if (unsupported) return;  // bin ignored (COVERIGN emitted)

        // Create a separate bin for each value
        int index = 0;
        for (AstNodeExpr* valuep : values) {
            const string sanitizedName = arrayBinp->name() + "_" + std::to_string(index);
            AstVar* const varp = createTrackedCoverpointBinCounter(
                coverpointp, arrayBinp, sanitizedName, atLeastValue,
                "Created array bin [" + std::to_string(index) + "]");

            // Generate matching code for this specific value
            generateArrayBinMatchCode(coverpointp, arrayBinp, exprp, varp, valuep);

            ++index;
        }

        UINFO(4, "    Generated " << index << " array bins");
    }

    // Generate matching code for a single array bin element
    void generateArrayBinMatchCode(AstCoverpoint* coverpointp, AstCoverBin* binp,
                                   AstNodeExpr* exprp, AstVar* hitVarp, AstNodeExpr* valuep) {
        // Create condition: expr == value
        AstNodeExpr* condp = new AstEq{binp->fileline(), exprp->cloneTree(false), valuep};

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
            const string sanitizedName = arrayBinp->name() + "_" + std::to_string(index);
            AstVar* const varp = createTrackedCoverpointBinCounter(
                coverpointp, arrayBinp, sanitizedName, atLeastValue,
                "Created transition array bin [" + std::to_string(index) + "]");

            // Generate matching code for this specific transition
            generateSingleTransitionCode(coverpointp, arrayBinp, exprp, varp, transSetp);

            ++index;
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

    static bool collectCrossRange(AstNode* itemp, AstNodeExpr* exprp, const V3Number& domainLo,
                                  const V3Number& domainHi, std::vector<CrossRange>& ranges) {
        V3Number lo{itemp, 1, 0};
        V3Number hi{itemp, 1, 0};
        if (AstConst* constp = VN_CAST(itemp, Const)) {
            if (constp->num().isOpaque()) {
                constp->v3warn(COVERIGN, "Ignoring unsupported coverage cross ignore_bins "
                                         "selector with non-integral value.");
                return false;
            }
            if (constp->num().isFourState()) {
                constp->v3error("Four-state (x/z) value in coverage cross selector; "
                                "values must be two-state constants.");
                return false;
            }
            lo = canonicalCoverageValue(constp, exprp);
            hi = lo;
        } else if (AstInsideRange* const rangep = VN_CAST(itemp, InsideRange)) {
            AstNodeExpr* const lhsp = rangep->lhsp();
            AstNodeExpr* const rhsp = rangep->rhsp();
            AstConst* const loConstp = VN_CAST(lhsp, Const);
            AstConst* const hiConstp = VN_CAST(rhsp, Const);
            const bool loUnbounded = VN_IS(lhsp, Unbounded);
            const bool hiUnbounded = VN_IS(rhsp, Unbounded);
            if ((!loConstp && !loUnbounded) || (!hiConstp && !hiUnbounded)) {
                rangep->v3warn(COVERIGN, "Ignoring unsupported coverage cross ignore_bins "
                                         "selector with non-constant range bound.");
                return false;
            }
            if ((loConstp && loConstp->num().isOpaque())
                || (hiConstp && hiConstp->num().isOpaque())) {
                rangep->v3warn(COVERIGN, "Ignoring unsupported coverage cross ignore_bins "
                                         "selector with non-integral range bound.");
                return false;
            }
            if ((loConstp && loConstp->num().isFourState())
                || (hiConstp && hiConstp->num().isFourState())) {
                rangep->v3error("Four-state (x/z) value in coverage cross selector range; "
                                "range bounds must be two-state constants.");
                return false;
            }
            lo = loUnbounded ? domainLo : canonicalCoverageValue(loConstp, exprp);
            hi = hiUnbounded ? domainHi : canonicalCoverageValue(hiConstp, exprp);
        } else {
            itemp->v3warn(COVERIGN, "Ignoring unsupported coverage cross ignore_bins "
                                    "selector with non-constant value.");
            return false;
        }

        // Reversed ranges and ranges outside the coverpoint domain select no bins.
        if (!coverageValueLte(itemp, lo, hi) || !coverageValueLte(itemp, lo, domainHi)
            || !coverageValueLte(itemp, domainLo, hi))
            return true;
        const V3Number clippedLo = coverageValueLte(itemp, lo, domainLo) ? domainLo : lo;
        const V3Number clippedHi = coverageValueLte(itemp, domainHi, hi) ? domainHi : hi;
        ranges.emplace_back(clippedLo, clippedHi);
        return true;
    }

    static bool collectCrossRanges(AstNode* itemsp, AstNodeExpr* exprp,
                                   std::vector<CrossRange>& ranges) {
        const V3Number domainLo = coverpointDomainBound(exprp, exprp, false);
        const V3Number domainHi = coverpointDomainBound(exprp, exprp, true);
        for (AstNode* itemp = itemsp; itemp; itemp = itemp->nextp()) {
            if (!collectCrossRange(itemp, exprp, domainLo, domainHi, ranges)) return false;
        }
        return true;
    }

    static bool crossRangeSetsOverlap(AstNode* nodep, const std::vector<CrossRange>& lhs,
                                      const std::vector<CrossRange>& rhs) {
        for (const CrossRange& left : lhs) {
            for (const CrossRange& right : rhs) {
                if (coverageValueLte(nodep, left.lo, right.hi)
                    && coverageValueLte(nodep, right.lo, left.hi))
                    return true;
            }
        }
        return false;
    }

    static bool wildcardPatternOverlapsUnsignedRange(const V3Number& pattern, const V3Number& lo,
                                                     const V3Number& hi, int width) {
        bool possible[2][2] = {};
        possible[1][1] = true;
        for (int bit = width - 1; bit >= 0; --bit) {
            bool nextPossible[2][2] = {};
            const int loBit = lo.bitIs1(bit) ? 1 : 0;
            const int hiBit = hi.bitIs1(bit) ? 1 : 0;
            for (int tightLo = 0; tightLo <= 1; ++tightLo) {
                for (int tightHi = 0; tightHi <= 1; ++tightHi) {
                    if (!possible[tightLo][tightHi]) continue;
                    for (int value = 0; value <= 1; ++value) {
                        if ((pattern.bitIs0(bit) && value != 0)
                            || (pattern.bitIs1(bit) && value != 1))
                            continue;
                        if ((tightLo && value < loBit) || (tightHi && value > hiBit)) continue;
                        const int nextTightLo = tightLo && value == loBit;
                        const int nextTightHi = tightHi && value == hiBit;
                        nextPossible[nextTightLo][nextTightHi] = true;
                    }
                }
            }
            for (int tightLo = 0; tightLo <= 1; ++tightLo) {
                for (int tightHi = 0; tightHi <= 1; ++tightHi)
                    possible[tightLo][tightHi] = nextPossible[tightLo][tightHi];
            }
        }
        return possible[0][0] || possible[0][1] || possible[1][0] || possible[1][1];
    }

    static bool wildcardPatternOverlapsRange(AstConst* patternp, AstNodeExpr* exprp,
                                             const CrossRange& range) {
        const bool autoExtend = coverageAutoExtend(patternp);
        const int patternWidth = autoExtend ? exprp->width() : patternp->width();
        const bool patternSigned = autoExtend ? exprp->isSigned() : patternp->isSigned();
        V3Number pattern{patternp, patternWidth, 0};
        if (autoExtend || patternp->isSigned()) {
            pattern.opExtendS(patternp->num(), patternp->width());
        } else {
            pattern.opAssign(patternp->num());
        }

        const V3Number patternDomainLo
            = coverageDomainBound(patternp, patternWidth, patternSigned, false);
        const V3Number patternDomainHi
            = coverageDomainBound(patternp, patternWidth, patternSigned, true);
        if (!coverageValueLte(patternp, range.lo, patternDomainHi)
            || !coverageValueLte(patternp, patternDomainLo, range.hi))
            return false;
        const V3Number clippedLo
            = coverageValueLte(patternp, range.lo, patternDomainLo) ? patternDomainLo : range.lo;
        const V3Number clippedHi
            = coverageValueLte(patternp, patternDomainHi, range.hi) ? patternDomainHi : range.hi;

        V3Number lo{patternp, patternWidth, 0};
        V3Number hi{patternp, patternWidth, 0};
        lo.opAssign(clippedLo);
        hi.opAssign(clippedHi);
        if (!patternSigned)
            return wildcardPatternOverlapsUnsignedRange(pattern, lo, hi, patternWidth);

        const bool loNegative = clippedLo.bitIs1(clippedLo.width() - 1);
        const bool hiNegative = clippedHi.bitIs1(clippedHi.width() - 1);
        if (loNegative == hiNegative)
            return wildcardPatternOverlapsUnsignedRange(pattern, lo, hi, patternWidth);

        UASSERT_OBJ(loNegative && !hiNegative, patternp,
                    "Signed coverage selector range has reversed signs");
        V3Number zero{patternp, patternWidth, 0};
        V3Number allOnes{patternp, patternWidth, 0};
        allOnes.setAllBits1();
        return wildcardPatternOverlapsUnsignedRange(pattern, lo, allOnes, patternWidth)
               || wildcardPatternOverlapsUnsignedRange(pattern, zero, hi, patternWidth);
    }

    static bool wildcardBinOverlapsRanges(AstCoverBin* binp, AstNodeExpr* exprp,
                                          const std::vector<CrossRange>& selectorRanges,
                                          bool& overlaps) {
        overlaps = false;
        const V3Number domainLo = coverpointDomainBound(exprp, exprp, false);
        const V3Number domainHi = coverpointDomainBound(exprp, exprp, true);
        for (AstNode* itemp = binp->rangesp(); itemp; itemp = itemp->nextp()) {
            if (AstConst* const patternp = VN_CAST(itemp, Const)) {
                if (patternp->num().isOpaque()) {
                    patternp->v3warn(
                        COVERIGN, "Ignoring unsupported coverage cross ignore_bins selector with "
                                  "non-integral wildcard bin value.");
                    return false;
                }
                for (const CrossRange& selectorRange : selectorRanges) {
                    if (wildcardPatternOverlapsRange(patternp, exprp, selectorRange)) {
                        overlaps = true;
                        return true;
                    }
                }
                continue;
            }
            std::vector<CrossRange> binRanges;
            if (!collectCrossRange(itemp, exprp, domainLo, domainHi, binRanges)) return false;
            if (crossRangeSetsOverlap(binp, binRanges, selectorRanges)) {
                overlaps = true;
                return true;
            }
        }
        return true;
    }

    static bool coverBinHasValues(AstCoverBin* binp, AstNodeExpr* exprp) {
        const V3Number domainLo = coverpointDomainBound(binp, exprp, false);
        const V3Number domainHi = coverpointDomainBound(binp, exprp, true);
        const CrossRange domain{domainLo, domainHi};
        for (AstNode* itemp = binp->rangesp(); itemp; itemp = itemp->nextp()) {
            if (AstConst* const constp = VN_CAST(itemp, Const)) {
                if (constp->num().isOpaque()) return true;
                if (binp->isWildcard()) {
                    if (wildcardPatternOverlapsRange(constp, exprp, domain)) return true;
                } else if (constp->num().isFourState() || coverageValueFits(constp, exprp)) {
                    return true;
                }
                continue;
            }
            if (AstInsideRange* const rangep = VN_CAST(itemp, InsideRange)) {
                AstNodeExpr* const lhsp = rangep->lhsp();
                AstNodeExpr* const rhsp = rangep->rhsp();
                AstConst* const loConstp = VN_CAST(lhsp, Const);
                AstConst* const hiConstp = VN_CAST(rhsp, Const);
                const bool loUnbounded = VN_IS(lhsp, Unbounded);
                const bool hiUnbounded = VN_IS(rhsp, Unbounded);
                if ((!loConstp && !loUnbounded) || (!hiConstp && !hiUnbounded)) return true;
                if ((loConstp && (loConstp->num().isOpaque() || loConstp->num().isFourState()))
                    || (hiConstp && (hiConstp->num().isOpaque() || hiConstp->num().isFourState())))
                    return true;
                const V3Number lo
                    = loUnbounded ? domainLo : canonicalCoverageValue(loConstp, exprp);
                const V3Number hi
                    = hiUnbounded ? domainHi : canonicalCoverageValue(hiConstp, exprp);
                if (coverageValueLte(rangep, lo, hi) && coverageValueLte(rangep, lo, domainHi)
                    && coverageValueLte(rangep, domainLo, hi))
                    return true;
                continue;
            }
            return true;
        }
        return false;
    }

    bool prepareCrossSelector(AstCoverCrossBinSel* selp,
                              const std::map<std::string, size_t>& cpIndices,
                              const std::vector<AstCoverpoint*>& coverpointRefs,
                              const std::vector<std::vector<CrossBinInfo>>& allCpBins,
                              const std::vector<std::set<std::string>>& allCpBinNames,
                              CrossIgnorePlan& plan) {
        if (selp->selectType() == VCoverCrossBinSelType::LOG_AND
            || selp->selectType() == VCoverCrossBinSelType::LOG_OR) {
            return prepareCrossSelector(selp->lhsp(), cpIndices, coverpointRefs, allCpBins,
                                        allCpBinNames, plan)
                   && prepareCrossSelector(selp->rhsp(), cpIndices, coverpointRefs, allCpBins,
                                           allCpBinNames, plan);
        }

        UASSERT_OBJ(selp->selectType() == VCoverCrossBinSelType::BINSOF, selp,
                    "Unexpected coverage cross selector operation");
        const auto cpIt = cpIndices.find(selp->coverpointName());
        if (cpIt == cpIndices.end()) {
            selp->v3error("Coverage cross selector references coverpoint '"
                          << selp->coverpointName()
                          << "' that is not part of this cross (IEEE 1800-2023 19.6.1).");
            return false;
        }
        const size_t dimension = cpIt->second;
        AstNodeExpr* const exprp = coverpointRefs[dimension]->exprp();
        std::vector<CrossRange> selectorRanges;
        const bool hasIntersect = selp->rangesp();
        if (hasIntersect && !collectCrossRanges(selp->rangesp(), exprp, selectorRanges))
            return false;

        const bool foundNamedBin
            = selp->binName().empty()
              || allCpBinNames[dimension].find(selp->binName()) != allCpBinNames[dimension].end();
        std::set<const AstCoverBin*> selectedBins;
        for (const CrossBinInfo& bin : allCpBins[dimension]) {
            AstCoverBin* const binp = bin.binp;
            if (!selp->binName().empty() && selp->binName() != bin.sourceName) continue;
            if (!hasIntersect) {
                selectedBins.insert(binp);
                continue;
            }
            if (binp->isWildcard()) {
                bool overlaps = false;
                if (!wildcardBinOverlapsRanges(binp, exprp, selectorRanges, overlaps))
                    return false;
                if (overlaps) selectedBins.insert(binp);
            } else {
                std::vector<CrossRange> binRanges;
                if (!collectCrossRanges(binp->rangesp(), exprp, binRanges)) return false;
                if (crossRangeSetsOverlap(selp, binRanges, selectorRanges))
                    selectedBins.insert(binp);
            }
        }
        if (!foundNamedBin) {
            selp->v3error("Coverage cross selector references unknown bin '"
                          << selp->coverpointName() << "." << selp->binName()
                          << "' (IEEE 1800-2023 19.6.1).");
            return false;
        }
        const bool inserted
            = plan.leaves.emplace(selp, CrossSelectorLeafInfo{dimension, selectedBins}).second;
        UASSERT_OBJ(inserted, selp, "Coverage cross selector leaf prepared twice");
        return true;
    }

    static bool crossSelectorMatches(const AstCoverCrossBinSel* selp,
                                     const std::vector<AstCoverBin*>& combination,
                                     const CrossIgnorePlan& plan) {
        if (selp->selectType() == VCoverCrossBinSelType::LOG_AND) {
            return crossSelectorMatches(selp->lhsp(), combination, plan)
                   && crossSelectorMatches(selp->rhsp(), combination, plan);
        }
        if (selp->selectType() == VCoverCrossBinSelType::LOG_OR) {
            return crossSelectorMatches(selp->lhsp(), combination, plan)
                   || crossSelectorMatches(selp->rhsp(), combination, plan);
        }
        const auto leafIt = plan.leaves.find(selp);
        UASSERT_OBJ(leafIt != plan.leaves.end(), selp,
                    "Coverage cross selector leaf was not prepared");
        const CrossSelectorLeafInfo& leaf = leafIt->second;
        const bool matches = leaf.bins.find(combination[leaf.dimension]) != leaf.bins.end();
        return selp->negated() ? !matches : matches;
    }

    static bool crossCombinationIgnored(const std::vector<AstCoverBin*>& combination,
                                        const CrossIgnorePlan& plan) {
        for (const AstCoverCrossBinSel* const selp : plan.selectors) {
            if (crossSelectorMatches(selp, combination, plan)) return true;
        }
        return false;
    }

    // Recursive helper to generate Cartesian product of cross bins
    void generateCrossBinsRecursive(AstCoverCross* crossp,
                                    const std::vector<AstCoverpoint*>& coverpointRefs,
                                    const std::vector<std::vector<CrossBinInfo>>& allCpBins,
                                    const CrossIgnorePlan& ignorePlan,
                                    std::vector<AstCoverBin*> currentCombination,
                                    size_t dimension) {
        if (dimension == allCpBins.size()) {
            // Base case: we have a complete combination, generate the cross bin
            if (!crossCombinationIgnored(currentCombination, ignorePlan))
                generateOneCrossBin(crossp, coverpointRefs, currentCombination);
            return;
        }

        // Recursive case: iterate through bins at current dimension
        for (const CrossBinInfo& bin : allCpBins[dimension]) {
            currentCombination.push_back(bin.binp);
            generateCrossBinsRecursive(crossp, coverpointRefs, allCpBins, ignorePlan,
                                       currentCombination, dimension + 1);
            currentCombination.pop_back();
        }
    }

    // Generate a single cross bin for a specific combination of bins
    void generateOneCrossBin(AstCoverCross* crossp,
                             const std::vector<AstCoverpoint*>& coverpointRefs,
                             const std::vector<AstCoverBin*>& bins) {
        // Build sanitized name from all bins
        string binName;
        string crossBins;  // Comma-separated individual bin names (one per coverpoint dimension)

        for (size_t i = 0; i < bins.size(); ++i) {
            if (i > 0) {
                binName += "_x_";
                crossBins += ",";
            }
            binName += bins[i]->name();
            crossBins += bins[i]->name();
        }
        const string varName = "__Vcov_" + crossp->name() + "_" + std::to_string(m_crossBinId++);

        // Create member variable for this cross bin
        AstVar* const varp
            = createCoverageCounterVar(crossp->fileline(), varName, bins[0]->findUInt32DType());

        UINFO(4, "      Created cross bin variable: " << varName);

        // Track this for coverage computation
        AstCoverBin* const pseudoBinp = new AstCoverBin{
            crossp->fileline(), binName, static_cast<AstNode*>(nullptr), false, false};
        m_binInfos.push_back(BinInfo(pseudoBinp, varp, 1, nullptr, crossp, crossBins));

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
            if (refp->exprp()) {
                // Non-standard hierarchical/dotted cross item (e.g. 'cross a.b'): an implicit
                // coverpoint over the referenced expression (carried in refp->exprp()).  The
                // grammar already warned NONSTD; implicit coverpoints are not yet implemented, so
                // generate no sampling code for this cross.  When support is added the implicit
                // coverpoint should be synthesized upstream (V3LinkParse) as a real AstCoverpoint
                // so it flows through the normal coverpoint path - by here coverpoint lowering has
                // already run.
                refp->v3warn(COVERIGN,
                             "Unsupported: cross of hierarchical reference (implicit coverpoint)");
                return;
            }
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

        // Collect concrete bins from all coverpoints (excluding ignore/illegal bins).
        std::vector<std::vector<CrossBinInfo>> allCpBins;
        std::vector<std::set<std::string>> allCpBinNames;
        std::vector<AstCoverBin*> ownedCrossBins;
        for (AstCoverpoint* cpp : coverpointRefs) {
            std::vector<CrossBinInfo> cpBins;
            std::set<std::string> cpBinNames;
            for (AstNode* binp = cpp->binsp(); binp; binp = binp->nextp()) {
                AstCoverBin* const cbinp = VN_AS(binp, CoverBin);
                cpBinNames.insert(cbinp->name());
                if (cbinp->binsType() != VCoverBinsType::BINS_USER
                    && cbinp->binsType() != VCoverBinsType::BINS_WILDCARD)
                    continue;
                if (!cbinp->isArray()) {
                    if (!coverBinHasValues(cbinp, cpp->exprp())) continue;
                    cpBins.emplace_back(cbinp, cbinp->name());
                    continue;
                }

                bool unsupported = false;
                std::vector<AstNodeExpr*> values
                    = extractArrayValues(cbinp, cpp->exprp(), unsupported);
                if (unsupported) continue;
                int index = 0;
                for (AstNodeExpr* const valuep : values) {
                    const string binName = cbinp->name() + "[" + std::to_string(index) + "]";
                    AstCoverBin* const concreteBinp = new AstCoverBin{
                        cbinp->fileline(), binName, valuep, false, false, cbinp->isWildcard()};
                    ownedCrossBins.push_back(concreteBinp);
                    cpBins.emplace_back(concreteBinp, cbinp->name());
                    ++index;
                }
            }
            UINFO(4, "      Found " << cpBins.size() << " bins in " << cpp->name());
            allCpBins.push_back(cpBins);
            allCpBinNames.push_back(cpBinNames);
        }

        std::map<std::string, size_t> cpIndices;
        for (size_t dimension = 0; dimension < coverpointRefs.size(); ++dimension) {
            cpIndices.emplace(coverpointRefs[dimension]->name(), dimension);
        }
        CrossIgnorePlan ignorePlan;
        for (AstNode* binp = crossp->binsp(); binp; binp = binp->nextp()) {
            AstCoverCrossBin* const ignorep = VN_AS(binp, CoverCrossBin);
            UASSERT_OBJ(ignorep->binsType() == VCoverBinsType::BINS_IGNORE, ignorep,
                        "Only ignore_bins are supported in coverage crosses");
            UASSERT_OBJ(!ignorep->iffp(), ignorep,
                        "Coverage cross ignore_bins iff should have been rejected by parser");
            AstCoverCrossBinSel* const selp = ignorep->selectp();
            if (prepareCrossSelector(selp, cpIndices, coverpointRefs, allCpBins, allCpBinNames,
                                     ignorePlan))
                ignorePlan.selectors.push_back(selp);
        }

        // Generate cross bins using Cartesian product
        generateCrossBinsRecursive(crossp, coverpointRefs, allCpBins, ignorePlan, {}, 0);
        for (AstCoverBin* const binp : ownedCrossBins) VL_DO_DANGLING(pushDeletep(binp), binp);
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
                } else if (coverageValuesEqual(irp, canonicalCoverageValue(minConstp, exprp),
                                               canonicalCoverageValue(maxConstp, exprp))) {
                    // Single value
                    if (isWildcard) {
                        rangeCondp = buildWildcardCondition(binp, exprp, minConstp);
                    } else {
                        rangeCondp = makeValueCondition(binp->fileline(), exprp, minConstp);
                    }
                } else {
                    rangeCondp = makeRangeCondition(irp->fileline(), exprp, minExprp, maxExprp);
                }
            } else if (AstConst* constp = VN_CAST(currRangep, Const)) {
                if (isWildcard) {
                    rangeCondp = buildWildcardCondition(binp, exprp, constp);
                } else {
                    rangeCondp = makeValueCondition(binp->fileline(), exprp, constp);
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
        const bool autoExtend = coverageAutoExtend(constp);
        const int patternWidth = autoExtend ? exprp->width() : constp->width();
        const bool patternSigned = autoExtend ? exprp->isSigned() : constp->isSigned();
        V3Number pattern{constp, patternWidth, 0};
        if (autoExtend || constp->isSigned()) {
            pattern.opExtendS(constp->num(), constp->width());
        } else {
            pattern.opAssign(constp->num());
        }

        // Extract mask from constant (bits that are not X/Z)
        V3Number mask{constp, patternWidth};
        V3Number value{constp, patternWidth};

        for (int bit = 0; bit < patternWidth; ++bit) {
            if (pattern.bitIs0(bit) || pattern.bitIs1(bit)) {
                mask.setBit(bit, 1);
                value.setBit(bit, pattern.bitIs1(bit) ? 1 : 0);
            } else {
                mask.setBit(bit, 0);
                value.setBit(bit, 0);
            }
        }

        // Generate: (expr & mask) == (value & mask)
        AstConst* const maskConstp = new AstConst{fl, mask};
        AstConst* const valueConstp = new AstConst{fl, value};

        AstNodeExpr* resizedExprp = exprp->cloneTree(false);
        if (resizedExprp->width() < patternWidth) {
            resizedExprp
                = exprp->isSigned()
                      ? static_cast<AstNodeExpr*>(new AstExtendS{fl, resizedExprp, patternWidth})
                      : static_cast<AstNodeExpr*>(new AstExtend{fl, resizedExprp, patternWidth});
        } else if (resizedExprp->width() > patternWidth) {
            resizedExprp = new AstSel{fl, resizedExprp, 0, patternWidth};
        }
        AstNodeExpr* const exprMasked = new AstAnd{fl, resizedExprp, maskConstp};
        AstNodeExpr* const valueMasked = new AstAnd{fl, valueConstp, maskConstp->cloneTree(false)};

        AstNodeExpr* const patternMatchp = new AstEq{fl, exprMasked, valueMasked};
        const V3Number patternDomainLo
            = coverageDomainBound(constp, patternWidth, patternSigned, false);
        const V3Number patternDomainHi
            = coverageDomainBound(constp, patternWidth, patternSigned, true);
        AstNodeExpr* const domainMatchp
            = makeCanonicalRangeCondition(fl, exprp, patternDomainLo, patternDomainHi);
        return new AstAnd{fl, domainMatchp, patternMatchp};
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
            retp->add(" = (__Vtot != 0.0) ? (100.0 * __Vcov / __Vtot) : 100.0;");
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
            // No coverage to compute - return 100%.
            // Any parser-generated initialization of returnVar is overridden by our assignment.
            UINFO(4, "    Empty covergroup, returning 100.0");
            funcp->addStmtsp(new AstAssign{fl, new AstVarRef{fl, returnVarp, VAccess::WRITE},
                                           new AstConst{fl, AstConst::RealDouble{}, 100.0}});
            UINFO(4, "    Added assignment to return 100.0");
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
            const std::string binName = binp->name();

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
    static bool isEnclosingInstanceVar(const AstVar* varp) {
        return varp->isClassMember() && !varp->lifetime().isStatic() && !varp->isParam();
    }

    void rewriteThisRef(AstThisRef* refp, AstVar* handleVarp) {
        const AstClassRefDType* const refDTypep
            = VN_CAST(refp->dtypep()->skipRefp(), ClassRefDType);
        UASSERT_OBJ(refDTypep && refDTypep->classp() == m_covergroupp, refp,
                    "Unexpected this reference in embedded covergroup");
        AstNodeExpr* const newp = new AstVarRef{refp->fileline(), handleVarp, VAccess::READ};
        refp->replaceWith(newp);
        VL_DO_DANGLING(pushDeletep(refp), refp);
    }

    void rewriteVarRef(AstVarRef* refp, AstVar* handleVarp) {
        FileLine* const fl = refp->fileline();
        AstMemberSel* const selp
            = new AstMemberSel{fl, new AstVarRef{fl, handleVarp, VAccess::READ}, refp->varp()};
        selp->access(refp->access());
        refp->replaceWith(selp);
        VL_DO_DANGLING(pushDeletep(refp), refp);
    }

    void deleteCoverageItems() {
        for (AstCoverpoint* const cpp : m_coverpoints) {
            VL_DO_DANGLING(pushDeletep(cpp->unlinkFrBack()), cpp);
        }
        for (AstCoverCross* const crossp : m_coverCrosses) {
            VL_DO_DANGLING(pushDeletep(crossp->unlinkFrBack()), crossp);
        }
    }

    class FormalRefVisitor final : public VNVisitor {
        const std::map<const AstVar*, AstVar*>& m_replacements;

        void visit(AstVarRef* nodep) override {
            const auto it = m_replacements.find(nodep->varp());
            if (it != m_replacements.end()) nodep->varp(it->second);
        }
        void visit(AstNode* nodep) override { iterateChildren(nodep); }

    public:
        explicit FormalRefVisitor(const std::map<const AstVar*, AstVar*>& replacements)
            : m_replacements{replacements} {}
        void scan(AstNode* nodep) { iterate(nodep); }
    };

    void rebindConstructorFormalRefs() {
        std::map<const AstVar*, AstVar*> replacements;
        for (AstNode* stmtp = m_constructorp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (const AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (!varp->isIO()) continue;
                AstVar* const memberp
                    = VN_CAST(m_memberMap.findMember(m_covergroupp, varp->name()), Var);
                UASSERT_OBJ(memberp && memberp->isClassMember(), varp,
                            "Covergroup constructor argument missing persistent member");
                replacements.emplace(varp, memberp);
            }
        }
        FormalRefVisitor visitor{replacements};
        for (AstCoverpoint* const cpp : m_coverpoints) visitor.scan(cpp);
        for (AstCoverCross* const crossp : m_coverCrosses) visitor.scan(crossp);
    }

    AstVarRef* installEnclosingBackPointer() {
        // Simple-case support for embedded covergroups (IEEE 1800-2023 19.4) whose
        // coverpoints reference members of the enclosing class ("Class members can be used
        // in coverpoint expressions").  The covergroup is lowered into a sibling class with
        // no implicit handle to the enclosing object, so such references would emit
        // uncompilable C++.  Add an explicit back-pointer member to the enclosing instance,
        // route the member references through it, and initialize it right after the
        // 'cgvar = new' construction.  The enclosing member values are only read in
        // sample(), which runs after construction, so this ordering is safe.  Returns an invalid
        // reference if an outer class member cannot be reached; otherwise returns an empty result.
        if (!m_enclosingClassp) return nullptr;  // Offending refs require an enclosing class

        AstVarRef* invalidp = nullptr;
        AstNode* offenderp = nullptr;
        std::set<const AstVar*> ownVars;
        for (AstNode* itemp = m_covergroupp->membersp(); itemp; itemp = itemp->nextp()) {
            if (const AstVar* const varp = VN_CAST(itemp, Var)) ownVars.insert(varp);
        }
        std::set<const AstVar*> enclosingVars;
        m_enclosingClassp->foreachMember([&](AstClass* const, AstVar* const varp) {
            if (isEnclosingInstanceVar(varp)) enclosingVars.insert(varp);
        });

        std::vector<AstVarRef*> refsToRewrite;
        std::vector<AstThisRef*> thisRefsToRewrite;
        const auto scan = [&](AstNode* rootp) {
            rootp->foreach([&](AstVarRef* refp) {
                if (invalidp) return;
                const AstVar* const varp = refp->varp();
                if (!isEnclosingInstanceVar(varp) || ownVars.count(varp)) return;
                if (!enclosingVars.count(varp)) {
                    invalidp = refp;
                    return;
                }
                refsToRewrite.push_back(refp);
                if (!offenderp) offenderp = refp;
            });
            if (invalidp) return;
            rootp->foreach([&](AstThisRef* refp) {
                const AstClassRefDType* const refDTypep
                    = VN_CAST(refp->dtypep()->skipRefp(), ClassRefDType);
                if (refDTypep && refDTypep->classp() == m_covergroupp) {
                    thisRefsToRewrite.push_back(refp);
                    if (!offenderp) offenderp = refp;
                }
            });
        };
        for (AstCoverpoint* const cpp : m_coverpoints) scan(cpp);
        for (AstCoverCross* const crossp : m_coverCrosses) scan(crossp);
        if (invalidp || !offenderp) return invalidp;

        AstVar* embeddedVarp = nullptr;
        for (AstNode* itemp = m_enclosingClassp->membersp(); itemp; itemp = itemp->nextp()) {
            AstVar* const varp = VN_CAST(itemp, Var);
            if (!varp) continue;
            const AstClassRefDType* const refp
                = VN_CAST(varp->dtypep()->skipRefp(), ClassRefDType);
            if (refp && refp->classp() == m_covergroupp) {
                embeddedVarp = varp;
                break;
            }
        }
        UASSERT_OBJ(embeddedVarp, m_covergroupp, "Embedded covergroup variable not found");

        std::vector<AstNodeAssign*> constructps;
        AstFunc* const enclosingNewp
            = VN_CAST(m_memberMap.findMember(m_enclosingClassp, "new"), Func);
        if (enclosingNewp) {
            enclosingNewp->foreach([&](AstNodeAssign* asgnp) {
                const AstNew* const newp = VN_CAST(asgnp->rhsp(), New);
                const AstVarRef* const lhsRefp = VN_CAST(asgnp->lhsp(), VarRef);
                if (newp && lhsRefp && lhsRefp->varp() == embeddedVarp) {
                    const AstClassRefDType* const refp = VN_CAST(newp->dtypep(), ClassRefDType);
                    if (refp && refp->classp() == m_covergroupp) constructps.push_back(asgnp);
                }
            });
        }
        // Commit: add the back-pointer member, rewrite the references, initialize the handle.
        FileLine* const fl = m_covergroupp->fileline();
        AstClassRefDType* const enclDTypep = new AstClassRefDType{fl, m_enclosingClassp, nullptr};
        enclDTypep->rawPointer(true);
        v3Global.rootp()->typeTablep()->addTypesp(enclDTypep);
        AstVar* const handleVarp
            = new AstVar{fl, VVarType::MEMBER, "__Vcg_enclosingp", enclDTypep};
        handleVarp->isStatic(false);
        m_covergroupp->addMembersp(handleVarp);

        // Route each enclosing-member reference through the back-pointer: 'm' -> 'h.m'.
        for (AstVarRef* const refp : refsToRewrite) { rewriteVarRef(refp, handleVarp); }
        for (AstThisRef* const refp : thisRefsToRewrite) { rewriteThisRef(refp, handleVarp); }

        // Initialize the raw back-pointer after each construction.  With no construction site,
        // the embedded covergroup handle remains null, so no back-pointer is observed.
        for (AstNodeAssign* const constructp : constructps) {
            FileLine* const cfl = constructp->fileline();
            AstMemberSel* const lhsp
                = new AstMemberSel{cfl, constructp->lhsp()->cloneTree(false), handleVarp};
            lhsp->access(VAccess::WRITE);
            AstCExpr* const thisp = new AstCExpr{cfl, "this"};
            thisp->dtypep(enclDTypep);
            constructp->addNextHere(new AstAssign{cfl, lhsp, thisp});
        }
        return nullptr;
    }

    void visit(AstClass* nodep) override {
        UINFO(9, "Visiting class: " << nodep->name() << " isCovergroup=" << nodep->isCovergroup());
        if (nodep->isCovergroup()) {
            VL_RESTORER(m_covergroupp);
            VL_RESTORER(m_sampleFuncp);
            VL_RESTORER(m_constructorp);
            VL_RESTORER_CLEAR(m_coverpoints);
            VL_RESTORER_CLEAR(m_coverpointMap);
            VL_RESTORER_CLEAR(m_coverCrosses);
            m_covergroupp = nodep;
            m_sampleFuncp = nullptr;
            m_constructorp = nullptr;

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
                deleteCoverageItems();
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
            rebindConstructorFormalRefs();

            // Embedded covergroups (IEEE 1800-2023 19.4): coverpoints, iff expressions, and
            // crosses may reference members of the enclosing class. The covergroup is lowered
            // into a sibling class with no implicit handle to the enclosing instance. Install
            // an explicit back-pointer and route the references through it.
            if (AstVarRef* const invalidp = installEnclosingBackPointer()) {
                invalidp->v3error("Non-static member "
                                  << invalidp->varp()->prettyNameQ()
                                  << " of an outer class requires an explicit "
                                     "object handle (IEEE 1800-2023 8.23).");
                deleteCoverageItems();
                return;
            }
            processCovergroup();
            // Remove lowered coverpoints/crosses from the class - they have been
            // fully translated into C++ code and must not reach downstream passes
            deleteCoverageItems();
        } else {
            // Track the lexically enclosing class so a nested covergroup can resolve
            // references to the enclosing object's members (installEnclosingBackPointer).
            VL_RESTORER(m_enclosingClassp);
            m_enclosingClassp = nodep;
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
