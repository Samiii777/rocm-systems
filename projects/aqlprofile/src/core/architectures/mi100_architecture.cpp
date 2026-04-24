// MIT License
//
// Copyright (c) 2017-2025 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "core/architectures/mi100_architecture.hpp"
#include "gfxip/gfx9/gfx9_block_table.h"

namespace aql_profile {

Mi100Architecture::Mi100Architecture(const AgentInfo* agent_info) {
  InitializeConfig(agent_info);
  InitializeRegisterSchema();
  InitializeBlockTable();
}

void Mi100Architecture::InitializeConfig(const AgentInfo* agent_info) {
  // Call base class initialization
  Gfx9Architecture::InitializeConfig(agent_info);

  // MI100-specific overrides
  config_.name = "MI100";
  config_.has_spm_core1 = true;        // Dual-core SPM support
  config_.spm_sample_delay_max = 0x34;

  // MI100 typically has 120 CUs
  // Agent info should provide actual values
}

void Mi100Architecture::InitializeBlockTable() {
  // Use MI100-specific block table if available, otherwise use base GFX9
  // In the actual implementation, this would reference gfx908-specific blocks
  extern const GpuBlockInfo* gfx908_block_table[AQLPROFILE_BLOCKS_NUMBER];
  block_table_ = gfx908_block_table;
  block_count_ = AQLPROFILE_BLOCKS_NUMBER;
}

}  // namespace aql_profile
