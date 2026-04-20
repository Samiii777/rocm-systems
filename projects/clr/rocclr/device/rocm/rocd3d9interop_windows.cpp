/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "rocdevice.hpp"
#include "rocmemory.hpp"
#include "rocd3d9interop.hpp"

#ifdef _WIN32

#include <D3D9.h>

#include "DxxOpenCLInteropExt.h"
#include "platform/interop_d3d9.hpp"

namespace amd {
namespace roc {
namespace D3D9Interop {

/**
 * @brief Query GPU mask from D3D9 device using AMD DXX extensions
 *
 * Loads atidxx64.dll (or atidxx32.dll) and uses AMD DirectX extension
 * interface to query which GPU(s) in a multi-GPU chain can interoperate
 * with the given D3D9 device.
 *
 * @param pd3d9Device D3D9Ex device
 * @param pd3d9DeviceGPUMask Output bitmask of GPU indices
 * @return true if GPU mask was successfully queried
 */
static bool queryD3D9DeviceGPUMask(IDirect3DDevice9Ex* pd3d9Device, UINT* pd3d9DeviceGPUMask) {
  IAmdDxExt* pExt = nullptr;
  IAmdDxExtCLInterop* pCLExt = nullptr;
  PFNAmdDxExtCreate9 AmdDxExtCreate9;
  HRESULT hr = S_OK;

// Get a handle to the DXX DLL with extension API support
#if defined _WIN64
  static constexpr CHAR dxxModuleName[13] = "atidxx64.dll";
#else
  static constexpr CHAR dxxModuleName[13] = "atidxx32.dll";
#endif

  HMODULE hDLL = GetModuleHandle(dxxModuleName);

  if (hDLL == nullptr) {
    hr = E_FAIL;
  }

  // Get the exported AmdDxExtCreate9() function pointer
  if (SUCCEEDED(hr)) {
    AmdDxExtCreate9 =
        reinterpret_cast<PFNAmdDxExtCreate9>(GetProcAddress(hDLL, "AmdDxExtCreate9"));
    if (AmdDxExtCreate9 == nullptr) {
      hr = E_FAIL;
    }
  }

  // Create the extension object
  if (SUCCEEDED(hr)) {
    hr = AmdDxExtCreate9(pd3d9Device, &pExt);
  }

  // Get the extension version information
  if (SUCCEEDED(hr)) {
    AmdDxExtVersion extVersion;
    hr = pExt->GetVersion(&extVersion);

    if (extVersion.majorVersion == 0) {
      hr = E_FAIL;
    }
  }

  // Get the OpenCL Interop interface
  if (SUCCEEDED(hr)) {
    pCLExt = static_cast<IAmdDxExtCLInterop*>(pExt->GetExtInterface(AmdDxExtCLInteropID));
    if (pCLExt != nullptr) {
      // Get the GPU mask using the CL Interop extension.
      pCLExt->QueryInteropGpuMask(pd3d9DeviceGPUMask);
    } else {
      hr = E_FAIL;
    }
  }

  if (pCLExt != nullptr) {
    pCLExt->Release();
  }

  if (pExt != nullptr) {
    pExt->Release();
  }

  return (SUCCEEDED(hr));
}

bool associateD3D9Device(const Device* device, IDirect3DDevice9Ex* pd3d9Device) {
  if (!device || !pd3d9Device) {
    return false;
  }

  // Verify device has valid LUID
  if (!device->hasValidLUID()) {
    LogError("ROCr device does not have valid LUID for D3D9 interop");
    return false;
  }

  // D3D9Ex provides GetAdapterLUID() directly (no DXGI)
  LUID adapterLuid;
  HRESULT hr = pd3d9Device->GetAdapterLUID(&adapterLuid);
  if (FAILED(hr)) {
    LogError("Failed to get adapter LUID from D3D9 device");
    return false;
  }

  // Stage 1: match the adapter LUID
  bool canInteroperate =
      (device->getDeviceLUID().HighPart == adapterLuid.HighPart) &&
      (device->getDeviceLUID().LowPart == adapterLuid.LowPart);

  // Stage 2: match the GPU chain ID using DXX extension
  if (canInteroperate) {
    UINT d3d9DeviceGPUMask = 0;
    UINT chainBitMask = 1 << device->getGpuIndex();

    if (queryD3D9DeviceGPUMask(pd3d9Device, &d3d9DeviceGPUMask)) {
      canInteroperate = (chainBitMask & d3d9DeviceGPUMask) != 0;
    } else {
      // Special handling for Intel iGPU + AMD dGPU in LDA mode
      if (chainBitMask > 1) {
        canInteroperate = false;
      }
      // If we're GPU 0 and can't query mask, assume it's OK
    }
  }

  if (!canInteroperate) {
    LogError("D3D9 device and ROCr device cannot interoperate (LUID or GPU mask mismatch)");
  }

  return canInteroperate;
}

void dissociateD3D9Device(const Device* device) {
  // Currently no cleanup needed
  // Future: may need to track associated devices and release resources
}

bool Export(const Memory* memory, IDirect3DSurface9* d3d9Surface,
            hsa_handle_t* handle, int* offset) {
  if (!memory || !d3d9Surface || !handle || !offset) {
    return false;
  }

  HRESULT hr = S_OK;
  IDirect3DResource9* pResource = nullptr;
  HANDLE hShared = nullptr;

  // Get the parent resource (texture/rendertarget) that contains this surface
  hr = d3d9Surface->GetContainer(__uuidof(IDirect3DResource9), (void**)&pResource);
  if (FAILED(hr) || !pResource) {
    LogError("Failed to get container resource from D3D9 surface");
    return false;
  }

  // For D3D9, shared handles must be queried from textures created with D3DPOOL_DEFAULT
  // Try to get shared handle - requires the resource was created with appropriate flags
  IDirect3DTexture9* pTexture = nullptr;
  hr = pResource->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&pTexture);

  if (SUCCEEDED(hr) && pTexture) {
    // D3D9Ex provides GetSharedHandle on shared textures
    // Note: This requires the texture was created with the sharing flag
    hr = pTexture->GetSharedHandle(&hShared);
    pTexture->Release();

    if (FAILED(hr) || !hShared) {
      pResource->Release();
      LogError("Failed to get shared handle from D3D9 texture (not created with sharing flag?)");
      return false;
    }
  } else {
    pResource->Release();
    LogError("D3D9 surface container is not a texture");
    return false;
  }

  pResource->Release();

  // Cast to HSA handle (platform-specific handle type)
  *handle = reinterpret_cast<hsa_handle_t>(hShared);
  *offset = 0;  // D3D resources are typically zero-offset

  return true;
}

}  // namespace D3D9Interop
}  // namespace roc
}  // namespace amd

#endif  // _WIN32
