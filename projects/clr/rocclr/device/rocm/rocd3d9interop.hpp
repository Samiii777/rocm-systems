/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifndef ROC_D3D9_INTEROP_HPP_
#define ROC_D3D9_INTEROP_HPP_

#include "top.hpp"

#ifdef _WIN32

#include <d3d9.h>

namespace amd {
namespace roc {

// Forward declarations
class Device;
class Memory;

namespace D3D9Interop {

/**
 * @brief Validate D3D9 device matches ROCr GPU device
 *
 * Performs two-stage validation:
 * 1. LUID matching via IDirect3DDevice9Ex::GetAdapterLUID()
 * 2. GPU mask matching via AMD DXX extensions (for multi-GPU/LDA)
 *
 * @param device ROCr device to validate against
 * @param d3d9Device D3D9Ex device to associate
 * @return true if devices can interoperate, false otherwise
 */
bool associateD3D9Device(
    const Device* device,
    IDirect3DDevice9Ex* d3d9Device
);

/**
 * @brief Dissociate D3D9 device from ROCr device
 *
 * Cleanup function called during context destruction
 *
 * @param device ROCr device
 */
void dissociateD3D9Device(const Device* device);

/**
 * @brief Export D3D9 surface to HSA handle for interop
 *
 * Extracts shared handle from D3D9 surface and returns it
 * as HSA handle for memory mapping
 *
 * @param memory ROCr memory object
 * @param d3d9Surface D3D9 surface to export
 * @param handle Output HSA handle
 * @param offset Output offset into resource
 * @return true if export succeeded, false otherwise
 */
bool Export(
    const Memory* memory,
    IDirect3DSurface9* d3d9Surface,
    hsa_handle_t* handle,
    int* offset
);

}  // namespace D3D9Interop
}  // namespace roc
}  // namespace amd

#endif  // _WIN32

#endif  // ROC_D3D9_INTEROP_HPP_
