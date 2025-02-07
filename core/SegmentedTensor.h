// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#ifndef SEGMENTED_TENSOR_H
#define SEGMENTED_TENSOR_H

#include <string.h> // memset
#include <type_traits> // std::is_standard_layout
#include <stdlib.h> // malloc, realloc, free
#include <stddef.h> // size_t, ptrdiff_t

#include "EbmInternal.h" // EBM_INLINE
#include "Logging.h" // EBM_ASSERT & LOG

// TODO : simplify this in our code by removing the templating.  We always use ActiveDataType and FractionalDataType, so we don't need something generic which just complicates reading the code later for no benefit to this project
template<typename TDivisions, typename TValues>
struct SegmentedTensor final {
private:

   struct DimensionInfoStack {
      const TDivisions * m_pDivision1;
      const TDivisions * m_pDivision2;
      size_t m_cNewDivisions;
   };

   struct DimensionInfoStackExpand {
      const TDivisions * m_pDivision1;
      size_t m_iDivision2;
      size_t m_cNewDivisions;
   };

   // TODO : is this still required after we do tree splitting by pairs??
   // we always allocate our array because we don't want to Require Add(...) to check for the null pointer
   // always allocate one so that we never have to check if we have sufficient storage when we call Reset with one division and two values
   static constexpr size_t k_initialDivisionCapacity = 1;
   static constexpr size_t k_initialValueCapacity = 2;

public:

   struct DimensionInfo {
      size_t m_cDivisions;
      TDivisions * m_aDivisions;
      size_t m_cDivisionCapacity;
   };

   size_t m_cValueCapacity;
   size_t m_cVectorLength;
   size_t m_cDimensionsMax;
   size_t m_cDimensions;
   TValues * m_aValues;
   bool m_bExpanded;
   // use the "struct hack" since Flexible array member method is not available in C++
   // m_aDimensions must be the last item in this struct
   DimensionInfo m_aDimensions[1];

   EBM_INLINE static SegmentedTensor * Allocate(const size_t cDimensionsMax, const size_t cVectorLength) {
      EBM_ASSERT(cDimensionsMax <= k_cDimensionsMax);
      EBM_ASSERT(1 <= cVectorLength); // having 0 classes makes no sense, and having 1 class is useless

      if(IsMultiplyError(cVectorLength, k_initialValueCapacity)) {
         LOG_0(TraceLevelWarning, "WARNING Allocate IsMultiplyError(cVectorLength, k_initialValueCapacity)");
         return nullptr;
      }
      const size_t cValueCapacity = cVectorLength * k_initialValueCapacity;
      if(IsMultiplyError(sizeof(TValues), cValueCapacity)) {
         LOG_0(TraceLevelWarning, "WARNING Allocate IsMultiplyError(sizeof(TValues), cValueCapacity)");
         return nullptr;
      }
      const size_t cBytesValues = sizeof(TValues) * cValueCapacity;

      // this can't overflow since cDimensionsMax can't be bigger than k_cDimensionsMax, which is arround 64
      const size_t cBytesSegmentedRegion = sizeof(SegmentedTensor) - sizeof(DimensionInfo) + sizeof(DimensionInfo) * cDimensionsMax;
      SegmentedTensor * const pSegmentedRegion = static_cast<SegmentedTensor *>(malloc(cBytesSegmentedRegion));
      if(UNLIKELY(nullptr == pSegmentedRegion)) {
         LOG_0(TraceLevelWarning, "WARNING Allocate nullptr == pSegmentedRegion");
         return nullptr;
      }
      memset(pSegmentedRegion, 0, cBytesSegmentedRegion); // we do this so that if we later fail while allocating arrays inside of this that we can exit easily, otherwise we would need to be careful to only free pointers that had non-initialized garbage inside of them

      pSegmentedRegion->m_cVectorLength = cVectorLength;
      pSegmentedRegion->m_cDimensionsMax = cDimensionsMax;
      pSegmentedRegion->m_cDimensions = cDimensionsMax;
      pSegmentedRegion->m_cValueCapacity = cValueCapacity;

      TValues * const aValues = static_cast<TValues *>(malloc(cBytesValues));
      if(UNLIKELY(nullptr == aValues)) {
         LOG_0(TraceLevelWarning, "WARNING Allocate nullptr == aValues");
         free(pSegmentedRegion); // don't need to call the full Free(*) yet
         return nullptr;
      }
      pSegmentedRegion->m_aValues = aValues;
      // we only need to set the base case to zero, not our entire initial allocation
      memset(aValues, 0, sizeof(TValues) * cVectorLength); // we checked for cVectorLength * k_initialValueCapacity * sizeof(TValues), and 1 <= k_initialValueCapacity, so sizeof(TValues) * cVectorLength can't overflow

      if(0 != cDimensionsMax) {
         DimensionInfo * pDimension = ARRAY_TO_POINTER(pSegmentedRegion->m_aDimensions);
         size_t iDimension = 0;
         do {
            EBM_ASSERT(0 == pDimension->m_cDivisions);
            pDimension->m_cDivisionCapacity = k_initialDivisionCapacity;
            TDivisions * const aDivisions = static_cast<TDivisions *>(malloc(sizeof(TDivisions) * k_initialDivisionCapacity)); // this multiply can't overflow
            if(UNLIKELY(nullptr == aDivisions)) {
               LOG_0(TraceLevelWarning, "WARNING Allocate nullptr == aDivisions");
               Free(pSegmentedRegion); // free everything!
               return nullptr;
            }
            pDimension->m_aDivisions = aDivisions;
            ++pDimension;
            ++iDimension;
         } while(iDimension < cDimensionsMax);
      }
      return pSegmentedRegion;
   }

   EBM_INLINE static void Free(SegmentedTensor * const pSegmentedRegion) {
      if(LIKELY(nullptr != pSegmentedRegion)) {
         free(pSegmentedRegion->m_aValues);
         if(LIKELY(0 != pSegmentedRegion->m_cDimensionsMax)) {
            const DimensionInfo * pDimensionInfo = ARRAY_TO_POINTER(pSegmentedRegion->m_aDimensions);
            const DimensionInfo * const pDimensionInfoEnd = &pDimensionInfo[pSegmentedRegion->m_cDimensionsMax];
            do {
               free(pDimensionInfo->m_aDivisions);
               ++pDimensionInfo;
            } while(pDimensionInfoEnd != pDimensionInfo);
         }
         free(pSegmentedRegion);
      }
   }

   EBM_INLINE void SetCountDimensions(const size_t cDimensions) {
      EBM_ASSERT(cDimensions <= m_cDimensionsMax);
      m_cDimensions = cDimensions;
   }

   EBM_INLINE TDivisions * GetDivisionPointer(const size_t iDimension) {
      EBM_ASSERT(iDimension < m_cDimensions);
      return &ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_aDivisions[0];
   }

   EBM_INLINE TValues * GetValuePointer() {
      return &m_aValues[0];
   }

   EBM_INLINE void Reset() {
      for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
         ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_cDivisions = 0;
      }
      // we only need to set the base case to zero
      // this can't overflow since we previously allocated this memory
      memset(m_aValues, 0, sizeof(TValues) * m_cVectorLength);
      m_bExpanded = false;
   }

   EBM_INLINE bool SetCountDivisions(const size_t iDimension, const size_t cDivisions) {
      EBM_ASSERT(iDimension < m_cDimensions);
      DimensionInfo * const pDimension = &ARRAY_TO_POINTER(m_aDimensions)[iDimension];
      EBM_ASSERT(!m_bExpanded || cDivisions <= pDimension->m_cDivisions); // we shouldn't be able to expand our length after we're been expanded since expanded should be the maximum size already
      if(UNLIKELY(pDimension->m_cDivisionCapacity < cDivisions)) {
         EBM_ASSERT(!m_bExpanded); // we shouldn't be able to expand our length after we're been expanded since expanded should be the maximum size already

         if(IsAddError(cDivisions, cDivisions >> 1)) {
            LOG_0(TraceLevelWarning, "WARNING SetCountDivisions IsAddError(cDivisions, cDivisions >> 1)");
            return true;
         }
         size_t cNewDivisionCapacity = cDivisions + (cDivisions >> 1); // just increase it by 50% since we don't expect to grow our divisions often after an initial period, and realloc takes some of the cost of growing away
         LOG_N(TraceLevelInfo, "SetCountDivisions Growing to size %zu", cNewDivisionCapacity);

         if(IsMultiplyError(sizeof(TDivisions), cNewDivisionCapacity)) {
            LOG_0(TraceLevelWarning, "WARNING SetCountDivisions IsMultiplyError(sizeof(TDivisions), cNewDivisionCapacity)");
            return true;
         }
         size_t cBytes = sizeof(TDivisions) * cNewDivisionCapacity;
         TDivisions * const aNewDivisions = static_cast<TDivisions *>(realloc(pDimension->m_aDivisions, cBytes));
         if(UNLIKELY(nullptr == aNewDivisions)) {
            // according to the realloc spec, if realloc fails to allocate the new memory, it returns nullptr BUT the old memory is valid.
            // we leave m_aThreadByteBuffer1 alone in this instance and will free that memory later in the destructor
            LOG_0(TraceLevelWarning, "WARNING SetCountDivisions nullptr == aNewDivisions");
            return true;
         }
         pDimension->m_aDivisions = aNewDivisions;
         pDimension->m_cDivisionCapacity = cNewDivisionCapacity;
      } // never shrink our array unless the user chooses to Trim()
      pDimension->m_cDivisions = cDivisions;
      return false;
   }

   EBM_INLINE bool EnsureValueCapacity(const size_t cValues) {
      if(UNLIKELY(m_cValueCapacity < cValues)) {
         EBM_ASSERT(!m_bExpanded); // we shouldn't be able to expand our length after we're been expanded since expanded should be the maximum size already

         if(IsAddError(cValues, cValues >> 1)) {
            LOG_0(TraceLevelWarning, "WARNING EnsureValueCapacity IsAddError(cValues, cValues >> 1)");
            return true;
         }
         size_t cNewValueCapacity = cValues + (cValues >> 1); // just increase it by 50% since we don't expect to grow our values often after an initial period, and realloc takes some of the cost of growing away
         LOG_N(TraceLevelInfo, "EnsureValueCapacity Growing to size %zu", cNewValueCapacity);

         if(IsMultiplyError(sizeof(TValues), cNewValueCapacity)) {
            LOG_0(TraceLevelWarning, "WARNING EnsureValueCapacity IsMultiplyError(sizeof(TValues), cNewValueCapacity)");
            return true;
         }
         size_t cBytes = sizeof(TValues) * cNewValueCapacity;
         TValues * const aNewValues = static_cast<TValues *>(realloc(m_aValues, cBytes));
         if(UNLIKELY(nullptr == aNewValues)) {
            // according to the realloc spec, if realloc fails to allocate the new memory, it returns nullptr BUT the old memory is valid.
            // we leave m_aThreadByteBuffer1 alone in this instance and will free that memory later in the destructor
            LOG_0(TraceLevelWarning, "WARNING EnsureValueCapacity nullptr == aNewValues");
            return true;
         }
         m_aValues = aNewValues;
         m_cValueCapacity = cNewValueCapacity;
      } // never shrink our array unless the user chooses to Trim()
      return false;
   }

   EBM_INLINE bool Copy(const SegmentedTensor & rhs) {
      EBM_ASSERT(m_cDimensions == rhs.m_cDimensions);

      size_t cValues = m_cVectorLength;
      for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
         const DimensionInfo * const pDimension = &ARRAY_TO_POINTER_CONST(rhs.m_aDimensions)[iDimension];
         size_t cDivisions = pDimension->m_cDivisions;
         EBM_ASSERT(!IsMultiplyError(cValues, cDivisions + 1)); // we're copying this memory, so multiplication can't overflow
         cValues *= (cDivisions + 1);
         if(UNLIKELY(SetCountDivisions(iDimension, cDivisions))) {
            LOG_0(TraceLevelWarning, "WARNING Copy SetCountDivisions(iDimension, cDivisions)");
            return true;
         }
         EBM_ASSERT(!IsMultiplyError(sizeof(TDivisions), cDivisions)); // we're copying this memory, so multiplication can't overflow
         memcpy(ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_aDivisions, pDimension->m_aDivisions, sizeof(TDivisions) * cDivisions);
      }
      if(UNLIKELY(EnsureValueCapacity(cValues))) {
         LOG_0(TraceLevelWarning, "WARNING Copy EnsureValueCapacity(cValues)");
         return true;
      }
      EBM_ASSERT(!IsMultiplyError(sizeof(TValues), cValues)); // we're copying this memory, so multiplication can't overflow
      memcpy(m_aValues, rhs.m_aValues, sizeof(TValues) * cValues);
      m_bExpanded = rhs.m_bExpanded;
      return false;
   }

//#ifndef NDEBUG
//   EBM_INLINE TValues * GetValue(const TDivisions * const aDivisionValue) const {
//      if(0 == m_cDimensions) {
//         return &m_aValues[0]; // there are no dimensions, and only 1 value
//      }
//      const DimensionInfo * pDimension = ARRAY_TO_POINTER(m_aDimensions);
//      const TDivisions * pDivisionValue = aDivisionValue;
//      const TDivisions * const pDivisionValueEnd = &aDivisionValue[m_cDimensions];
//      size_t iValue = 0;
//      size_t valueMultiple = m_cVectorLength;
//
//      if(m_bExpanded) {
//         while(true) {
//            const TDivisions d = *pDivisionValue;
//            EBM_ASSERT(!IsMultiplyError(d, valueMultiple)); // we're accessing existing memory, so it can't overflow
//            size_t addValue = d * valueMultiple;
//            EBM_ASSERT(!IsAddError(addValue, iValue)); // we're accessing existing memory, so it can't overflow
//            iValue += addValue;
//            ++pDivisionValue;
//            if(pDivisionValueEnd == pDivisionValue) {
//               break;
//            }
//            const size_t cDivisions = pDimension->m_cDivisions;
//            EBM_ASSERT(1 <= cDivisions); // since we're expanded we should have at least one division and two values
//            EBM_ASSERT(!IsMultiplyError(cDivisions + 1, valueMultiple)); // we're accessing existing memory, so it can't overflow
//            valueMultiple *= cDivisions + 1;
//            ++pDimension;
//         }
//      } else {
//         DO: this code is no longer executed because we always expand our models now.  We can probably get rid of it, but I'm leaving it here for a while to decide if there are really no use cases
//         do {
//            const size_t cDivisions = pDimension->m_cDivisions;
//            if(LIKELY(0 != cDivisions)) {
//               const TDivisions * const aDivisions = pDimension->m_aDivisions;
//               const TDivisions d = *pDivisionValue;
//               ptrdiff_t high = cDivisions - 1;
//               ptrdiff_t middle;
//               ptrdiff_t low = 0;
//               TDivisions midVal;
//               do {
//                  middle = (low + high) >> 1;
//                  midVal = aDivisions[middle];
//                  if(UNLIKELY(midVal == d)) {
//                     // this happens just once during our descent, so it's less likely than continuing searching
//                     goto no_check;
//                  }
//                  high = UNPREDICTABLE(midVal < d) ? high : middle - 1;
//                  low = UNPREDICTABLE(midVal < d) ? middle + 1 : low;
//               } while(LIKELY(low <= high));
//               middle = UNPREDICTABLE(midVal < d) ? middle + 1 : middle;
//            no_check:
//               EBM_ASSERT(!IsMultiplyError(middle, valueMultiple)); // we're accessing existing memory, so it can't overflow
//               ptrdiff_t addValue = middle * valueMultiple;
//               EBM_ASSERT(!IsAddError(iValue, addValue)); // we're accessing existing memory, so it can't overflow
//               iValue += addValue;
//               EBM_ASSERT(!IsMultiplyError(valueMultiple, cDivisions + 1)); // we're accessing existing memory, so it can't overflow
//               valueMultiple *= cDivisions + 1;
//            }
//            ++pDimension;
//            ++pDivisionValue;
//         } while(pDivisionValueEnd != pDivisionValue);
//      }
//      return &m_aValues[iValue];
//   }
//#endif // NDEBUG

   EBM_INLINE void Multiply(const TValues v) {
      size_t cValues = 1;
      for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
         EBM_ASSERT(!IsMultiplyError(cValues, ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_cDivisions + 1)); // we're accessing existing memory, so it can't overflow
         cValues *= ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_cDivisions + 1;
      }

      TValues * pCur = &m_aValues[0];
      TValues * pEnd = &m_aValues[cValues * m_cVectorLength];
      // we always have 1 value, even if we have zero divisions
      do {
         *pCur *= v;
         ++pCur;
      } while(pEnd != pCur);
   }

   bool Expand(const size_t * const acValuesPerDimension) {
      LOG_0(TraceLevelVerbose, "Entered Expand");

      EBM_ASSERT(1 <= m_cDimensions); // you can't really expand something with zero dimensions
      EBM_ASSERT(nullptr != acValuesPerDimension);
      // ok, checking the max isn't really the best here, but doing this right seems pretty complicated, and this should detect any real problems.
      // don't make this a static assert.  The rest of our class is fine as long as Expand is never called
      EBM_ASSERT(std::numeric_limits<size_t>::max() == std::numeric_limits<TDivisions>::max() && std::numeric_limits<size_t>::min() == std::numeric_limits<TDivisions>::min());
      if(m_bExpanded) {
         // we're already expanded
         LOG_0(TraceLevelVerbose, "Exited Expand");
         return false;
      }

      EBM_ASSERT(m_cDimensions <= k_cDimensionsMax);
      DimensionInfoStackExpand aDimensionInfoStackExpand[k_cDimensionsMax];

      const DimensionInfo * pDimensionFirst1 = ARRAY_TO_POINTER(m_aDimensions);

      DimensionInfoStackExpand * pDimensionInfoStackFirst = aDimensionInfoStackExpand;
      const DimensionInfoStackExpand * const pDimensionInfoStackEnd = &aDimensionInfoStackExpand[m_cDimensions];
      const size_t * pcValuesPerDimension = acValuesPerDimension;

      size_t cValues1 = 1;
      size_t cNewValues = 1;

      EBM_ASSERT(0 < m_cDimensions);
      // first, get basic counts of how many divisions and values we'll have in our final result
      do {
         const size_t cDivisions1 = pDimensionFirst1->m_cDivisions;

         EBM_ASSERT(!IsMultiplyError(cValues1, cDivisions1 + 1)); // this is accessing existing memory, so it can't overflow
         cValues1 *= cDivisions1 + 1;

         pDimensionInfoStackFirst->m_pDivision1 = &pDimensionFirst1->m_aDivisions[cDivisions1];
         const size_t cValuesPerDimension = *pcValuesPerDimension;
         EBM_ASSERT(!IsMultiplyError(cNewValues, cValuesPerDimension));  // we check for simple multiplication overflow from m_cBins in EbmTrainingState->Initialize when we unpack featureCombinationIndexes and in GetInteractionScore for interactions
         cNewValues *= cValuesPerDimension;
         const size_t cNewDivisions = cValuesPerDimension - 1;

         pDimensionInfoStackFirst->m_iDivision2 = cNewDivisions;
         pDimensionInfoStackFirst->m_cNewDivisions = cNewDivisions;

         ++pDimensionFirst1;
         ++pcValuesPerDimension;
         ++pDimensionInfoStackFirst;
      } while(pDimensionInfoStackEnd != pDimensionInfoStackFirst);

      if(IsMultiplyError(cNewValues, m_cVectorLength)) {
         LOG_0(TraceLevelWarning, "WARNING Expand IsMultiplyError(cNewValues, m_cVectorLength)");
         return true;
      }
      const size_t cVectoredNewValues = cNewValues * m_cVectorLength;
      // call EnsureValueCapacity before using the m_aValues pointer since m_aValues might change inside EnsureValueCapacity
      if(UNLIKELY(EnsureValueCapacity(cVectoredNewValues))) {
         LOG_0(TraceLevelWarning, "WARNING Expand EnsureValueCapacity(cVectoredNewValues))");
         return true;
      }

      TValues * const aValues = m_aValues;
      const DimensionInfo * const aDimension1 = ARRAY_TO_POINTER(m_aDimensions);

      EBM_ASSERT(cValues1 <= cNewValues);
      EBM_ASSERT(!IsMultiplyError(m_cVectorLength, cValues1)); // we checked against cNewValues above, and cValues1 should be smaller
      const TValues * pValue1 = &aValues[m_cVectorLength * cValues1];
      TValues * pValueTop = &aValues[cVectoredNewValues];

      // traverse the values in reverse so that we can put our results at the higher order indexes where we are guaranteed not to overwrite our existing values which we still need to copy
      // first do the values because we need to refer to the old divisions when making decisions about where to move next
      while(true) {
         const TValues * pValue1Move = pValue1;
         const TValues * const pValueTopEnd = pValueTop - m_cVectorLength;
         do {
            --pValue1Move;
            --pValueTop;
            *pValueTop = *pValue1Move;
         } while(pValueTopEnd != pValueTop);

         // For a single dimensional SegmentedRegion checking here is best.  
         // For two or higher dimensions, we could instead check inside our loop below for when we reach the end of the pDimensionInfoStack, thus eliminating the check on most loops.  
         // we'll spend most of our time working on single features though, so we optimize for that case, but if we special cased the single dimensional case, then we would want 
         // to move this check into the loop below in the case of multi-dimensioncal SegmentedTensors
         if(UNLIKELY(aValues == pValueTop)) {
            // we've written our final tensor cell, so we're done
            break;
         }

         DimensionInfoStackExpand * pDimensionInfoStackSecond = aDimensionInfoStackExpand;
         const DimensionInfo * pDimensionSecond1 = aDimension1;

         size_t multiplication1 = m_cVectorLength;

         while(true) {
            const TDivisions * const pDivision1 = pDimensionInfoStackSecond->m_pDivision1;
            size_t iDivision2 = pDimensionInfoStackSecond->m_iDivision2;

            TDivisions * const aDivisions1 = pDimensionSecond1->m_aDivisions;

            if(UNPREDICTABLE(aDivisions1 < pDivision1)) {
               EBM_ASSERT(0 < iDivision2);

               const TDivisions * const pDivision1MinusOne = pDivision1 - 1;

               const size_t d1 = static_cast<size_t>(*pDivision1MinusOne);

               --iDivision2;

               const bool bMove = UNPREDICTABLE(iDivision2 <= d1);
               pDimensionInfoStackSecond->m_pDivision1 = bMove ? pDivision1MinusOne : pDivision1;
               pValue1 = bMove ? pValue1 - multiplication1 : pValue1;

               pDimensionInfoStackSecond->m_iDivision2 = iDivision2;
               break;
            } else {
               if(UNPREDICTABLE(0 < iDivision2)) {
                  pDimensionInfoStackSecond->m_iDivision2 = iDivision2 - 1;
                  break;
               } else {
                  pValue1 -= multiplication1; // put us before the beginning.  We'll add the full row first

                  const size_t cDivisions1 = pDimensionSecond1->m_cDivisions;

                  EBM_ASSERT(!IsMultiplyError(multiplication1, 1 + cDivisions1)); // we're already allocated values, so this is accessing what we've already allocated, so it must not overflow
                  multiplication1 *= 1 + cDivisions1;

                  pValue1 += multiplication1; // go to the last valid entry back to where we started.  If we don't move down a set, then we re-do this set of numbers

                  pDimensionInfoStackSecond->m_pDivision1 = &aDivisions1[cDivisions1];
                  pDimensionInfoStackSecond->m_iDivision2 = pDimensionInfoStackSecond->m_cNewDivisions;

                  ++pDimensionSecond1;
                  ++pDimensionInfoStackSecond;
                  continue;
               }
            }
         }
      }

      EBM_ASSERT(pValueTop == m_aValues);
      EBM_ASSERT(pValue1 == m_aValues + m_cVectorLength);

      for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
         const size_t cDivisions = acValuesPerDimension[iDimension] - 1;

         if(cDivisions == ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_cDivisions) {
            continue;
         }

         if(UNLIKELY(SetCountDivisions(iDimension, cDivisions))) {
            LOG_0(TraceLevelWarning, "WARNING Expand SetCountDivisions(iDimension, cDivisions)");
            return true;
         }

         for(size_t iDivision = 0; iDivision < cDivisions; ++iDivision) {
            ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_aDivisions[iDivision] = iDivision;
         }
      }

      m_bExpanded = true;
      LOG_0(TraceLevelVerbose, "Exited Expand");
      return false;
   }

   EBM_INLINE void AddExpanded(const TValues * const aFromValues) {
      EBM_ASSERT(m_bExpanded);
      size_t cItems = m_cVectorLength;
      for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
         // this can't overflow since we've already allocated them!
         cItems *= ARRAY_TO_POINTER(m_aDimensions)[iDimension].m_cDivisions + 1;
      }

      const TValues * pFromValue = aFromValues;
      TValues * pToValue = m_aValues;
      const TValues * const pToValueEnd = m_aValues + cItems;
      do {
         *pToValue += *pFromValue;
         ++pFromValue;
         ++pToValue;
      } while(pToValueEnd != pToValue);
   }

   // TODO : consider adding templated cVectorLength and cDimensions to this function.  At worst someone can pass in 0 and use the loops without needing to super-optimize it
   bool Add(const SegmentedTensor & rhs) {
      DimensionInfoStack dimensionStack[k_cDimensionsMax];

      EBM_ASSERT(m_cDimensions == rhs.m_cDimensions);

      if(0 == m_cDimensions) {
         EBM_ASSERT(1 <= m_cValueCapacity);
         EBM_ASSERT(nullptr != m_aValues);

         TValues * pTo = &m_aValues[0];
         const TValues * pFrom = &rhs.m_aValues[0];
         const TValues * const pToEnd = &pTo[m_cVectorLength];
         do {
            *pTo += *pFrom;
            ++pTo;
            ++pFrom;
         } while(pToEnd != pTo);

         return false;
      }

      if(m_bExpanded) {
         // TODO: the existing code below works, but handle this differently (we can do it more efficiently)
      }

      if(rhs.m_bExpanded) {
         // TODO: the existing code below works, but handle this differently (we can do it more efficiently)
      }

      const DimensionInfo * pDimensionFirst1 = ARRAY_TO_POINTER(m_aDimensions);
      const DimensionInfo * pDimensionFirst2 = ARRAY_TO_POINTER_CONST(rhs.m_aDimensions);

      DimensionInfoStack * pDimensionInfoStackFirst = dimensionStack;
      const DimensionInfoStack * const pDimensionInfoStackEnd = &dimensionStack[m_cDimensions];

      size_t cValues1 = 1;
      size_t cValues2 = 1;
      size_t cNewValues = 1;

      EBM_ASSERT(0 < m_cDimensions);
      // first, get basic counts of how many divisions and values we'll have in our final result
      do {
         const size_t cDivisions1 = pDimensionFirst1->m_cDivisions;
         TDivisions * p1Cur = pDimensionFirst1->m_aDivisions;
         const size_t cDivisions2 = pDimensionFirst2->m_cDivisions;
         TDivisions * p2Cur = pDimensionFirst2->m_aDivisions;

         cValues1 *= cDivisions1 + 1; // this can't overflow since we're counting existing allocated memory
         cValues2 *= cDivisions2 + 1; // this can't overflow since we're counting existing allocated memory

         TDivisions * const p1End = &p1Cur[cDivisions1];
         TDivisions * const p2End = &p2Cur[cDivisions2];

         pDimensionInfoStackFirst->m_pDivision1 = p1End;
         pDimensionInfoStackFirst->m_pDivision2 = p2End;

         size_t cNewSingleDimensionDivisions = 0;

         // processing forwards here is slightly faster in terms of cache fetch efficiency.  We'll then be guaranteed to have the divisions at least in the cache, which will be benefitial when traversing backwards later below
         while(true) {
            if(UNLIKELY(p2End == p2Cur)) {
               // check the other array first.  Most of the time the other array will be shorter since we'll be adding
               // a sequence of Segmented lines and our main line will be in *this, and there will be more segments in general for
               // a line that is added to a lot
               cNewSingleDimensionDivisions += static_cast<size_t>(p1End - p1Cur);
               break;
            }
            if(UNLIKELY(p1End == p1Cur)) {
               cNewSingleDimensionDivisions += static_cast<size_t>(p2End - p2Cur);
               break;
            }
            ++cNewSingleDimensionDivisions; // if we move one or both pointers, we just added annother unique one

            const TDivisions d1 = *p1Cur;
            const TDivisions d2 = *p2Cur;

            p1Cur = UNPREDICTABLE(d1 <= d2) ? p1Cur + 1 : p1Cur;
            p2Cur = UNPREDICTABLE(d2 <= d1) ? p2Cur + 1 : p2Cur;
         }
         pDimensionInfoStackFirst->m_cNewDivisions = cNewSingleDimensionDivisions;
         EBM_ASSERT(!IsMultiplyError(cNewValues, cNewSingleDimensionDivisions + 1)); // we check for simple multiplication overflow from m_cBins in EbmTrainingState->Initialize when we unpack featureCombinationIndexes and in GetInteractionScore for interactions
         cNewValues *= cNewSingleDimensionDivisions + 1;

         ++pDimensionFirst1;
         ++pDimensionFirst2;

         ++pDimensionInfoStackFirst;
      } while(pDimensionInfoStackEnd != pDimensionInfoStackFirst);

      if(IsMultiplyError(cNewValues, m_cVectorLength)) {
         LOG_0(TraceLevelWarning, "WARNING Add IsMultiplyError(cNewValues, m_cVectorLength)");
         return true;
      }
      // call EnsureValueCapacity before using the m_aValues pointer since m_aValues might change inside EnsureValueCapacity
      if(UNLIKELY(EnsureValueCapacity(cNewValues * m_cVectorLength))) {
         LOG_0(TraceLevelWarning, "WARNING Add EnsureValueCapacity(cNewValues * m_cVectorLength)");
         return true;
      }

      const TValues * pValue2 = &rhs.m_aValues[m_cVectorLength * cValues2];  // we're accessing allocated memory, so it can't overflow
      const DimensionInfo * const aDimension2 = ARRAY_TO_POINTER_CONST(rhs.m_aDimensions);

      TValues * const aValues = m_aValues;
      const DimensionInfo * const aDimension1 = ARRAY_TO_POINTER(m_aDimensions);

      const TValues * pValue1 = &aValues[m_cVectorLength * cValues1]; // we're accessing allocated memory, so it can't overflow
      TValues * pValueTop = &aValues[m_cVectorLength * cNewValues]; // we're accessing allocated memory, so it can't overflow

      // traverse the values in reverse so that we can put our results at the higher order indexes where we are guaranteed not to overwrite our existing values which we still need to copy
      // first do the values because we need to refer to the old divisions when making decisions about where to move next
      while(true) {
         const TValues * pValue1Move = pValue1;
         const TValues * pValue2Move = pValue2;
         const TValues * const pValueTopEnd = pValueTop - m_cVectorLength;
         do {
            --pValue1Move;
            --pValue2Move;
            --pValueTop;
            *pValueTop = *pValue1Move + *pValue2Move;
         } while(pValueTopEnd != pValueTop);

         // For a single dimensional SegmentedRegion checking here is best.  
         // For two or higher dimensions, we could instead check inside our loop below for when we reach the end of the pDimensionInfoStack, thus eliminating the check on most loops.  
         // we'll spend most of our time working on single features though, so we optimize for that case, but if we special cased the single dimensional case, then we would want 
         // to move this check into the loop below in the case of multi-dimensioncal SegmentedTensors
         if(UNLIKELY(aValues == pValueTop)) {
            // we've written our final tensor cell, so we're done
            break;
         }

         DimensionInfoStack * pDimensionInfoStackSecond = dimensionStack;
         const DimensionInfo * pDimensionSecond1 = aDimension1;
         const DimensionInfo * pDimensionSecond2 = aDimension2;

         size_t multiplication1 = m_cVectorLength;
         size_t multiplication2 = m_cVectorLength;

         while(true) {
            const TDivisions * const pDivision1 = pDimensionInfoStackSecond->m_pDivision1;
            const TDivisions * const pDivision2 = pDimensionInfoStackSecond->m_pDivision2;

            TDivisions * const aDivisions1 = pDimensionSecond1->m_aDivisions;
            TDivisions * const aDivisions2 = pDimensionSecond2->m_aDivisions;

            if(UNPREDICTABLE(aDivisions1 < pDivision1)) {
               if(UNPREDICTABLE(aDivisions2 < pDivision2)) {
                  const TDivisions * const pDivision1MinusOne = pDivision1 - 1;
                  const TDivisions * const pDivision2MinusOne = pDivision2 - 1;

                  const TDivisions d1 = *pDivision1MinusOne;
                  const TDivisions d2 = *pDivision2MinusOne;

                  const bool bMove1 = UNPREDICTABLE(d2 <= d1);
                  pDimensionInfoStackSecond->m_pDivision1 = bMove1 ? pDivision1MinusOne : pDivision1;
                  pValue1 = bMove1 ? pValue1 - multiplication1 : pValue1;

                  const bool bMove2 = UNPREDICTABLE(d1 <= d2);
                  pDimensionInfoStackSecond->m_pDivision2 = bMove2 ? pDivision2MinusOne : pDivision2;
                  pValue2 = bMove2 ? pValue2 - multiplication2 : pValue2;
                  break;
               } else {
                  pValue1 -= multiplication1;
                  pDimensionInfoStackSecond->m_pDivision1 = pDivision1 - 1;
                  break;
               }
            } else {
               if(UNPREDICTABLE(aDivisions2 < pDivision2)) {
                  pValue2 -= multiplication2;
                  pDimensionInfoStackSecond->m_pDivision2 = pDivision2 - 1;
                  break;
               } else {
                  pValue1 -= multiplication1; // put us before the beginning.  We'll add the full row first
                  pValue2 -= multiplication2; // put us before the beginning.  We'll add the full row first

                  const size_t cDivisions1 = pDimensionSecond1->m_cDivisions;
                  const size_t cDivisions2 = pDimensionSecond2->m_cDivisions;

                  EBM_ASSERT(!IsMultiplyError(multiplication1, 1 + cDivisions1)); // we're accessing allocated memory, so it can't overflow
                  multiplication1 *= 1 + cDivisions1;
                  EBM_ASSERT(!IsMultiplyError(multiplication2, 1 + cDivisions2)); // we're accessing allocated memory, so it can't overflow
                  multiplication2 *= 1 + cDivisions2;

                  pValue1 += multiplication1; // go to the last valid entry back to where we started.  If we don't move down a set, then we re-do this set of numbers
                  pValue2 += multiplication2; // go to the last valid entry back to where we started.  If we don't move down a set, then we re-do this set of numbers

                  pDimensionInfoStackSecond->m_pDivision1 = &aDivisions1[cDivisions1];
                  pDimensionInfoStackSecond->m_pDivision2 = &aDivisions2[cDivisions2];
                  ++pDimensionSecond1;
                  ++pDimensionSecond2;
                  ++pDimensionInfoStackSecond;
                  continue;
               }
            }
         }
      }

      EBM_ASSERT(pValueTop == m_aValues);
      EBM_ASSERT(pValue1 == m_aValues + m_cVectorLength);
      EBM_ASSERT(pValue2 == rhs.m_aValues + m_cVectorLength);

      // now finally do the divisions

      const DimensionInfoStack * pDimensionInfoStackCur = dimensionStack;
      const DimensionInfo * pDimension1Cur = aDimension1;
      const DimensionInfo * pDimension2Cur = aDimension2;
      size_t iDimension = 0;
      do {
         const size_t cNewDivisions = pDimensionInfoStackCur->m_cNewDivisions;
         const size_t cOriginalDivisionsBeforeSetting = pDimension1Cur->m_cDivisions;
         
         // this will increase our capacity, if required.  It will also change m_cDivisions, so we get that before calling it.  SetCountDivisions might change m_aValuesAndDivisions, so we need to actually keep it here after getting m_cDivisions but before set set all our pointers
         if(UNLIKELY(SetCountDivisions(iDimension, cNewDivisions))) {
            LOG_0(TraceLevelWarning, "WARNING Add SetCountDivisions(iDimension, cNewDivisions)");
            return true;
         }
         
         const TDivisions * p1Cur = &pDimension1Cur->m_aDivisions[cOriginalDivisionsBeforeSetting];
         const TDivisions * p2Cur = &pDimension2Cur->m_aDivisions[pDimension2Cur->m_cDivisions];
         TDivisions * pTopCur = &pDimension1Cur->m_aDivisions[cNewDivisions];

         // traverse in reverse so that we can put our results at the higher order indexes where we are guaranteed not to overwrite our existing values which we still need to copy
         while(true) {
            EBM_ASSERT(pDimension1Cur->m_aDivisions <= pTopCur);
            EBM_ASSERT(pDimension1Cur->m_aDivisions <= p1Cur);
            EBM_ASSERT(pDimension2Cur->m_aDivisions <= p2Cur);
            EBM_ASSERT(p1Cur <= pTopCur);
            EBM_ASSERT(static_cast<size_t>(p2Cur - pDimension2Cur->m_aDivisions) <= static_cast<size_t>(pTopCur - pDimension1Cur->m_aDivisions));

            if(UNLIKELY(pTopCur == p1Cur)) {
               // since we've finished the rhs divisions, our SegmentedRegion already has the right divisions in place, so all we need is to add the value of the last region in rhs to our remaining values
               break;
            }
            // pTopCur is an index above pDimension1Cur->m_aDivisions.  p2Cur is an index above pDimension2Cur->m_aDivisions.  We want to decide if they are at the same index above their respective arrays
            if(UNLIKELY(static_cast<size_t>(pTopCur - pDimension1Cur->m_aDivisions) == static_cast<size_t>(p2Cur - pDimension2Cur->m_aDivisions))) {
               EBM_ASSERT(pDimension1Cur->m_aDivisions < pTopCur);
               // direct copy the remaining divisions.  There should be at least one
               memcpy(pDimension1Cur->m_aDivisions, pDimension2Cur->m_aDivisions, static_cast<size_t>(pTopCur - pDimension1Cur->m_aDivisions) * sizeof(TDivisions));
               break;
            }

            const TDivisions * const p1CurMinusOne = p1Cur - 1;
            const TDivisions * const p2CurMinusOne = p2Cur - 1;

            const TDivisions d1 = *p1CurMinusOne;
            const TDivisions d2 = *p2CurMinusOne;

            p1Cur = UNPREDICTABLE(d2 <= d1) ? p1CurMinusOne : p1Cur;
            p2Cur = UNPREDICTABLE(d1 <= d2) ? p2CurMinusOne : p2Cur;

            const TDivisions d = UNPREDICTABLE(d1 <= d2) ? d2 : d1;

            --pTopCur; // if we move one or both pointers, we just added annother unique one
            *pTopCur = d;
         }
         ++pDimension1Cur;
         ++pDimension2Cur;
         ++pDimensionInfoStackCur;
         ++iDimension;
      } while(iDimension != m_cDimensions);
      return false;
   }

#ifndef NDEBUG
   bool IsEqual(const SegmentedTensor & rhs) const {
      if(m_cDimensions != rhs.m_cDimensions) {
         return false;
      }

      size_t cValues = m_cVectorLength;
      for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
         const DimensionInfo * const pDimension1 = &ARRAY_TO_POINTER(m_aDimensions)[iDimension];
         const DimensionInfo * const pDimension2 = &ARRAY_TO_POINTER(rhs.m_aDimensions)[iDimension];

         size_t cDivisions = pDimension1->m_cDivisions;
         if(cDivisions != pDimension2->m_cDivisions) {
            return false;
         }

         if(0 != cDivisions) {
            EBM_ASSERT(!IsMultiplyError(cValues, cDivisions + 1)); // we're accessing allocated memory, so it can't overflow
            cValues *= cDivisions + 1;

            const TDivisions * pD1Cur = pDimension1->m_aDivisions;
            const TDivisions * pD2Cur = pDimension2->m_aDivisions;
            const TDivisions * const pD1End = pD1Cur + cDivisions;
            do {
               if(UNLIKELY(*pD1Cur != *pD2Cur)) {
                  return false;
               }
               ++pD1Cur;
               ++pD2Cur;
            } while(LIKELY(pD1End != pD1Cur));
         }
      }

      const TValues * pV1Cur = &m_aValues[0];
      const TValues * pV2Cur = &rhs.m_aValues[0];
      const TValues * const pV1End = pV1Cur + cValues;
      do {
         if(UNLIKELY(*pV1Cur != *pV2Cur)) {
            return false;
         }
         ++pV1Cur;
         ++pV2Cur;
      } while(LIKELY(pV1End != pV1Cur));

      return true;
   }
#endif // NDEBUG

   static_assert(std::is_standard_layout<TDivisions>::value, "SegmentedRegion must be a standard layout class.  We use realloc, which isn't compatible with using complex classes.  Interop data must also be standard layout classes.  Lastly, we put this class into a union, so the destructor would need to be called manually anyways");
   static_assert(std::is_standard_layout<TValues>::value, "SegmentedRegion must be a standard layout class.  We use realloc, which isn't compatible with using complex classes.  Interop data must also be standard layout classes.  Lastly, we put this class into a union, so the destructor would need to be called manually anyways");
};
static_assert(std::is_standard_layout<SegmentedTensor<ActiveDataType, FractionalDataType>>::value, "SegmentedRegion must be a standard layout class.  We use realloc, which isn't compatible with using complex classes.  Interop data must also be standard layout classes.  Lastly, we put this class into a union, so the destructor needs to be called manually anyways");

#endif // SEGMENTED_TENSOR_H
