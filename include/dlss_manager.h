#pragma once
// ============================================================================
// dlss_manager.h — NVIDIA DLSS SDK (NGX) integration manager for DX12
// ============================================================================
#include <d3d12.h>
#include <dxgi1_4.h>

namespace DLSSManager {

// Supported operational modes
enum class Mode {
    DLAA = 0,       // Native resolution anti-aliasing (render res == display res)
    Quality,        // DLSS Quality (render at ~66% of display)
    Balanced,       // DLSS Balanced (render at ~58%)
    Performance,    // DLSS Performance (render at ~50%)
    UltraPerf       // DLSS Ultra Performance (render at ~33%)
};

// Initialize DLSS with the given device, queue, resolution, and format settings.
// renderWidth/Height = the resolution the game renders at BEFORE upscaling.
// displayWidth/Height = the final output resolution (swap chain size).
// displayFormat = the DXGI format of the swap chain back buffer (e.g. R8G8B8A8_UNORM).
bool Initialize(
    ID3D12Device*       pDevice,
    ID3D12CommandQueue* pQueue,
    UINT renderWidth,  UINT renderHeight,
    UINT displayWidth, UINT displayHeight,
    DXGI_FORMAT displayFormat = DXGI_FORMAT_UNKNOWN
);

// Called each frame in the Present hook to run the DLSS evaluation pass
// and copy the processed texture to the swap chain back buffer.
bool Evaluate();

// Check if DLSS is currently initialized
bool IsInitialized();

// Release all DLSS resources (call before device destruction or resize).
void Shutdown();

// Release intermediate resources on swap chain resize.
void OnResize();

// Mode control
void SetMode(Mode mode);
Mode GetMode();

// Sharpness control [0.0f .. 1.0f]
void SetSharpness(float sharpness);
float GetSharpness();

// Enable / Disable toggle
void SetEnabled(bool enabled);
bool IsEnabled();

// Trigger a one-frame temporal accumulation reset (e.g. camera cut)
void TriggerReset();

// Resolution queries
UINT GetRenderWidth();
UINT GetRenderHeight();
UINT GetDisplayWidth();
UINT GetDisplayHeight();

// Provide per-frame input resources (set externally by BufferExtraction).
void SetColorInput(ID3D12Resource* pColor);
void SetDepthInput(ID3D12Resource* pDepth);
void SetMotionVectorInput(ID3D12Resource* pMV);
void SetExposureInput(ID3D12Resource* pExposure);  // optional

// Queries for UI / diagnostics
bool HasColorInput();
bool HasDepthInput();
bool HasMotionVectorInput();

// Get the DLSS output texture (upscaled / DLAA result).
ID3D12Resource* GetOutput();

} // namespace DLSSManager
