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

#include "core/architectures/mi300_architecture.hpp"
#include "gfxip/gfx9/gfx9_block_table.h"
#include "def/gpu_block_info.h"

namespace aql_profile {

Mi300Architecture::Mi300Architecture(const AgentInfo* agent_info) {
  InitializeConfig(agent_info);
  InitializeRegisterSchema();
  InitializeBlockTable();
}

void Mi300Architecture::InitializeConfig(const AgentInfo* agent_info) {
  // Call base class initialization
  Gfx9Architecture::InitializeConfig(agent_info);

  // MI300-specific overrides
  config_.name = "MI300";
  config_.xcc_count = agent_info->xcc_num;  // Typically 8
  config_.aid_count = 4;                     // MI300 has 4 AIDs

  // MI300-specific features
  config_.has_aid_aware_counters = true;  // Counters route through AIDs
  config_.has_spm_core1 = false;          // MI300 uses different SPM approach

  // MI300 typically has 304 CUs (MI300X) or 228 CUs (MI300A)
  // Agent info provides actual values
}

void Mi300Architecture::InitializeBlockTable() {
  // Use MI300-specific block table (gfx940)
  extern const GpuBlockInfo* gfx940_block_table[AQLPROFILE_BLOCKS_NUMBER];
  block_table_ = gfx940_block_table;
  block_count_ = AQLPROFILE_BLOCKS_NUMBER;
}

size_t Mi300Architecture::GetBytesNeededForBlock(uint32_t block_id) const {
  const GpuBlockInfo* block_info = GetBlockInfo(block_id);
  if (!block_info) return 0;

  // For AID-aware blocks (e.g., UMC), calculation is different
  if (block_info->attr & CounterBlockAidAttr) {
    // AID blocks: instance count already accounts for distribution across AIDs
    // Only need space for the instances, not multiplied by XCC count
    return block_info->instance_count * sizeof(uint64_t);
  }

  // For non-AID blocks, use standard calculation
  return HardwareArchitecture::GetBytesNeededForBlock(block_id);
}

}  // namespace aql_profile
