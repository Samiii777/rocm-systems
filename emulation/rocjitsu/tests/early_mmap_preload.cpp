// Copyright (c) 2026 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT
//
// Regression helper for the "early mmap during library initialization" crash
// (rocm-systems #8037).
//
// This is a tiny shared library whose constructor calls mmap() during its own
// static initialization. When it is preloaded *before* librocjitsu_kmd
// (rocjitsu_shared) so that its constructor runs first, the mmap() call is
// routed to the interposed mmap() in the KMD shim while the shim's own
// constructor has not yet run LibcPassthrough::resolve(). Historically that
// dereferenced a null real.mmap function pointer and crashed. The interposer
// now falls back to a raw SYS_mmap in that window, so this must complete
// cleanly.
//
// The constructor is given a low priority number so it runs as early as
// possible, mimicking the OpenBLAS worker-thread / loader init path from the
// bug report.

#include <cstdio>
#include <cstdlib>

#include <sys/mman.h>
#include <unistd.h>

namespace {

// Runs a mmap()/munmap() round trip. Reported via a global flag so the driver
// program can assert success.
bool g_early_mmap_ok = false;

__attribute__((constructor(101))) void rj_early_mmap_ctor() {
  void *p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fprintf(stderr, "early_mmap_preload: mmap failed during constructor\n");
    return;
  }
  // Touch the mapping to ensure it is actually usable memory.
  *static_cast<volatile char *>(p) = 42;
  if (munmap(p, 4096) != 0) {
    fprintf(stderr, "early_mmap_preload: munmap failed during constructor\n");
    return;
  }
  g_early_mmap_ok = true;
  fprintf(stderr, "early_mmap_preload: early mmap round trip OK (%p)\n", p);
}

} // namespace

// Exported so the driver program can confirm the constructor ran and succeeded.
extern "C" int rj_early_mmap_succeeded() { return g_early_mmap_ok ? 1 : 0; }
