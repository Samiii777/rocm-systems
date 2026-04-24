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

#ifndef SRC_CORE_HARDWARE_CONFIG_VALIDATION_HPP_
#define SRC_CORE_HARDWARE_CONFIG_VALIDATION_HPP_

#include "core/hardware_config.hpp"
#include <stdexcept>
#include <string>

namespace aql_profile {

/// Validate HardwareConfig for consistency and reasonable limits
/// Throws std::invalid_argument on validation failure
inline void ValidateHardwareConfig(const HardwareConfig& config) {
  // Validate basic topology
  if (config.se_count == 0) {
    throw std::invalid_argument("HardwareConfig: se_count must be > 0");
  }
  if (config.se_count > 32) {
    throw std::invalid_argument("HardwareConfig: se_count exceeds maximum (32)");
  }

  if (config.sa_per_se_count == 0) {
    throw std::invalid_argument("HardwareConfig: sa_per_se_count must be > 0");
  }
  if (config.sa_per_se_count > 8) {
    throw std::invalid_argument("HardwareConfig: sa_per_se_count exceeds maximum (8)");
  }

  if (config.cu_count == 0) {
    throw std::invalid_argument("HardwareConfig: cu_count must be > 0");
  }
  if (config.cu_count > 512) {
    throw std::invalid_argument("HardwareConfig: cu_count exceeds maximum (512)");
  }

  // Validate XCC configuration
  if (config.xcc_count == 0) {
    throw std::invalid_argument("HardwareConfig: xcc_count must be >= 1");
  }
  if (config.xcc_count > 16) {
    throw std::invalid_argument("HardwareConfig: xcc_count exceeds maximum (16)");
  }

  // SE count must be divisible by XCC count
  if (config.se_count % config.xcc_count != 0) {
    throw std::invalid_argument(
        "HardwareConfig: se_count must be divisible by xcc_count");
  }

  // Validate AID configuration
  if (config.aid_count == 0) {
    throw std::invalid_argument("HardwareConfig: aid_count must be >= 1");
  }
  if (config.aid_count > 8) {
    throw std::invalid_argument("HardwareConfig: aid_count exceeds maximum (8)");
  }

  // AID-aware counters only valid with multiple AIDs
  if (config.has_aid_aware_counters && config.aid_count <= 1) {
    throw std::invalid_argument(
        "HardwareConfig: aid_aware_counters requires aid_count > 1");
  }

  // WGP count validation (GFX10+)
  if (config.wgp_count > 256) {
    throw std::invalid_argument("HardwareConfig: wgp_count exceeds maximum (256)");
  }

  // For architectures with WGP, it should approximate CU/2
  if (config.wgp_count > 0) {
    uint32_t expected_wgp = config.cu_count / 2;
    // Allow some variance for asymmetric configurations
    if (config.wgp_count > expected_wgp * 2 || expected_wgp > config.wgp_count * 2) {
      throw std::invalid_argument(
          "HardwareConfig: wgp_count inconsistent with cu_count (expected ~CU/2)");
    }
  }

  // Validate capability flags
  if (config.has_spm_core1 && (config.xcc_count > 1 || config.aid_count > 1)) {
    throw std::invalid_argument(
        "HardwareConfig: spm_core1 not supported on multi-XCC/AID");
  }
}

/// Check if config is safe for buffer size calculations
/// Returns true if config values won't cause overflow in GetBytesNeededForBlock
inline bool IsConfigSafeForBufferCalc(const HardwareConfig& config) {
  // Check if SE * SA * WGP * XCC could overflow
  // Maximum safe value per dimension to prevent overflow
  constexpr uint64_t MAX_DIM = 256;

  if (config.se_count > MAX_DIM) return false;
  if (config.sa_per_se_count > MAX_DIM) return false;
  if (config.GetTotalWGPs() > MAX_DIM) return false;
  if (config.xcc_count > MAX_DIM) return false;

  // Check total product doesn't overflow
  uint64_t total = static_cast<uint64_t>(config.se_count) *
                   static_cast<uint64_t>(config.sa_per_se_count) *
                   static_cast<uint64_t>(config.GetTotalWGPs()) *
                   static_cast<uint64_t>(config.xcc_count);

  // Total events per block should be < 1M (reasonable upper bound)
  return total < 1000000;
}

}  // namespace aql_profile

#endif  // SRC_CORE_HARDWARE_CONFIG_VALIDATION_HPP_
