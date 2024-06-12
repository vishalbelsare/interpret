// Copyright (c) 2023 The InterpretML Contributors
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "pch.hpp"

#include <stdlib.h> // free
#include <stddef.h> // size_t, ptrdiff_t
#include <limits> // numeric_limits
#include <string.h> // memcpy

#include "libebm.h" // EBM_API_BODY
#include "logging.h" // EBM_ASSERT

#define ZONE_main
#include "zones.h"

#include "bridge.h"
#include "GradientPair.hpp"
#include "Bin.hpp"

#include "ebm_internal.hpp"
#include "RandomDeterministic.hpp"
#include "RandomNondeterministic.hpp"
#include "ebm_stats.hpp"
#include "Feature.hpp"
#include "Term.hpp"
#include "InnerBag.hpp"
#include "Tensor.hpp"
#include "BoosterCore.hpp"
#include "BoosterShell.hpp"

namespace DEFINED_ZONE_NAME {
#ifndef DEFINED_ZONE_NAME
#error DEFINED_ZONE_NAME must be defined
#endif // DEFINED_ZONE_NAME

extern void ConvertAddBin(const size_t cScores,
      const bool bHessian,
      const size_t cBins,
      const bool bUInt64Src,
      const bool bDoubleSrc,
      const bool bCountSrc,
      const bool bWeightSrc,
      const void* const aSrc,
      const UIntMain* const aCounts,
      const FloatPrecomp* const aWeights,
      const bool bUInt64Dest,
      const bool bDoubleDest,
      void* const aAddDest);

extern void TensorTotalsBuild(const bool bHessian,
      const size_t cScores,
      const size_t cRealDimensions,
      const size_t* const acBins,
      BinBase* aAuxiliaryBinsBase,
      BinBase* const aBinsBase
#ifndef NDEBUG
      ,
      BinBase* const aDebugCopyBinsBase,
      const BinBase* const pBinsEndDebug
#endif // NDEBUG
);

extern ErrorEbm PartitionOneDimensionalBoosting(RandomDeterministic* const pRng,
      BoosterShell* const pBoosterShell,
      const TermBoostFlags flags,
      const size_t cBins,
      const size_t iDimension,
      const size_t cSamplesLeafMin,
      const double hessianMin,
      const size_t cSplitsMax,
      const MonotoneDirection direction,
      const size_t cSamplesTotal,
      const FloatMain weightTotal,
      double* const pTotalGain);

extern ErrorEbm PartitionTwoDimensionalBoosting(BoosterShell* const pBoosterShell,
      const TermBoostFlags flags,
      const Term* const pTerm,
      const size_t* const acBins,
      const size_t cSamplesLeafMin,
      const double hessianMin,
      BinBase* aAuxiliaryBinsBase,
      double* const aWeights,
      double* const pTotalGain
#ifndef NDEBUG
      ,
      const BinBase* const aDebugCopyBinsBase
#endif // NDEBUG
);

extern ErrorEbm PartitionRandomBoosting(RandomDeterministic* const pRng,
      BoosterShell* const pBoosterShell,
      const Term* const pTerm,
      const TermBoostFlags flags,
      const IntEbm* const aLeavesMax,
      const MonotoneDirection significantDirection,
      double* const pTotalGain);

static void BoostZeroDimensional(BoosterShell* const pBoosterShell, const TermBoostFlags flags) {
   LOG_0(Trace_Verbose, "Entered BoostZeroDimensional");

   BoosterCore* const pBoosterCore = pBoosterShell->GetBoosterCore();
   const size_t cScores = pBoosterCore->GetCountScores();

   BinBase* const pMainBin = pBoosterShell->GetBoostingMainBins();
   EBM_ASSERT(nullptr != pMainBin);

   Tensor* const pInnerTermUpdate = pBoosterShell->GetInnerTermUpdate();
   FloatScore* aUpdateScores = pInnerTermUpdate->GetTensorScoresPointer();
   if(pBoosterCore->IsHessian()) {
      const auto* const pBin = pMainBin->Specialize<FloatMain, UIntMain, true, true, true>();
      const auto* const aGradientPairs = pBin->GetGradientPairs();
      if(TermBoostFlags_GradientSums & flags) {
         for(size_t iScore = 0; iScore < cScores; ++iScore) {
            const FloatCalc updateScore = ComputeSinglePartitionUpdateGradientSum(
                  static_cast<FloatCalc>(aGradientPairs[iScore].m_sumGradients));
            aUpdateScores[iScore] = static_cast<FloatScore>(updateScore);
         }
      } else {
         const FloatCalc weight = static_cast<FloatCalc>(pBin->GetWeight());
         for(size_t iScore = 0; iScore < cScores; ++iScore) {
            const FloatCalc updateScore =
                  ComputeSinglePartitionUpdate(static_cast<FloatCalc>(aGradientPairs[iScore].m_sumGradients),
                        TermBoostFlags_DisableNewtonUpdate & flags ?
                              weight :
                              static_cast<FloatCalc>(aGradientPairs[iScore].GetHess()));
            aUpdateScores[iScore] = static_cast<FloatScore>(updateScore);
         }
      }
   } else {
      const auto* const pBin = pMainBin->Specialize<FloatMain, UIntMain, true, true, false>();
      const auto* const aGradientPairs = pBin->GetGradientPairs();
      if(TermBoostFlags_GradientSums & flags) {
         for(size_t iScore = 0; iScore < cScores; ++iScore) {
            const FloatCalc updateScore = ComputeSinglePartitionUpdateGradientSum(
                  static_cast<FloatCalc>(aGradientPairs[iScore].m_sumGradients));
            aUpdateScores[iScore] = static_cast<FloatScore>(updateScore);
         }
      } else {
         const FloatCalc weight = static_cast<FloatCalc>(pBin->GetWeight());
         for(size_t iScore = 0; iScore < cScores; ++iScore) {
            const FloatCalc updateScore =
                  ComputeSinglePartitionUpdate(static_cast<FloatCalc>(aGradientPairs[iScore].m_sumGradients), weight);
            aUpdateScores[iScore] = static_cast<FloatScore>(updateScore);
         }
      }
   }

   LOG_0(Trace_Verbose, "Exited BoostZeroDimensional");
}

static ErrorEbm BoostSingleDimensional(RandomDeterministic* const pRng,
      BoosterShell* const pBoosterShell,
      const TermBoostFlags flags,
      const size_t cBins,
      const FloatMain weightTotal,
      const size_t iDimension,
      const size_t cSamplesLeafMin,
      const double hessianMin,
      const IntEbm countLeavesMax,
      const MonotoneDirection direction,
      double* const pTotalGain) {
   ErrorEbm error;

   LOG_0(Trace_Verbose, "Entered BoostSingleDimensional");

   EBM_ASSERT(IntEbm{2} <= countLeavesMax); // otherwise we would have called BoostZeroDimensional
   size_t cSplitsMax = static_cast<size_t>(countLeavesMax) - size_t{1};
   if(IsConvertError<size_t>(countLeavesMax)) {
      // we can never exceed a size_t number of leaves, so let's just set it to the maximum if we were going to overflow
      // because it will generate the same results as if we used the true number
      cSplitsMax = std::numeric_limits<size_t>::max();
   }

   BoosterCore* const pBoosterCore = pBoosterShell->GetBoosterCore();

   EBM_ASSERT(1 <= pBoosterCore->GetTrainingSet()->GetCountSamples());

   error = PartitionOneDimensionalBoosting(pRng,
         pBoosterShell,
         flags,
         cBins,
         iDimension,
         cSamplesLeafMin,
         hessianMin,
         cSplitsMax,
         direction,
         pBoosterCore->GetTrainingSet()->GetCountSamples(),
         weightTotal,
         pTotalGain);

   LOG_0(Trace_Verbose, "Exited BoostSingleDimensional");
   return error;
}

// TODO: for higher dimensional spaces, we need to add/subtract individual cells alot and the hessian isn't required
// (yet) in order to make decisions about
//   where to split.  For dimensions higher than 2, we might want to copy the tensor to a new tensor AFTER binning that
//   keeps only the gradients and then
//    go back to our original tensor after splits to determine the hessian
static ErrorEbm BoostMultiDimensional(BoosterShell* const pBoosterShell,
      const TermBoostFlags flags,
      const size_t iTerm,
      const size_t cSamplesLeafMin,
      const double hessianMin,
      double* const pTotalGain) {
   LOG_0(Trace_Verbose, "Entered BoostMultiDimensional");

   BoosterCore* const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(iTerm < pBoosterCore->GetCountTerms());
   const Term* const pTerm = pBoosterCore->GetTerms()[iTerm];

   EBM_ASSERT(2 <= pTerm->GetCountDimensions());
   EBM_ASSERT(2 <= pTerm->GetCountRealDimensions());

   ErrorEbm error;

   const size_t cTensorBins = pTerm->GetCountTensorBins();
   EBM_ASSERT(1 <= cTensorBins);

   size_t acBins[k_cDimensionsMax];
   size_t* pcBins = acBins;

   const TermFeature* pTermFeature = pTerm->GetTermFeatures();
   const TermFeature* const pTermFeaturesEnd = &pTermFeature[pTerm->GetCountDimensions()];
   do {
      const FeatureBoosting* pFeature = pTermFeature->m_pFeature;
      const size_t cBins = pFeature->GetCountBins();
      EBM_ASSERT(size_t{1} <= cBins); // we don't boost on empty training sets
      if(size_t{1} < cBins) {
         *pcBins = cBins;
         ++pcBins;
      }
      ++pTermFeature;
   } while(pTermFeaturesEnd != pTermFeature);

   const size_t cScores = pBoosterCore->GetCountScores();

   const size_t cAuxillaryBins = pTerm->GetCountAuxillaryBins();

   const size_t cBytesPerMainBin = GetBinSize<FloatMain, UIntMain>(true, true, pBoosterCore->IsHessian(), cScores);

   // we don't need to free this!  It's tracked and reused by pBoosterShell
   BinBase* const aMainBins = pBoosterShell->GetBoostingMainBins();
   EBM_ASSERT(nullptr != aMainBins);

   // we also need to zero the auxillary bins
   aMainBins->ZeroMem(cBytesPerMainBin, cAuxillaryBins, cTensorBins);

#ifndef NDEBUG
   // make a copy of the original bins for debugging purposes

   BinBase* aDebugCopyBins = nullptr;
   if(!IsMultiplyError(cBytesPerMainBin, cTensorBins)) {
      ANALYSIS_ASSERT(0 != cBytesPerMainBin);
      aDebugCopyBins = static_cast<BinBase*>(malloc(cBytesPerMainBin * cTensorBins));
      if(nullptr != aDebugCopyBins) {
         // if we can't allocate, don't fail.. just stop checking
         memcpy(aDebugCopyBins, aMainBins, cBytesPerMainBin * cTensorBins);
      }
   }
#endif // NDEBUG

   BinBase* aAuxiliaryBins = IndexBin(aMainBins, cBytesPerMainBin * cTensorBins);

   TensorTotalsBuild(pBoosterCore->IsHessian(),
         cScores,
         pTerm->GetCountRealDimensions(),
         acBins,
         aAuxiliaryBins,
         aMainBins
#ifndef NDEBUG
         ,
         aDebugCopyBins,
         pBoosterShell->GetDebugMainBinsEnd()
#endif // NDEBUG
   );

   // permutation0
   // gain_permute0
   //   divs0
   //   gain0
   //     divs00
   //     gain00
   //       divs000
   //       gain000
   //       divs001
   //       gain001
   //     divs01
   //     gain01
   //       divs010
   //       gain010
   //       divs011
   //       gain011
   //   divs1
   //   gain1
   //     divs10
   //     gain10
   //       divs100
   //       gain100
   //       divs101
   //       gain101
   //     divs11
   //     gain11
   //       divs110
   //       gain110
   //       divs111
   //       gain111
   //---------------------------
   // permutation1
   // gain_permute1
   //   divs0
   //   gain0
   //     divs00
   //     gain00
   //       divs000
   //       gain000
   //       divs001
   //       gain001
   //     divs01
   //     gain01
   //       divs010
   //       gain010
   //       divs011
   //       gain011
   //   divs1
   //   gain1
   //     divs10
   //     gain10
   //       divs100
   //       gain100
   //       divs101
   //       gain101
   //     divs11
   //     gain11
   //       divs110
   //       gain110
   //       divs111
   //       gain111       *

   // size_t aiDimensionPermutation[k_cDimensionsMax];
   // for(unsigned int iDimensionInitialize = 0; iDimensionInitialize < cDimensions; ++iDimensionInitialize) {
   //    aiDimensionPermutation[iDimensionInitialize] = iDimensionInitialize;
   // }
   // size_t aiDimensionPermutationBest[k_cDimensionsMax];

   // DO this is a fixed length that we should make variable!
   // size_t aDOSplits[1000000];
   // size_t aDOSplitsBest[1000000];

   // do {
   //    size_t aiDimensions[k_cDimensionsMax];
   //    memset(aiDimensions, 0, sizeof(aiDimensions[0]) * cDimensions));
   //    while(true) {

   //      EBM_ASSERT(0 == iDimension);
   //      while(true) {
   //         ++aiDimension[iDimension];
   //         if(aiDimension[iDimension] !=
   //               pTerms->GetFeatures()[aiDimensionPermutation[iDimension]].m_pFeature->m_cBins) {
   //            break;
   //         }
   //         aiDimension[iDimension] = 0;
   //         ++iDimension;
   //         if(iDimension == cDimensions) {
   //            goto move_next_permutation;
   //         }
   //      }
   //   }
   //   move_next_permutation:
   //} while(std::next_permutation(aiDimensionPermutation, &aiDimensionPermutation[cDimensions]));

   double* aWeights = nullptr;

   if(2 == pTerm->GetCountRealDimensions()) {
      error = PartitionTwoDimensionalBoosting(pBoosterShell,
            flags,
            pTerm,
            acBins,
            cSamplesLeafMin,
            hessianMin,
            aAuxiliaryBins,
            aWeights,
            pTotalGain
#ifndef NDEBUG
            ,
            aDebugCopyBins
#endif // NDEBUG
      );
      if(Error_None != error) {
#ifndef NDEBUG
         free(aDebugCopyBins);
#endif // NDEBUG

         LOG_0(Trace_Verbose, "Exited BoostMultiDimensional with Error code");

         return error;
      }

      EBM_ASSERT(!std::isnan(*pTotalGain));
      EBM_ASSERT(0 <= *pTotalGain);
   } else {
      LOG_0(Trace_Warning, "WARNING BoostMultiDimensional 2 != pTerm->GetCountSignificantFeatures()");

      // TODO: eventually handle this in our caller and this function can specialize in handling just 2 dimensional
      //       then we can replace this branch with an assert
#ifndef NDEBUG
      EBM_ASSERT(false);
      free(aDebugCopyBins);
#endif // NDEBUG
      return Error_UnexpectedInternal;
   }

#ifndef NDEBUG
   free(aDebugCopyBins);
#endif // NDEBUG

   LOG_0(Trace_Verbose, "Exited BoostMultiDimensional");
   return Error_None;
}

static ErrorEbm BoostRandom(RandomDeterministic* const pRng,
      BoosterShell* const pBoosterShell,
      const size_t iTerm,
      const TermBoostFlags flags,
      const IntEbm* const aLeavesMax,
      const MonotoneDirection significantDirection,
      double* const pTotalGain) {
   // THIS RANDOM SPLIT FUNCTION IS PRIMARILY USED FOR DIFFERENTIAL PRIVACY EBMs

   LOG_0(Trace_Verbose, "Entered BoostRandom");

   ErrorEbm error;

   BoosterCore* const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(iTerm < pBoosterCore->GetCountTerms());
   const Term* const pTerm = pBoosterCore->GetTerms()[iTerm];

   error = PartitionRandomBoosting(pRng, pBoosterShell, pTerm, flags, aLeavesMax, significantDirection, pTotalGain);
   if(Error_None != error) {
      LOG_0(Trace_Verbose, "Exited BoostRandom with Error code");
      return error;
   }

   EBM_ASSERT(!std::isnan(*pTotalGain));
   EBM_ASSERT(0 <= *pTotalGain);

   LOG_0(Trace_Verbose, "Exited BoostRandom");
   return Error_None;
}

// we made this a global because if we had put this variable inside the BoosterCore object, then we would need to
// dereference that before getting the count.  By making this global we can send a log message incase a bad BoosterCore
// object is sent into us we only decrease the count if the count is non-zero, so at worst if there is a race condition
// then we'll output this log message more times than desired, but we can live with that
static int g_cLogGenerateTermUpdate = 10;

EBM_API_BODY ErrorEbm EBM_CALLING_CONVENTION GenerateTermUpdate(void* rng,
      BoosterHandle boosterHandle,
      IntEbm indexTerm,
      TermBoostFlags flags,
      double learningRate,
      IntEbm minSamplesLeaf,
      double minHessian,
      const IntEbm* leavesMax,
      const MonotoneDirection* direction,
      double* avgGainOut) {
   ErrorEbm error;

   LOG_COUNTED_N(&g_cLogGenerateTermUpdate,
         Trace_Info,
         Trace_Verbose,
         "GenerateTermUpdate: "
         "rng=%p, "
         "boosterHandle=%p, "
         "indexTerm=%" IntEbmPrintf ", "
         "flags=0x%" UTermBoostFlagsPrintf ", "
         "learningRate=%le, "
         "minSamplesLeaf=%" IntEbmPrintf ", "
         "minHessian=%le, "
         "leavesMax=%p, "
         "direction=%p, "
         "avgGainOut=%p",
         rng,
         static_cast<void*>(boosterHandle),
         indexTerm,
         static_cast<UTermBoostFlags>(flags), // signed to unsigned conversion is defined behavior in C++
         learningRate,
         minSamplesLeaf,
         minHessian,
         static_cast<const void*>(leavesMax),
         static_cast<const void*>(direction),
         static_cast<void*>(avgGainOut));

   if(LIKELY(nullptr != avgGainOut)) {
      *avgGainOut = k_illegalGainDouble;
   }

   BoosterShell* const pBoosterShell = BoosterShell::GetBoosterShellFromHandle(boosterHandle);
   if(nullptr == pBoosterShell) {
      // already logged
      return Error_IllegalParamVal;
   }

   // set this to illegal so if we exit with an error we have an invalid index
   pBoosterShell->SetTermIndex(BoosterShell::k_illegalTermIndex);

   if(indexTerm < 0) {
      LOG_0(Trace_Error, "ERROR GenerateTermUpdate indexTerm must be positive");
      return Error_IllegalParamVal;
   }

   BoosterCore* const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(nullptr != pBoosterCore);

   if(static_cast<IntEbm>(pBoosterCore->GetCountTerms()) <= indexTerm) {
      LOG_0(Trace_Error, "ERROR GenerateTermUpdate indexTerm above the number of terms that we have");
      return Error_IllegalParamVal;
   }
   size_t iTerm = static_cast<size_t>(indexTerm);

   // this is true because 0 < pBoosterCore->m_cTerms since our caller needs to pass in a valid indexTerm to this
   // function
   EBM_ASSERT(nullptr != pBoosterCore->GetTerms());
   Term* const pTerm = pBoosterCore->GetTerms()[iTerm];

   LOG_COUNTED_0(pTerm->GetPointerCountLogEnterGenerateTermUpdateMessages(),
         Trace_Info,
         Trace_Verbose,
         "Entered GenerateTermUpdate");

   if(flags &
         ~(TermBoostFlags_DisableNewtonGain | TermBoostFlags_DisableNewtonUpdate | TermBoostFlags_GradientSums |
               TermBoostFlags_RandomSplits)) {
      LOG_0(Trace_Error, "ERROR GenerateTermUpdate flags contains unknown flags. Ignoring extras.");
   }

   if(std::isnan(learningRate)) {
      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate learningRate is NaN");
   } else if(std::numeric_limits<double>::infinity() == learningRate) {
      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate learningRate is +infinity");
   } else if(0.0 == learningRate) {
      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate learningRate is zero");
   } else if(learningRate < double{0}) {
      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate learningRate is negative");
   }

   size_t cSamplesLeafMin = size_t{0}; // this is the min value
   if(IntEbm{0} <= minSamplesLeaf) {
      cSamplesLeafMin = static_cast<size_t>(minSamplesLeaf);
      if(IsConvertError<size_t>(minSamplesLeaf)) {
         // we can never exceed a size_t number of samples, so let's just set it to the maximum if we were going to
         // overflow because it will generate the same results as if we used the true number
         cSamplesLeafMin = std::numeric_limits<size_t>::max();
      }
   } else {
      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate minSamplesLeaf can't be less than 0.  Adjusting to 0.");
   }

   if(std::isnan(minHessian) || minHessian <= 0.0) {
      minHessian = std::numeric_limits<double>::min();
      LOG_0(Trace_Warning,
            "WARNING GenerateTermUpdate minHessian must be a positive number. Adjusting to minimum float");
   }

   const size_t cScores = pBoosterCore->GetCountScores();
   if(size_t{0} == cScores) {
      // if there is only 1 target class for classification, then we can predict the output with 100% accuracy.
      // The term scores are a tensor with zero length array logits, which means for our representation that we have
      // zero items in the array total. Since we can predit the output with 100% accuracy, our gain will be 0.
      if(LIKELY(nullptr != avgGainOut)) {
         *avgGainOut = 0.0;
      }
      pBoosterShell->SetTermIndex(iTerm);

      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate size_t { 0 } == cScores");
      return Error_None;
   }
   EBM_ASSERT(nullptr != pBoosterShell->GetTermUpdate());
   EBM_ASSERT(nullptr != pBoosterShell->GetInnerTermUpdate());

   size_t cTensorBins = pTerm->GetCountTensorBins();
   if(size_t{0} == cTensorBins) {
      // there are zero samples and 0 bins in one of the features in the dimensions, so the update tensor has 0 bins

      // if GetCountTensorBins is 0, then we leave pBoosterShell->GetTermUpdate() with invalid data since
      // out Tensor class does not support tensors of zero elements

      if(LIKELY(nullptr != avgGainOut)) {
         *avgGainOut = 0.0;
      }
      pBoosterShell->SetTermIndex(iTerm);

      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate size_t { 0 } == cTensorBins");
      return Error_None;
   }

   const size_t cInnerBagsAfterZero =
         size_t{0} == pBoosterCore->GetCountInnerBags() ? size_t{1} : pBoosterCore->GetCountInnerBags();
   const size_t cRealDimensions = pTerm->GetCountRealDimensions();
   const size_t cDimensions = pTerm->GetCountDimensions();

   // TODO: we can probably eliminate lastDimensionLeavesMax and cSignificantBinCount and just fetch them from
   // iDimensionImportant afterwards
   IntEbm lastDimensionLeavesMax = IntEbm{0};
   // this initialization isn't required, but this variable ends up touching a lot of downstream state
   // and g++ seems to warn about all of that usage, even in other downstream functions!
   size_t cSignificantBinCount = size_t{0};
   MonotoneDirection significantDirection = MONOTONE_NONE;
   size_t iDimensionImportant = 0;
   if(nullptr == leavesMax) {
      LOG_0(Trace_Warning, "WARNING GenerateTermUpdate leavesMax was null, so there won't be any splits");
   } else {
      if(0 != cRealDimensions) {
         size_t iDimensionInit = 0;
         const IntEbm* pLeavesMax = leavesMax;
         const TermFeature* pTermFeature = pTerm->GetTermFeatures();
         EBM_ASSERT(1 <= cDimensions);
         const TermFeature* const pTermFeaturesEnd = &pTermFeature[cDimensions];
         do {
            const FeatureBoosting* const pFeature = pTermFeature->m_pFeature;
            const size_t cBins = pFeature->GetCountBins();
            MonotoneDirection featureDirection = MONOTONE_NONE;
            if(nullptr != direction) {
               featureDirection = *direction;
               ++direction;
            }
            if(size_t{1} < cBins) {
               // if there is only 1 dimension then this is our first time here and lastDimensionLeavesMax must be zero
               EBM_ASSERT(2 <= cTensorBins);
               EBM_ASSERT(size_t{2} <= cRealDimensions || IntEbm{0} == lastDimensionLeavesMax);

               iDimensionImportant = iDimensionInit;
               cSignificantBinCount = cBins;
               significantDirection |= featureDirection;
               EBM_ASSERT(nullptr != pLeavesMax);
               const IntEbm countLeavesMax = *pLeavesMax;
               if(countLeavesMax <= IntEbm{1}) {
                  LOG_0(Trace_Warning, "WARNING GenerateTermUpdate countLeavesMax is 1 or less.");
               } else {
                  // keep iteration even once we find this so that we output logs for any bins of 1
                  lastDimensionLeavesMax = countLeavesMax;
               }
            }
            ++iDimensionInit;
            ++pLeavesMax;
            ++pTermFeature;
         } while(pTermFeaturesEnd != pTermFeature);

         EBM_ASSERT(size_t{2} <= cSignificantBinCount);
      }
   }

   EBM_ASSERT(1 <= cTensorBins);
   EBM_ASSERT(2 <= cTensorBins || IntEbm{0} == lastDimensionLeavesMax);

   pBoosterShell->GetTermUpdate()->SetCountDimensions(cDimensions);
   pBoosterShell->GetTermUpdate()->Reset();

   double gainAvg = 0.0;
   if(0 != pBoosterCore->GetTrainingSet()->GetCountSamples()) {
      const double gradientConstant = pBoosterCore->GradientConstant();

      const double multipleCommon = gradientConstant / cInnerBagsAfterZero;
      double multiple = multipleCommon;
      double gainMultiple = multipleCommon;
      if(TermBoostFlags_GradientSums & flags) {
         multiple *= pBoosterCore->LearningRateAdjustmentDifferentialPrivacy();
      } else if(TermBoostFlags_DisableNewtonUpdate & flags) {
         multiple *= pBoosterCore->LearningRateAdjustmentGradientBoosting();
      } else {
         multiple /= pBoosterCore->HessianConstant();
         multiple *= pBoosterCore->LearningRateAdjustmentHessianBoosting();
      }
      if(TermBoostFlags_DisableNewtonGain & flags) {
         gainMultiple *= pBoosterCore->GainAdjustmentGradientBoosting();
      } else {
         gainMultiple /= pBoosterCore->HessianConstant();
         gainMultiple *= pBoosterCore->GainAdjustmentHessianBoosting();
      }
      multiple *= learningRate;
      gainMultiple *= gradientConstant;

      RandomDeterministic* pRng = reinterpret_cast<RandomDeterministic*>(rng);
      RandomDeterministic rngInternal;
      // TODO: move this code down into our called functions since we can happily pass down nullptr into there and then
      // use the rng CPU register trick at the lowest function level
      if(nullptr == pRng) {
         // We use the RNG for two things during the boosting update, and none of them requires
         // a cryptographically secure random number generator. We use the RNG for:
         //   - Deciding ties in regular boosting, but we use random boosting in DP-EBMs, which doesn't have ties
         //   - Deciding split points during random boosting. The DP-EBM proof doesn't rely on the perfect
         //     randomness of the chosen split points. It only relies on the fact that the splits are
         //     chosen independently of the data. We could allow an attacker to choose the split points,
         //     and privacy would be preserved provided the attacker was not able to look at the data when
         //     choosing the splits.
         //
         // Since we do not need high-quality non-determinism, generate a non-deterministic seed
         uint64_t seed;
         try {
            RandomNondeterministic<uint64_t> randomGenerator;
            seed = randomGenerator.Next(std::numeric_limits<uint64_t>::max());
         } catch(const std::bad_alloc&) {
            LOG_0(Trace_Warning, "WARNING GenerateTermUpdate Out of memory in std::random_device");
            return Error_OutOfMemory;
         } catch(...) {
            LOG_0(Trace_Warning, "WARNING GenerateTermUpdate Unknown error in std::random_device");
            return Error_UnexpectedInternal;
         }
         rngInternal.Initialize(seed);
         pRng = &rngInternal;
      }

      pBoosterShell->GetInnerTermUpdate()->SetCountDimensions(cDimensions);
      // if we have ignored dimensions, set the splits count to zero!
      // we only need to do this once instead of per-loop since any dimensions with 1 bin
      // are going to remain having 0 splits.
      pBoosterShell->GetInnerTermUpdate()->Reset();

      if(IntEbm{0} == lastDimensionLeavesMax || (1 != cRealDimensions && MONOTONE_NONE != significantDirection)) {
         // this is kind of hacky where if any one of a number of things occurs (like we have only 1 leaf)
         // we sum everything into a single bin. The alternative would be to always sum into the tensor bins
         // but then collapse them afterwards into a single bin, but that's more work.
         cTensorBins = 1;
      }

      BinBase* const aFastBins = pBoosterShell->GetBoostingFastBinsTemp();
      EBM_ASSERT(nullptr != aFastBins);

      const size_t cBytesPerMainBin = GetBinSize<FloatMain, UIntMain>(true, true, pBoosterCore->IsHessian(), cScores);
      EBM_ASSERT(!IsMultiplyError(cBytesPerMainBin, cTensorBins));
      const size_t cBytesMainBins = cBytesPerMainBin * cTensorBins;

      BinBase* const aMainBins = pBoosterShell->GetBoostingMainBins();
      EBM_ASSERT(nullptr != aMainBins);

#ifndef NDEBUG
      size_t cAuxillaryBins = pTerm->GetCountAuxillaryBins();
      if(0 != (TermBoostFlags_RandomSplits & flags) || 2 < cRealDimensions) {
         // if we're doing random boosting we allocated the auxillary memory, but we don't need it
         cAuxillaryBins = 0;
      }
      EBM_ASSERT(!IsAddError(cTensorBins, cAuxillaryBins));
      EBM_ASSERT(!IsMultiplyError(cBytesPerMainBin, cTensorBins + cAuxillaryBins));
      pBoosterShell->SetDebugMainBinsEnd(IndexBin(aMainBins, cBytesPerMainBin * (cTensorBins + cAuxillaryBins)));
#endif // NDEBUG

      size_t iBag = 0;
      EBM_ASSERT(1 <= cInnerBagsAfterZero);
      do {
         memset(aMainBins, 0, cBytesMainBins);

         EBM_ASSERT(1 <= pBoosterCore->GetTrainingSet()->GetCountSubsets());
         DataSubsetBoosting* pSubset = pBoosterCore->GetTrainingSet()->GetSubsets();
         const DataSubsetBoosting* const pSubsetsEnd = pSubset + pBoosterCore->GetTrainingSet()->GetCountSubsets();
         do {
            int cPack;
            if(1 == cTensorBins) {
               // this is kind of hacky where if any one of a number of things occurs (like we have only 1 leaf)
               // we sum everything into a single bin. The alternative would be to always sum into the tensor bins
               // but then collapse them afterwards into a single bin, but that's more work.
               cPack = k_cItemsPerBitPackUndefined;
            } else {
               EBM_ASSERT(1 <= pTerm->GetBitsRequiredMin());
               cPack =
                     GetCountItemsBitPacked(pTerm->GetBitsRequiredMin(), pSubset->GetObjectiveWrapper()->m_cUIntBytes);
            }

            size_t cBytesPerFastBin;
            if(sizeof(UIntBig) == pSubset->GetObjectiveWrapper()->m_cUIntBytes) {
               if(sizeof(FloatBig) == pSubset->GetObjectiveWrapper()->m_cFloatBytes) {
                  cBytesPerFastBin = GetBinSize<FloatBig, UIntBig>(false, false, pBoosterCore->IsHessian(), cScores);
               } else {
                  EBM_ASSERT(sizeof(FloatSmall) == pSubset->GetObjectiveWrapper()->m_cFloatBytes);
                  cBytesPerFastBin = GetBinSize<FloatSmall, UIntBig>(false, false, pBoosterCore->IsHessian(), cScores);
               }
            } else {
               EBM_ASSERT(sizeof(UIntSmall) == pSubset->GetObjectiveWrapper()->m_cUIntBytes);
               if(sizeof(FloatBig) == pSubset->GetObjectiveWrapper()->m_cFloatBytes) {
                  cBytesPerFastBin = GetBinSize<FloatBig, UIntSmall>(false, false, pBoosterCore->IsHessian(), cScores);
               } else {
                  EBM_ASSERT(sizeof(FloatSmall) == pSubset->GetObjectiveWrapper()->m_cFloatBytes);
                  cBytesPerFastBin =
                        GetBinSize<FloatSmall, UIntSmall>(false, false, pBoosterCore->IsHessian(), cScores);
               }
            }
            EBM_ASSERT(!IsMultiplyError(cBytesPerFastBin, cTensorBins));

            size_t cParallelTensorBins = cTensorBins;
            bool bParallelBins = false;
            const size_t cSIMDPack = pSubset->GetObjectiveWrapper()->m_cSIMDPack;

            // in the future use TermBoostFlags_DisableNewtonGain and TermBoostFlags_DisableNewtonUpdate and
            // TermBoostFlags_GradientSums flags in addition to what the objective allows when setting bHessian
            const bool bHessian = pBoosterCore->IsHessian();
#if 0 < HESSIAN_PARALLEL_BIN_BYTES_MAX || 0 < GRADIENT_PARALLEL_BIN_BYTES_MAX || 0 < MULTISCORE_PARALLEL_BIN_BYTES_MAX
            size_t cBytesParallelMax;
            if(bHessian) {
               if(size_t {1} == cScores) {
                  cBytesParallelMax = HESSIAN_PARALLEL_BIN_BYTES_MAX;
               } else {
                  cBytesParallelMax = MULTISCORE_PARALLEL_BIN_BYTES_MAX;
               }
            } else {
               if(size_t {1} == cScores) {
                  cBytesParallelMax = GRADIENT_PARALLEL_BIN_BYTES_MAX;
               } else {
                  // don't allow parallel gradient multiclass boosting. multiclass should be hessian boosting
                  cBytesParallelMax = 0;
               }
            }
            if(1 != cSIMDPack && 1 != cTensorBins) {
               const size_t cBytesParallel = cBytesPerFastBin * cTensorBins * cSIMDPack;
               if(cBytesParallel <= cBytesParallelMax) {
                  // use parallel bins
                  bParallelBins = true;
                  cParallelTensorBins *= cSIMDPack;
               }
            }
#endif

            aFastBins->ZeroMem(cBytesPerFastBin, cParallelTensorBins);

            BinSumsBoostingBridge params;
            params.m_bParallelBins = bParallelBins ? EBM_TRUE : EBM_FALSE;
            params.m_bHessian = bHessian ? EBM_TRUE : EBM_FALSE;
            params.m_cScores = cScores;
            params.m_cPack = cPack;
            params.m_cSamples = pSubset->GetCountSamples();
            params.m_cBytesFastBins = cBytesPerFastBin * cTensorBins;
            params.m_aGradientsAndHessians = pSubset->GetGradHess();
            params.m_aWeights = pSubset->GetInnerBag(iBag)->GetWeights();
            params.m_aPacked = pSubset->GetTermData(iTerm);
            params.m_aFastBins = aFastBins;
#ifndef NDEBUG
            params.m_pDebugFastBinsEnd = IndexBin(aFastBins, cBytesPerFastBin * cParallelTensorBins);
#endif // NDEBUG
            error = pSubset->BinSumsBoosting(&params);
            if(Error_None != error) {
               return error;
            }

            const bool bUInt64Src = sizeof(UIntBig) == pSubset->GetObjectiveWrapper()->m_cUIntBytes;
            const bool bDoubleSrc = sizeof(FloatBig) == pSubset->GetObjectiveWrapper()->m_cFloatBytes;

            ++pSubset;

            BinBase* pFastBins = aFastBins;
            for(size_t i = 0; i < cSIMDPack; ++i) {
               const UIntMain* aCounts = nullptr;
               const FloatPrecomp* aWeights = nullptr;
               if(pSubsetsEnd == pSubset && (!bParallelBins || i == cSIMDPack - 1)) {
                  // the aCounts and aWeights tensors contain the final counts and weights, so when calling
                  // ConvertAddBin we only want to call it once with these tensors since otherwise they
                  // would be added multiple times
                  aCounts = TermInnerBag::GetCounts(
                        size_t{1} == cTensorBins, iTerm, iBag, pBoosterCore->GetTrainingSet()->GetTermInnerBags());
                  aWeights = TermInnerBag::GetWeights(
                        size_t{1} == cTensorBins, iTerm, iBag, pBoosterCore->GetTrainingSet()->GetTermInnerBags());
               }

               ConvertAddBin(cScores,
                     pBoosterCore->IsHessian(),
                     cTensorBins,
                     bUInt64Src,
                     bDoubleSrc,
                     false,
                     false,
                     pFastBins,
                     aCounts,
                     aWeights,
                     std::is_same<UIntMain, uint64_t>::value,
                     std::is_same<FloatMain, double>::value,
                     aMainBins);

               if(!bParallelBins) {
                  break;
               }
               pFastBins = IndexBin(pFastBins, cBytesPerFastBin * cTensorBins);
            }
         } while(pSubsetsEnd != pSubset);

         // TODO: we can exit here back to python to allow caller modification to our histograms
         //       although having inner bags makes this complicated since each inner bag has it's own
         //       histogram, so we'd need to exit and re-enter 100 times over if we had 100 inner bags
         //       and we'd need to have the BinBoosting function be called 100 times, followed by 100 calls
         //       to cut the tensor, then we'd need to have a single final call to combine the results
         //       which is more complicated.  It will be nicer if we end up eliminated inner bagging
         //       or use subsampling each boost step to avoid having multiple inner bags

         if(1 == cTensorBins) {
            LOG_0(Trace_Warning, "WARNING GenerateTermUpdate boosting zero dimensional");
            BoostZeroDimensional(pBoosterShell, flags);
         } else {
            const double weightTotal = pBoosterCore->GetTrainingSet()->GetBagWeightTotal(iBag);
            EBM_ASSERT(0 < weightTotal); // if all are zeros we assume there are no weights and use the count

            double gain;
            if(0 != (TermBoostFlags_RandomSplits & flags) || 2 < cRealDimensions) {
               // THIS RANDOM SPLIT OPTION IS PRIMARILY USED FOR DIFFERENTIAL PRIVACY EBMs

               error = BoostRandom(pRng, pBoosterShell, iTerm, flags, leavesMax, significantDirection, &gain);
               if(Error_None != error) {
                  return error;
               }
            } else if(1 == cRealDimensions) {
               EBM_ASSERT(nullptr != leavesMax); // otherwise we'd use BoostZeroDimensional above
               EBM_ASSERT(IntEbm{2} <= lastDimensionLeavesMax); // otherwise we'd use BoostZeroDimensional above
               EBM_ASSERT(size_t{2} <= cSignificantBinCount); // otherwise we'd use BoostZeroDimensional above

               EBM_ASSERT(1 == pTerm->GetCountRealDimensions());
               EBM_ASSERT(cSignificantBinCount == pTerm->GetCountTensorBins());
               EBM_ASSERT(0 == pTerm->GetCountAuxillaryBins());

               error = BoostSingleDimensional(pRng,
                     pBoosterShell,
                     flags,
                     cSignificantBinCount,
                     static_cast<FloatMain>(weightTotal),
                     iDimensionImportant,
                     cSamplesLeafMin,
                     minHessian,
                     lastDimensionLeavesMax,
                     significantDirection,
                     &gain);
               if(Error_None != error) {
                  return error;
               }
            } else {
               error = BoostMultiDimensional(pBoosterShell, flags, iTerm, cSamplesLeafMin, minHessian, &gain);
               if(Error_None != error) {
                  return error;
               }
            }

            // gain should be +inf if there was an overflow in our callees
            EBM_ASSERT(!std::isnan(gain));
            EBM_ASSERT(0 <= gain);

            // this could re-promote gain to be +inf again if weightTotal < 1.0
            // do the sample count inversion here in case adding all the avgeraged gains pushes us into +inf
            gain = gain / weightTotal * gainMultiple;
            gainAvg += gain;
            EBM_ASSERT(!std::isnan(gainAvg));
            EBM_ASSERT(0.0 <= gainAvg);
         }

         // TODO : when we thread this code, let's have each thread take a lock and update the combined line segment.
         // They'll each do it while the others are working, so there should be no blocking and our final result won't
         // require adding by the main thread
         error = pBoosterShell->GetTermUpdate()->Add(*pBoosterShell->GetInnerTermUpdate());
         if(Error_None != error) {
            return error;
         }

         ++iBag;
      } while(cInnerBagsAfterZero != iBag);

      // gainAvg is +inf on overflow. It cannot be NaN, but check for that anyways since it's free
      EBM_ASSERT(!std::isnan(gainAvg));
      EBM_ASSERT(0 <= gainAvg);

      if(UNLIKELY(/* NaN */ !LIKELY(gainAvg <= std::numeric_limits<double>::max()))) {
         // this also checks for NaN since NaN < anything is FALSE

         // indicate an error/overflow with -inf similar to interaction strength.
         // Making it -inf gives it the worst ranking possible and avoids the weirdness of NaN

         // it is possible that some of our inner bags overflowed but others did not
         // in some boosting we allow both an update and an overflow.  We indicate the overflow
         // to the caller via a negative gain, but we pass through any update and let the caller
         // decide if they want to stop boosting at that point or continue.
         // So, if there is an update do not reset it here

         gainAvg = k_illegalGainDouble;
      } else {
         EBM_ASSERT(!std::isnan(gainAvg));
         EBM_ASSERT(!std::isinf(gainAvg));
         EBM_ASSERT(0.0 <= gainAvg);
      }

      LOG_0(Trace_Verbose, "GenerateTermUpdate done sampling set loop");

      bool bBad;
      // we need to divide by the number of sampling sets that we constructed this from.
      // We also need to slow down our growth so that the more relevant Features get a chance to grow first so we
      // multiply by a user defined learning rate
      if(size_t{2} == cScores) {
         // if(0 <= k_iZeroLogit || ptrdiff_t { 2 } == pBoosterCore->m_cClasses && bExpandBinaryLogits) {
         //    EBM_ASSERT(ptrdiff_t { 2 } <= pBoosterCore->m_cClasses);
         //    // TODO : for classification with logit zeroing, is our learning rate essentially being inflated as
         //        pBoosterCore->m_cClasses goes up?  If so, maybe we should divide by
         //        pBoosterCore->m_cClasses here to keep learning rates as equivalent as possible..
         //        Actually, I think the real solution here is that
         //    pBoosterCore->m_pTermUpdate->Multiply(
         //       learningRateFloat / cInnerBagsAfterZero * (pBoosterCore->m_cClasses - 1) /
         //       pBoosterCore->m_cClasses
         //    );
         // } else {
         //    // TODO : for classification, is our learning rate essentially being inflated as
         //         pBoosterCore->m_cClasses goes up?  If so, maybe we should divide by
         //         pBoosterCore->m_cClasses here to keep learning rates equivalent as possible
         //    pBoosterCore->m_pTermUpdate->Multiply(learningRateFloat / cInnerBagsAfterZero);
         // }

         // TODO: When NewtonBoosting is enabled, we need to multiply our rate by (K - 1)/K (see above), per:
         // https://arxiv.org/pdf/1810.09092v2.pdf (forumla 5) and also the
         // Ping Li paper (algorithm #1, line 5, (K - 1) / K )
         // https://arxiv.org/pdf/1006.5051.pdf

         bBad = pBoosterShell->GetTermUpdate()->MultiplyAndCheckForIssues(multiple * 0.5);
      } else {
         bBad = pBoosterShell->GetTermUpdate()->MultiplyAndCheckForIssues(multiple);
      }

      if(UNLIKELY(bBad)) {
         // our update contains a NaN or -inf or +inf and we cannot tollerate a model that does this, so destroy it

         pBoosterShell->GetTermUpdate()->SetCountDimensions(cDimensions);
         pBoosterShell->GetTermUpdate()->Reset();

         // also, signal to our caller that an overflow occured with a negative gain
         gainAvg = k_illegalGainDouble;
      }
   }

   pBoosterShell->SetTermIndex(iTerm);

   EBM_ASSERT(!std::isnan(gainAvg));
   EBM_ASSERT(std::numeric_limits<double>::infinity() != gainAvg);
   EBM_ASSERT(k_illegalGainDouble == gainAvg || double{0} <= gainAvg);

   if(nullptr != avgGainOut) {
      *avgGainOut = gainAvg;
   }

   LOG_COUNTED_N(pTerm->GetPointerCountLogExitGenerateTermUpdateMessages(),
         Trace_Info,
         Trace_Verbose,
         "Exited GenerateTermUpdate: "
         "gainAvg=%le",
         gainAvg);

   return Error_None;
}

} // namespace DEFINED_ZONE_NAME
