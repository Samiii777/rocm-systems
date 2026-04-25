// Copyright (c) 2025-2026 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

/// @file instruction_lowering.h
/// @brief Lowering functions for instructions that don't exist on the target ISA.
///
/// @details When the legalization table marks an instruction as Action::Expand,
/// the binary translator calls try_lower_expand() to obtain the replacement
/// instruction sequence. Each lowering function decodes the guest instruction's
/// operands and emits the equivalent host instruction words.
///
/// Adding a new lowering: implement the function, add it to the dispatch in
/// try_lower_expand(). No changes needed in the binary translator's main loop.

#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "rocjitsu/code/dbt/instruction_builder.h"
#include "rocjitsu/code/rj_code.h"
#include "rocjitsu/isa/arch/amdgpu/cdna4/machine_insts.h"
#include "rocjitsu/isa/instruction.h"

namespace rocjitsu {

namespace {

/// @brief Lower CDNA4 v_lshl_add_u64 to a 64-bit add via carry chain.
///
/// v_lshl_add_u64 vdst, src0, src1, src2 = (src0 << src1) + src2
/// When src1 is literal 0 (shift=0), this is a 64-bit add: vdst = src0 + src2.
/// Lowered to: v_add_co_u32 + s_wait_alu + v_add_co_ci_u32.
std::vector<uint32_t> lower_v_lshl_add_u64(const Instruction &inst,
                                           [[maybe_unused]] rj_code_arch_t host_arch) {
  const auto *raw = inst.raw_encoding();
  if (!raw || inst.size() < 8)
    return {};

  cdna4::Vop3MachineInst src{};
  std::memcpy(&src, raw, sizeof(src));
  const uint16_t vdst = src.vdst;
  const uint16_t src0 = src.src0;
  const uint16_t src2 = src.src2;

  constexpr uint16_t VCC_LO = 106;
  std::vector<uint32_t> words;

  // v_add_co_u32 vdst_lo, vcc_lo, src0_lo, src2_lo  (VOP3_SDST_ENC)
  {
    const uint32_t w0 = (0x35u << 26) | (768u << 16) | (VCC_LO << 8) | vdst;
    const uint32_t w1 = static_cast<uint32_t>(src0) | (static_cast<uint32_t>(src2) << 9);
    words.push_back(w0);
    words.push_back(w1);
  }

  // s_wait_alu 0xFFFD — required before reading vcc from prior VALU on RDNA3+.
  words.push_back(pack_sopp(8, 0xFFFD));

  // v_add_co_ci_u32 vdst_hi, vcc_lo, src0_hi, src2_hi, vcc_lo
  {
    const uint32_t w0 = (0x35u << 26) | (288u << 16) | (VCC_LO << 8) | (vdst + 1);
    const uint32_t w1 = static_cast<uint32_t>(src0 + 1) | (static_cast<uint32_t>(src2 + 1) << 9) |
                        (static_cast<uint32_t>(VCC_LO) << 18);
    words.push_back(w0);
    words.push_back(w1);
  }

  return words;
}

} // namespace

/// @brief Try to lower an instruction marked as Action::Expand.
/// @returns Replacement instruction words on success, empty vector if unhandled.
[[nodiscard]] inline std::vector<uint32_t> try_lower_expand(const Instruction &inst,
                                                            [[maybe_unused]] rj_code_arch_t guest,
                                                            rj_code_arch_t host) {
  if (std::string_view(inst.mnemonic()) == "v_lshl_add_u64")
    return lower_v_lshl_add_u64(inst, host);
  return {};
}

} // namespace rocjitsu
