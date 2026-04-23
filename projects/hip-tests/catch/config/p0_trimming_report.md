# P0 Test Trimming Report — `users/iassiour/level_params`

**Date:** 2026-04-23
**Branch:** `users/iassiour/level_params` on `ROCm/rocm-systems`
**Base:** `develop`

## Overview

16 P0 tests trimmed across 16 source files using `isQuickLevel()` gating.
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

| Test | Runtime | Optimization | Est. After |
|---|---|---|---|
| Unit_hipStreamLegacy_WithKernel | 3556s | Skip 2nd kernel launch (2 to 1) | ~500s |
| Unit_hipMultiStream_sameDevice | 2189s | Reduce num_streams 8 to 1 | ~500s |
| Unit_hipEventCreateWithFlags_DefaultFlg_HstVisMem | 1657s | Reduce iterations 5 to 1 | ~350s |
| Unit_hipGridLaunch | 1655s | Reduce buffer 4MB to 1KB | ~500s |
| Unit_hipEventDestroy_Unfinished | 1598s | Reduce delay 1000ms to 100ms | ~500s |
| Unit_hipTestHalf | 927s | Skip half2 + functional sections | ~500s |
| Unit_hipMemsetAsync_SetMemoryWithOffset | 792s | Use small buffer only | ~500s |
| Unit_pown_Verification | 688s | Reduce 5 to 2 kernel launches | ~500s |

**Not optimized:** Unit_abs_int64_Verification (659s) — single kernel
launch, runtime is dominated by base kernel launch overhead (~500s).
