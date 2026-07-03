// Copyright (c) 2026 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT
//
// Driver program for the early-mmap preload regression test (rocm-systems
// #8037). It is intended to be launched with:
//
//   LD_PRELOAD="<early_mmap_preload.so> <librocjitsu_kmd.so>" early_mmap_preload_main
//
// so that early_mmap_preload's constructor calls mmap() before the KMD shim's
// constructor has resolved its libc passthrough symbols. If the shim crashes on
// the early mmap, this process dies before reaching main() and the test fails.
// Otherwise the constructor sets a flag we verify here.

#include <cstdio>

// Provided by early_mmap_preload.so.
extern "C" int rj_early_mmap_succeeded();

int main() {
  if (!rj_early_mmap_succeeded()) {
    fprintf(stderr,
            "early_mmap_preload_main: early mmap constructor did not report "
            "success\n");
    return 1;
  }
  printf("early_mmap_preload_main: reached main() after early mmap OK\n");
  return 0;
}
