/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

/* Test Case Description:
   Stress test for memory pool allocations. Allocates blocks from 1% to 50%
   of a memory budget in 2% increments, verifies pool attributes
   (UsedMemCurrent, ReservedMemCurrent) after all allocations, then frees
   all blocks and verifies memory returns to zero. On integrated devices, where
   device memory is host RAM, the budget is capped at 1/8 of host memory.
*/

#include <hip_test_common.hh>
#include <hip_test_helper.hh>

#include <algorithm>

HIP_TEST_CASE(Unit_hipMemPoolMaxAlloc) {
  int device = 0;
  HIP_CHECK(hipSetDevice(device));

  hipMemPool_t pool;
  HIP_CHECK(hipDeviceGetDefaultMemPool(&pool, device));

  std::size_t free{}, total{};
  HIP_CHECK(hipMemGetInfo(&free, &total));
  std::size_t memBudget = std::min(total, free);

  hipDeviceProp_t prop{};
  HIP_CHECK(hipGetDeviceProperties(&prop, device));
  if (prop.integrated) {
    const std::size_t hostMemMB = HipTest::getTotalSystemMemoryInMB();
    if (hostMemMB == 0) {
      HIP_SKIP_TEST("total system memory could not be queried.");
    }
    memBudget = std::min(memBudget, hostMemMB / 8 * 1024 * 1024);
  }

  hipStream_t stream = nullptr;

  constexpr int kStartPct = 1;
  constexpr int kEndPct = 50;
  constexpr int kStepPct = 2;
  constexpr int kMaxAllocs = (kEndPct - kStartPct) / kStepPct + 1;
  const std::size_t memLimit = (memBudget / 100) * 60;

  struct PoolState {
    hipMemPool_t pool;
    hipStream_t stream;
    uint64_t origThreshold = 0;
    void* ptrs[kMaxAllocs] = {};
    int numAllocs = 0;
    ~PoolState() {
      for (int i = 0; i < numAllocs; i++) {
        static_cast<void>(hipFreeAsync(ptrs[i], stream));
      }
      static_cast<void>(hipStreamSynchronize(stream));
      static_cast<void>(
          hipMemPoolSetAttribute(pool, hipMemPoolAttrReleaseThreshold, &origThreshold));
    }
  } state{pool, stream};

  HIP_CHECK(hipMemPoolGetAttribute(pool, hipMemPoolAttrReleaseThreshold, &state.origThreshold));
  uint64_t threshold = 0;
  HIP_CHECK(hipMemPoolSetAttribute(pool, hipMemPoolAttrReleaseThreshold, &threshold));

  std::size_t expectedTotal = 0;

  // Allocate all blocks, stop when cumulative usage would exceed 60% of memBudget
  for (int pct = kStartPct; pct <= kEndPct; pct += kStepPct) {
    std::size_t allocSize = (memBudget / 100) * pct;
    if (allocSize == 0 || expectedTotal + allocSize > memLimit) break;
    HIP_CHECK(hipMallocAsync(&state.ptrs[state.numAllocs], allocSize, stream));
    HIP_CHECK(hipStreamSynchronize(stream));
    expectedTotal += allocSize;
    state.numAllocs++;
  }
  REQUIRE(state.numAllocs > 0);

  // Verify pool reports expected usage after all allocations
  uint64_t usedMem = 0;
  uint64_t reservedMem = 0;
  HIP_CHECK(hipMemPoolGetAttribute(pool, hipMemPoolAttrUsedMemCurrent, &usedMem));
  HIP_CHECK(hipMemPoolGetAttribute(pool, hipMemPoolAttrReservedMemCurrent, &reservedMem));
  REQUIRE(usedMem >= expectedTotal);
  REQUIRE(reservedMem >= expectedTotal);

  // Free all blocks
  for (; state.numAllocs > 0; state.numAllocs--) {
    HIP_CHECK(hipFreeAsync(state.ptrs[state.numAllocs - 1], stream));
    HIP_CHECK(hipStreamSynchronize(stream));
  }

  // Verify pool reports zero usage after all frees
  HIP_CHECK(hipMemPoolGetAttribute(pool, hipMemPoolAttrUsedMemCurrent, &usedMem));
  HIP_CHECK(hipMemPoolGetAttribute(pool, hipMemPoolAttrReservedMemCurrent, &reservedMem));
  REQUIRE(usedMem == 0);
  REQUIRE(reservedMem == 0);
}
