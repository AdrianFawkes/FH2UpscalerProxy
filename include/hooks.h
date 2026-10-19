#pragma once
// ============================================================================
// hooks.h — MinHook-based vtable hooks for DXGI / D3D12
// ============================================================================
#include <d3d12.h>
#include <dxgi1_4.h>

namespace Hooks {

// Install hooks on Present, ResizeBuffers, and ExecuteCommandLists.
// Call this after creating a dummy swap chain to extract vtable addresses.
bool Install(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* pQueue);

// Remove all hooks and cleanup MinHook.
void Remove();

// Accessors for hooked objects (used by DLSS evaluation).
ID3D12Device*         GetDevice();
ID3D12CommandQueue*   GetCommandQueue();
IDXGISwapChain3*      GetSwapChain();
UINT                  GetBackBufferIndex();

} // namespace Hooks
