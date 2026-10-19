#pragma once
// ============================================================================
// buffer_extraction.h — Depth buffer & Motion Vector extraction for FH2 / Azimuth
// ============================================================================
#include <d3d12.h>
#include <cstdint>

namespace BufferExtraction {

// Initialize staging resources and install ID3D12Device & ID3D12GraphicsCommandList hooks
bool Initialize(ID3D12Device* pDevice);

// Update active device pointer (when real game device is acquired)
void SetDevice(ID3D12Device* pDevice);

// Frame boundary reset (called on Present)
void ResetFrameState();

// Extracted GPU resources ready for DLSS/NGX evaluation
ID3D12Resource* GetDepthBuffer();
ID3D12Resource* GetMotionVectors();
ID3D12Resource* GetPreTonemapColor();

// Monitor submissions from CommandQueue::ExecuteCommandLists
void OnExecuteCommandLists(
    ID3D12CommandQueue*       pQueue,
    UINT                      numCommandLists,
    ID3D12CommandList* const* ppCommandLists
);

// Tear down hooks and release captured resources
void Shutdown();

} // namespace BufferExtraction
