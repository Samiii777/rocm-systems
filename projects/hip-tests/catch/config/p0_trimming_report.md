# P0 Test Trimming Report — `users/iassiour/level_params`

**Date:** 2026-04-23
**Branch:** `users/iassiour/level_params` on `ROCm/rocm-systems`
**Base:** `develop`

## Overview

31 P0 test cases (33 CTest entries) trimmed across 25 source files
using `isQuickLevel()` gating. One test (`Unit_hipMemsetDSync`) is a
`TEMPLATE_TEST_CASE` that expands to 3 CTest entries (int8_t, int16_t,
uint32_t).

At `HIP_TEST_LEVEL=level_0`, tests use reduced parameters for faster
execution. At `level_2` (default), original behavior is preserved.

---

## Commit 1: Trim 8 P0 tests for level_0

| Test | Change | File |
|---|---|---|
| Unit_hipStreamEndCapture_Positive_GraphDestroy | N: 1M to 10K | graph/hipStreamEndCapture.cc |
| Unit_hipStreamIsCapturing_Positive_Basic | N: 1M to 10K | graph/hipStreamIsCapturing.cc |
| Unit_hipStreamBeginCapture_Positive_Basic | N: 1M to 10K | graph/hipStreamBeginCapture.cc |
| Unit_hipMemPoolTrimTo_Positive_Basic | N: 1M to 4K | memory/hipMemPoolTrimTo.cc |
| Unit_hipMemset2DAsync_BasicFunctional | rows/cols: drop 100 from GENERATE | memory/hipMemset2D.cc |
| Unit_hipMalloc3D_Basic | dims: 64MB to 10x10x10 | memory/hipMalloc3D.cc |
| Unit_hipGetProcAddress_ValidateDeviceApis | array size: 20 to 13 | device/hipGetProcAddressDevMgmt.cc |
| Unit_hipStreamCreateWithPriority_FunctionalForAllPriorities | MEMCPYSIZE: 64MB/1MB to 100KB | stream/hipStreamCreateWithPriority.cc |

---

## Commit 2: Optimize 8 slow P0 tests (>600s)

| Test | Runtime | Optimization |
|---|---|---|
| Unit_hipStreamLegacy_WithKernel | 3556s | Skip 2nd kernel launch (2 to 1) |
| Unit_hipMultiStream_sameDevice | 2189s | Reduce num_streams 8 to 1 |
| Unit_hipEventCreateWithFlags_DefaultFlg_HstVisMem | 1657s | Reduce iterations 5 to 1 |
| Unit_hipGridLaunch | 1655s | Reduce buffer 4MB to 1KB |
| Unit_hipEventDestroy_Unfinished | 1598s | Reduce delay 1000ms to 100ms |
| Unit_hipTestHalf | 927s | Skip half2 + functional sections |
| Unit_hipMemsetAsync_SetMemoryWithOffset | 792s | Use small buffer only |
| Unit_pown_Verification | 688s | Reduce 5 to 2 kernel launches |

---

## Commit 3: Trim 15 P0 timeout tests

### Delay reductions

| Test | Before | After |
|---|---|---|
| Unit_hipEventQuery_DifferentDevice | 3000ms | 100ms |
| Unit_hipStreamDestroy_WithPendingWork | 500ms | 50ms |
| Unit_hipStreamSynchronize_FinishWork | 500ms | 50ms |
| Unit_hipStreamSynchronize_NullStreamSynchronization | 1000ms/2000ms | 100ms |
| Unit_hipStreamWaitEvent_Default | 2000ms | 100ms |

### Iteration/section reductions

| Test | Before | After |
|---|---|---|
| Unit_fp16_arith | 100 iters | 2 |
| Unit_fp162_arith | 100 iters | 2 |
| Unit_hipHostMalloc_CoherentTst | 3 sections | 1 |
| Unit_deviceAllocation_Malloc_PerThread_Graph | 5 types | 1 (int32) |
| Unit_deviceAllocation_New_PerThread_Graph | 5 types | 1 (int32) |

### GENERATE/combinatorial reductions

| Test | Before | After |
|---|---|---|
| Unit_hipMemsetSync | 4 alloc types x 2 widths | 1 x 1 |
| Unit_hipMemsetDSync (x3 types) | 4 allocs x 2 widths | 1 x 1 |
| Unit_hipMemset2DSync | 4 allocs x 2w x 2h | 1 x 1 x 1 |
| Unit_hipMemset3DSync | 4 allocs x 2^3 dims | 1 x 1 x 1 x 1 |
| Unit_hipMemcpyAsync_Positive_Basic | 3 streams | 1 |

---

## Summary

| Metric | Count |
|---|---|
| P0 test cases trimmed | 31 |
| CTest entries trimmed | 33 (includes 3 from template) |
| Source files modified | 25 |

**Not optimized:**
- Unit_abs_int64_Verification (659s) — single kernel, inherent overhead
- Unit_hipMemcpy_Positive_Basic — single memcpy, inherent overhead
- Unit_hipMemcpy2DAsync_Positive_Basic — single memcpy, inherent overhead
