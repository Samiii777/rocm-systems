/* Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include <gtest/gtest.h>

// Models the Device::virtualAlloc contract that both the PAL (paldevice.cpp)
// and ROCm (rocdevice.cpp) backends must honor: when the underlying virtual
// buffer cannot be created (VA space exhausted) CreateVirtualBuffer returns
// nullptr, and virtualAlloc must propagate nullptr instead of dereferencing
// it. The HIP layer (hip_vm.cpp hipMemAddressReserve) then turns a null base
// into hipErrorOutOfMemory rather than crashing the process.
namespace {

struct FakeMemory {
  void* svm_ptr;
  void* getSvmPtr() { return svm_ptr; }
};

// Mirrors the fixed virtualAlloc body: guard the null return before use.
void* virtualAllocModel(FakeMemory* mem) {
  if (mem == nullptr) {
    return nullptr;
  }
  return mem->getSvmPtr();
}

}  // namespace

TEST(VirtualAllocNullGuard, ReturnsNullWhenBufferCreationFails) {
  EXPECT_EQ(virtualAllocModel(nullptr), nullptr);
}

TEST(VirtualAllocNullGuard, ReturnsSvmPtrOnSuccess) {
  int marker = 0;
  FakeMemory mem{&marker};
  EXPECT_EQ(virtualAllocModel(&mem), &marker);
}