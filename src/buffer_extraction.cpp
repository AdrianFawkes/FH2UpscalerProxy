// ============================================================================
// buffer_extraction.cpp — Motion Vector & Depth Buffer Extraction for FH2 (Azimuth)
// ============================================================================
//
// ARCHITECTURE OVERVIEW:
// ----------------------
// NVIDIA DLSS (Super Resolution & DLAA) requires three main inputs per frame:
//   1. Color Buffer: Pre-tonemap HDR scene color (or back buffer)
//   2. Depth Buffer: Non-linear or linear Z-buffer at render resolution
//   3. Motion Vectors: 2D screen-space pixel velocity vectors
//
// In Forza Horizon 2 (running via the Azimuth Xbox One emulator / D3D12 layer),
// motion vectors are computed during the game's native motion-blur pass.
// This shader combines camera transformation matrices (Current vs. Previous
// View-Projection) and object velocities to generate full-resolution velocity
// vectors before the final post-processing and tonemapping passes.
//
// To extract these buffers without game crash or race conditions, we intercept:
//   - ID3D12Device::CreateGraphicsPipelineState & CreateComputePipelineState
//       -> Computes a 64-bit FNV-1a hash of the shader bytecode (PS/CS).
//   - ID3D12GraphicsCommandList::SetPipelineState
//       -> Identifies the active pass (Depth Prepass, Motion Blur, Tonemap).
//   - ID3D12GraphicsCommandList::ResourceBarrier
//       -> Tracks resource states, identifying candidate render targets & depth.
//   - ID3D12GraphicsCommandList::DrawIndexedInstanced / DrawInstanced / Dispatch
//       -> Executes the original draw call, then immediately performs a safe
//          GPU-side CopyResource into dedicated staging textures.
//   - Hand-off to DLSSManager::SetDepthInput() & SetMotionVectorInput().
//
// ============================================================================

#include "buffer_extraction.h"
#include "dlss_manager.h"
#include "logger.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <MinHook.h>

#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================================
// SHADER HASH CONFIGURATION & RENDERDOC / PIX GUIDE
// ============================================================================
// HOW TO FIND THESE HASHES USING RENDERDOC OR PIX:
// ------------------------------------------------
// 1. Launch Azimuth with RenderDoc (e.g. inject into azimuth.exe) or PIX.
// 2. In Forza Horizon 2, enter freeroam or a race, and trigger a frame capture (F12).
// 3. In the Event Browser:
//    - Look for the early depth pass (typically draws car bodies, terrain, roads with no color RTV).
//    - Look for the Motion Blur pass (typically named "MotionBlur", "Velocity", "TileMax",
//      or a full-screen draw/dispatch occurring between lighting compositing and tonemapping).
// 4. In RenderDoc, select the Draw Call or Dispatch:
//    - Click the "Pipeline State" tab.
//    - Select "Pixel Shader" (PS) or "Compute Shader" (CS).
//    - Note the shader details (RTV format, bytecode size, hash).
// 5. Alternatively, check "fh2_dlss_proxy.log" in the game directory!
//    Our proxy automatically computes and logs the 64-bit FNV-1a hash of EVERY
//    graphics and compute shader created by Azimuth upon boot and level load.
// 6. Paste the matching 64-bit hexadecimal hashes below:
// ============================================================================

// Concrete 64-bit FNV-1a hashes for FH2 Motion Blur Shaders (from azimuth game log)
static constexpr uint64_t kTargetMotionBlurShaderHash_PS1 = 0xb6ad8e59f720fac5ull;
static constexpr uint64_t kTargetMotionBlurShaderHash_PS2 = 0x07d8f91d820a6eacull;
static constexpr uint64_t kTargetMotionBlurShaderHash_CS  = 0x0000000000000000ull;

// [PLACEHOLDER] Target 64-bit FNV-1a hash for FH2 Depth Prepass Shader
static constexpr uint64_t kTargetDepthShaderHash_PS      = 0x0000000000000000ull;

// [PLACEHOLDER] Target 64-bit FNV-1a hash for FH2 Tonemapping Shader (for pre-tonemap color)
static constexpr uint64_t kTargetTonemapShaderHash_PS    = 0x0000000000000000ull;

namespace {

// ---------------------------------------------------------------------------
// 64-bit FNV-1a Hash Implementation for Shader Bytecode
// ---------------------------------------------------------------------------
uint64_t ComputeFNV1a64(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ull; // FNV offset basis
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(ptr[i]);
        hash *= 1099511628211ull;            // FNV prime
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Pipeline State Classification & Metadata
// ---------------------------------------------------------------------------
enum class PassType : uint8_t {
    Unknown = 0,
    DepthPrepass,
    MotionBlur,
    Tonemapping,
    LightingComposite
};

struct PsoMetadata {
    uint64_t    psHash{0};
    uint64_t    vsHash{0};
    uint64_t    csHash{0};
    DXGI_FORMAT dsvFormat{DXGI_FORMAT_UNKNOWN};
    UINT        numRTVs{0};
    DXGI_FORMAT rtvFormats[8]{DXGI_FORMAT_UNKNOWN};
    PassType    classifiedPass{PassType::Unknown};
};

// Thread-safe registry of Pipeline State Objects
static std::recursive_mutex                     g_psoMutex;
static std::unordered_map<void*, PsoMetadata>   g_psoRegistry;

// Resource tracking
struct TrackedResourceInfo {
    D3D12_RESOURCE_DESC   desc{};
    D3D12_RESOURCE_STATES currentState{D3D12_RESOURCE_STATE_COMMON};
    uint64_t              lastAccessSubmission{0};
};

static std::recursive_mutex                                           g_resourceMutex;
static std::unordered_map<ID3D12Resource*, TrackedResourceInfo>       g_trackedResources;

// Per-command-list tracking context
struct CmdListContext {
    PassType               activePass{PassType::Unknown};
    ID3D12PipelineState*   currentPso{nullptr};
    ID3D12Resource*        lastBoundDepth{nullptr};
    ID3D12Resource*        lastBoundVelocity{nullptr};
    ID3D12Resource*        lastBoundHdrColor{nullptr};
};

static std::recursive_mutex                                           g_cmdListContextMutex;
static std::unordered_map<ID3D12GraphicsCommandList*, CmdListContext> g_cmdListContexts;

// Global Extracted Staging Textures (owned by our proxy)
static ComPtr<ID3D12Resource> g_depthSnapshot;
static ComPtr<ID3D12Resource> g_motionVectorSnapshot;
static ComPtr<ID3D12Resource> g_colorSnapshot;

static std::atomic<bool> g_depthCapturedThisFrame{false};
static std::atomic<bool> g_mvCapturedThisFrame{false};
static std::atomic<bool> g_colorCapturedThisFrame{false};

static ID3D12Device* g_d3d12Device = nullptr;
static std::atomic<bool> g_hooksInstalled{false};

// ---------------------------------------------------------------------------
// Original Function Typedefs & Storage (for MinHook)
// ---------------------------------------------------------------------------
using PFN_SetPipelineState = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* pThis,
    ID3D12PipelineState*       pPipelineState
);

using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* pThis,
    UINT                       VertexCountPerInstance,
    UINT                       InstanceCount,
    UINT                       StartVertexLocation,
    UINT                       StartInstanceLocation
);

using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* pThis,
    UINT                       IndexCountPerInstance,
    UINT                       InstanceCount,
    UINT                       StartIndexLocation,
    INT                        BaseVertexLocation,
    UINT                       StartInstanceLocation
);

using PFN_Dispatch = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* pThis,
    UINT                       ThreadGroupCountX,
    UINT                       ThreadGroupCountY,
    UINT                       ThreadGroupCountZ
);

using PFN_ResourceBarrier = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*       pThis,
    UINT                             NumBarriers,
    const D3D12_RESOURCE_BARRIER*    pBarriers
);

using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*               pThis,
    UINT                                     NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE*       pRenderTargetDescriptors,
    BOOL                                     RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE*       pDepthStencilDescriptor
);

using PFN_CreateGraphicsPipelineState = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*                              pThis,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC*  pDesc,
    REFIID                                     riid,
    void**                                     ppPipelineState
);

using PFN_CreateComputePipelineState = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*                             pThis,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC*  pDesc,
    REFIID                                    riid,
    void**                                    ppPipelineState
);

static PFN_SetPipelineState            o_SetPipelineState            = nullptr;
static PFN_DrawInstanced               o_DrawInstanced               = nullptr;
static PFN_DrawIndexedInstanced        o_DrawIndexedInstanced        = nullptr;
static PFN_Dispatch                    o_Dispatch                    = nullptr;
static PFN_ResourceBarrier             o_ResourceBarrier             = nullptr;
static PFN_OMSetRenderTargets          o_OMSetRenderTargets          = nullptr;
static PFN_CreateGraphicsPipelineState o_CreateGraphicsPipelineState = nullptr;
static PFN_CreateComputePipelineState  o_CreateComputePipelineState  = nullptr;

// ---------------------------------------------------------------------------
// Helper: Format classification
// ---------------------------------------------------------------------------
bool IsDepthFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return true;
        default:
            return false;
    }
}

bool IsVelocityFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return true;
        default:
            return false;
    }
}

bool IsHdrColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Helper: Ensure staging snapshot texture exists and matches dimensions/format
// ---------------------------------------------------------------------------
bool EnsureSnapshotTexture(
    ID3D12Device*          pDevice,
    const D3D12_RESOURCE_DESC& srcDesc,
    DXGI_FORMAT            formatOverride,
    ComPtr<ID3D12Resource>& snapshotRes)
{
    if (!pDevice) pDevice = g_d3d12Device;
    if (!pDevice) return false;

    DXGI_FORMAT targetFormat = (formatOverride != DXGI_FORMAT_UNKNOWN) ? formatOverride : srcDesc.Format;

    if (snapshotRes) {
        D3D12_RESOURCE_DESC curDesc = snapshotRes->GetDesc();
        if (curDesc.Width == srcDesc.Width &&
            curDesc.Height == srcDesc.Height &&
            curDesc.Format == targetFormat)
        {
            return true; // Already valid
        }
        snapshotRes.Reset();
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment          = 0;
    desc.Width              = srcDesc.Width;
    desc.Height             = srcDesc.Height;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = targetFormat;
    desc.SampleDesc         = {1, 0};
    desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = pDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(snapshotRes.ReleaseAndGetAddressOf())
    );

    if (FAILED(hr)) {
        Log::ErrorF("Failed to create snapshot texture ({}x{}, format {})!",
                    srcDesc.Width, srcDesc.Height, static_cast<int>(targetFormat));
        return false;
    }

    Log::InfoF("Snapshot texture created: {}x{}, format {}",
               srcDesc.Width, srcDesc.Height, static_cast<int>(targetFormat));
    return true;
}

// ---------------------------------------------------------------------------
// Safe GPU copy: records transition barriers, CopyResource, and restores state
// ---------------------------------------------------------------------------
void RecordSafeCopy(
    ID3D12GraphicsCommandList* pCmdList,
    ID3D12Resource*            pSource,
    ID3D12Resource*            pDest,
    D3D12_RESOURCE_STATES      srcCurrentState)
{
    if (!pCmdList || !pSource || !pDest) return;

    // Transition Source -> COPY_SOURCE (if not already in that state)
    // Transition Dest   -> COPY_DEST
    std::vector<D3D12_RESOURCE_BARRIER> preBarriers;

    if (!(srcCurrentState & D3D12_RESOURCE_STATE_COPY_SOURCE)) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = pSource;
        b.Transition.StateBefore = srcCurrentState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        preBarriers.push_back(b);
    }

    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = pDest;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        preBarriers.push_back(b);
    }

    o_ResourceBarrier(pCmdList, static_cast<UINT>(preBarriers.size()), preBarriers.data());

    // Execute the GPU copy
    pCmdList->CopyResource(pDest, pSource);

    // Transition back:
    // Source -> srcCurrentState
    // Dest   -> NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE (DLSS-ready SRV state)
    std::vector<D3D12_RESOURCE_BARRIER> postBarriers;

    if (!(srcCurrentState & D3D12_RESOURCE_STATE_COPY_SOURCE)) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = pSource;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = srcCurrentState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers.push_back(b);
    }

    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = pDest;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers.push_back(b);
    }

    o_ResourceBarrier(pCmdList, static_cast<UINT>(postBarriers.size()), postBarriers.data());
}

// ---------------------------------------------------------------------------
// Hook: ID3D12Device::CreateGraphicsPipelineState
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hooked_CreateGraphicsPipelineState(
    ID3D12Device*                             pThis,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID                                    riid,
    void**                                    ppPipelineState)
{
    if (pThis && pThis != g_d3d12Device) {
        g_d3d12Device = pThis;
    }

    HRESULT hr = o_CreateGraphicsPipelineState(pThis, pDesc, riid, ppPipelineState);
    if (FAILED(hr) || !ppPipelineState || !*ppPipelineState || !pDesc) {
        return hr;
    }

    PsoMetadata meta{};
    meta.vsHash    = ComputeFNV1a64(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
    meta.psHash    = ComputeFNV1a64(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
    meta.dsvFormat = pDesc->DSVFormat;
    meta.numRTVs   = pDesc->NumRenderTargets;
    for (UINT i = 0; i < pDesc->NumRenderTargets && i < 8; ++i) {
        meta.rtvFormats[i] = pDesc->RTVFormats[i];
    }

    // Pass classification logic:
    // 1. Direct hash match
    if (meta.psHash == kTargetMotionBlurShaderHash_PS1 || meta.psHash == kTargetMotionBlurShaderHash_PS2) {
        meta.classifiedPass = PassType::MotionBlur;
        Log::InfoF("Identified MOTION BLUR PSO (PS Hash Match: {:#018x})", meta.psHash);
    } else if (kTargetDepthShaderHash_PS != 0 && meta.psHash == kTargetDepthShaderHash_PS) {
        meta.classifiedPass = PassType::DepthPrepass;
        Log::InfoF("Identified DEPTH PREPASS PSO (PS Hash Match: {:#018x})", meta.psHash);
    } else if (kTargetTonemapShaderHash_PS != 0 && meta.psHash == kTargetTonemapShaderHash_PS) {
        meta.classifiedPass = PassType::Tonemapping;
        Log::InfoF("Identified TONEMAP PSO (PS Hash Match: {:#018x})", meta.psHash);
    }
    // 2. Smart heuristics
    else if (meta.numRTVs == 1 && IsVelocityFormat(meta.rtvFormats[0])) {
        meta.classifiedPass = PassType::MotionBlur;
        Log::InfoF("Heuristic: Flagged candidate Motion Blur PSO [PS Hash: {:#018x}, RTV: {}]",
                   meta.psHash, static_cast<int>(meta.rtvFormats[0]));
    } else if (meta.numRTVs == 0 && IsDepthFormat(meta.dsvFormat)) {
        meta.classifiedPass = PassType::DepthPrepass;
        Log::InfoF("Heuristic: Flagged candidate Depth Prepass PSO [VS Hash: {:#018x}, DSV: {}]",
                   meta.vsHash, static_cast<int>(meta.dsvFormat));
    } else if (meta.numRTVs == 1 && meta.dsvFormat == DXGI_FORMAT_UNKNOWN &&
               (meta.rtvFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM ||
                meta.rtvFormats[0] == DXGI_FORMAT_R10G10B10A2_UNORM ||
                meta.rtvFormats[0] == DXGI_FORMAT_B8G8R8A8_UNORM)) {
        Log::InfoF("Candidate Tonemap/PostProcess PSO [PS: {:#018x}, VS: {:#018x}, RTV: {}]",
                   meta.psHash, meta.vsHash, static_cast<int>(meta.rtvFormats[0]));
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_psoMutex);
        g_psoRegistry[*ppPipelineState] = meta;
    }

    return hr;
}

// ---------------------------------------------------------------------------
// Hook: ID3D12Device::CreateComputePipelineState
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
    ID3D12Device*                            pThis,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID                                   riid,
    void**                                   ppPipelineState)
{
    if (pThis && pThis != g_d3d12Device) {
        g_d3d12Device = pThis;
    }

    HRESULT hr = o_CreateComputePipelineState(pThis, pDesc, riid, ppPipelineState);
    if (FAILED(hr) || !ppPipelineState || !*ppPipelineState || !pDesc) {
        return hr;
    }

    PsoMetadata meta{};
    meta.csHash = ComputeFNV1a64(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength);

    if (kTargetMotionBlurShaderHash_CS != 0 && meta.csHash == kTargetMotionBlurShaderHash_CS) {
        meta.classifiedPass = PassType::MotionBlur;
        Log::InfoF("Identified COMPUTE MOTION BLUR PSO (CS Hash Match: {:#018x})", meta.csHash);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_psoMutex);
        g_psoRegistry[*ppPipelineState] = meta;
    }

    return hr;
}

// ---------------------------------------------------------------------------
// Hook: ID3D12GraphicsCommandList::SetPipelineState
// ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Hooked_SetPipelineState(
    ID3D12GraphicsCommandList* pThis,
    ID3D12PipelineState*       pPipelineState)
{
    o_SetPipelineState(pThis, pPipelineState);

    if (!pPipelineState) return;

    try {
        PassType pass = PassType::Unknown;
        {
            std::lock_guard<std::recursive_mutex> lock(g_psoMutex);
            auto it = g_psoRegistry.find(pPipelineState);
            if (it != g_psoRegistry.end()) {
                pass = it->second.classifiedPass;
            }
        }

        std::lock_guard<std::recursive_mutex> lock(g_cmdListContextMutex);
        CmdListContext& ctx = g_cmdListContexts[pThis];
        ctx.currentPso = pPipelineState;
        ctx.activePass = pass;
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Hook: ID3D12GraphicsCommandList::ResourceBarrier
// ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Hooked_ResourceBarrier(
    ID3D12GraphicsCommandList*    pThis,
    UINT                          NumBarriers,
    const D3D12_RESOURCE_BARRIER* pBarriers)
{
    static thread_local bool t_insideBarrier = false;
    if (t_insideBarrier) {
        o_ResourceBarrier(pThis, NumBarriers, pBarriers);
        return;
    }

    struct BarrierReentrancyGuard {
        bool& flag;
        BarrierReentrancyGuard(bool& f) : flag(f) { flag = true; }
        ~BarrierReentrancyGuard() { flag = false; }
    } guard(t_insideBarrier);

    try {
        if (pBarriers && NumBarriers > 0) {
            if (!g_d3d12Device && pThis) {
                ComPtr<ID3D12Device> listDevice;
                if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(listDevice.GetAddressOf())))) {
                    g_d3d12Device = listDevice.Get();
                }
            }
            std::lock_guard<std::recursive_mutex> resLock(g_resourceMutex);
            std::lock_guard<std::recursive_mutex> ctxLock(g_cmdListContextMutex);
            CmdListContext& ctx = g_cmdListContexts[pThis];

            for (UINT i = 0; i < NumBarriers; ++i) {
                const auto& b = pBarriers[i];
                if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
                    ID3D12Resource* pRes = b.Transition.pResource;
                    D3D12_RESOURCE_STATES stateAfter = b.Transition.StateAfter;

                    TrackedResourceInfo& info = g_trackedResources[pRes];
                    info.currentState = stateAfter;
                    if (info.desc.Width == 0) {
                        info.desc = pRes->GetDesc();
                    }

                    // Identify Depth Buffer candidate & capture on transition to read
                    if (IsDepthFormat(info.desc.Format) && info.desc.Width >= 1280) {
                        ctx.lastBoundDepth = pRes;
                        if (!g_depthCapturedThisFrame.load(std::memory_order_relaxed) &&
                            (stateAfter & (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)))
                        {
                            DXGI_FORMAT copyFormat = info.desc.Format;
                            if (info.desc.Format == DXGI_FORMAT_R32_TYPELESS) copyFormat = DXGI_FORMAT_D32_FLOAT;
                            else if (info.desc.Format == DXGI_FORMAT_R24G8_TYPELESS) copyFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

                            if (EnsureSnapshotTexture(g_d3d12Device, info.desc, copyFormat, g_depthSnapshot)) {
                                RecordSafeCopy(pThis, pRes, g_depthSnapshot.Get(), b.Transition.StateBefore);
                                DLSSManager::SetDepthInput(g_depthSnapshot.Get());
                                g_depthCapturedThisFrame.store(true, std::memory_order_release);
                                Log::InfoF("Captured Depth Buffer on transition to read: {}x{}, format {}",
                                           info.desc.Width, info.desc.Height, static_cast<int>(info.desc.Format));
                            }
                        }
                    }

                    // Identify Velocity / Motion Vectors buffer candidate
                    if (IsVelocityFormat(info.desc.Format) &&
                        (stateAfter & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)))
                    {
                        ctx.lastBoundVelocity = pRes;
                    }

                    // Identify Pre-tonemap HDR color buffer candidate
                    if (IsHdrColorFormat(info.desc.Format) &&
                        (stateAfter & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)))
                    {
                        ctx.lastBoundHdrColor = pRes;
                    }
                }
            }
        }
    } catch (...) {
        // Prevent any C++ exception from escaping into Azimuth/game engine
    }

    o_ResourceBarrier(pThis, NumBarriers, pBarriers);
}

// ---------------------------------------------------------------------------
// Hook: ID3D12GraphicsCommandList::OMSetRenderTargets
// ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(
    ID3D12GraphicsCommandList*         pThis,
    UINT                               NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL                               RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    o_OMSetRenderTargets(pThis, NumRenderTargetDescriptors, pRenderTargetDescriptors,
                         RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

// ---------------------------------------------------------------------------
// Common Capture Logic Triggered on Draw / Dispatch
// ---------------------------------------------------------------------------
void OnAfterDrawOrDispatch(ID3D12GraphicsCommandList* pThis) {
    if (!pThis) return;

    static thread_local bool t_insideDraw = false;
    if (t_insideDraw) return;

    struct DrawReentrancyGuard {
        bool& flag;
        DrawReentrancyGuard(bool& f) : flag(f) { flag = true; }
        ~DrawReentrancyGuard() { flag = false; }
    } guard(t_insideDraw);

    try {
        if (!g_d3d12Device) {
            ComPtr<ID3D12Device> listDevice;
            if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(listDevice.GetAddressOf())))) {
                g_d3d12Device = listDevice.Get();
            }
        }
        if (!g_d3d12Device) return;
        CmdListContext ctx{};
        {
            std::lock_guard<std::recursive_mutex> lock(g_cmdListContextMutex);
            auto it = g_cmdListContexts.find(pThis);
            if (it != g_cmdListContexts.end()) {
                ctx = it->second;
            }
        }

        // ── Motion Blur Pass Extraction ──
        if (ctx.activePass == PassType::MotionBlur && ctx.lastBoundVelocity) {
            if (!g_mvCapturedThisFrame.load(std::memory_order_relaxed)) {
                D3D12_RESOURCE_DESC desc = ctx.lastBoundVelocity->GetDesc();
                if (EnsureSnapshotTexture(g_d3d12Device, desc, DXGI_FORMAT_UNKNOWN, g_motionVectorSnapshot)) {
                    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    {
                        std::lock_guard<std::recursive_mutex> lock(g_resourceMutex);
                        auto rIt = g_trackedResources.find(ctx.lastBoundVelocity);
                        if (rIt != g_trackedResources.end()) state = rIt->second.currentState;
                    }

                    RecordSafeCopy(pThis, ctx.lastBoundVelocity, g_motionVectorSnapshot.Get(), state);
                    DLSSManager::SetMotionVectorInput(g_motionVectorSnapshot.Get());
                    g_mvCapturedThisFrame.store(true, std::memory_order_release);
                    Log::InfoF("Captured full-res Motion Vectors: {}x{}", desc.Width, desc.Height);
                }
            }
        }

        // ── Depth Prepass Extraction ──
        if (ctx.activePass == PassType::DepthPrepass && ctx.lastBoundDepth) {
            if (!g_depthCapturedThisFrame.load(std::memory_order_relaxed)) {
                D3D12_RESOURCE_DESC desc = ctx.lastBoundDepth->GetDesc();
                DXGI_FORMAT copyFormat = desc.Format;
                if (desc.Format == DXGI_FORMAT_R32_TYPELESS) copyFormat = DXGI_FORMAT_D32_FLOAT;
                else if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) copyFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

                if (EnsureSnapshotTexture(g_d3d12Device, desc, copyFormat, g_depthSnapshot)) {
                    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    {
                        std::lock_guard<std::recursive_mutex> lock(g_resourceMutex);
                        auto rIt = g_trackedResources.find(ctx.lastBoundDepth);
                        if (rIt != g_trackedResources.end()) state = rIt->second.currentState;
                    }

                    RecordSafeCopy(pThis, ctx.lastBoundDepth, g_depthSnapshot.Get(), state);
                    DLSSManager::SetDepthInput(g_depthSnapshot.Get());
                    g_depthCapturedThisFrame.store(true, std::memory_order_release);
                    Log::InfoF("Captured Depth Buffer: {}x{}", desc.Width, desc.Height);
                }
            }
        }

        // ── Pre-tonemap HDR Color Extraction ──
        if (ctx.activePass == PassType::Tonemapping && ctx.lastBoundHdrColor) {
            if (!g_colorCapturedThisFrame.load(std::memory_order_relaxed)) {
                D3D12_RESOURCE_DESC desc = ctx.lastBoundHdrColor->GetDesc();
                if (EnsureSnapshotTexture(g_d3d12Device, desc, DXGI_FORMAT_UNKNOWN, g_colorSnapshot)) {
                    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    {
                        std::lock_guard<std::recursive_mutex> lock(g_resourceMutex);
                        auto rIt = g_trackedResources.find(ctx.lastBoundHdrColor);
                        if (rIt != g_trackedResources.end()) state = rIt->second.currentState;
                    }

                    RecordSafeCopy(pThis, ctx.lastBoundHdrColor, g_colorSnapshot.Get(), state);
                    DLSSManager::SetColorInput(g_colorSnapshot.Get());
                    g_colorCapturedThisFrame.store(true, std::memory_order_release);
                    Log::InfoF("Captured Pre-Tonemap Color Buffer: {}x{}", desc.Width, desc.Height);
                }
            }
        }
    } catch (...) {
        // Prevent any C++ exception from escaping into Azimuth/game engine
    }
}

// ---------------------------------------------------------------------------
// Hook: ID3D12GraphicsCommandList::DrawIndexedInstanced
// ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Hooked_DrawIndexedInstanced(
    ID3D12GraphicsCommandList* pThis,
    UINT                       IndexCountPerInstance,
    UINT                       InstanceCount,
    UINT                       StartIndexLocation,
    INT                        BaseVertexLocation,
    UINT                       StartInstanceLocation)
{
    o_DrawIndexedInstanced(pThis, IndexCountPerInstance, InstanceCount,
                           StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    OnAfterDrawOrDispatch(pThis);
}

// ---------------------------------------------------------------------------
// Hook: ID3D12GraphicsCommandList::DrawInstanced
// ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Hooked_DrawInstanced(
    ID3D12GraphicsCommandList* pThis,
    UINT                       VertexCountPerInstance,
    UINT                       InstanceCount,
    UINT                       StartVertexLocation,
    UINT                       StartInstanceLocation)
{
    o_DrawInstanced(pThis, VertexCountPerInstance, InstanceCount,
                    StartVertexLocation, StartInstanceLocation);
    OnAfterDrawOrDispatch(pThis);
}

// ---------------------------------------------------------------------------
// Hook: ID3D12GraphicsCommandList::Dispatch
// ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Hooked_Dispatch(
    ID3D12GraphicsCommandList* pThis,
    UINT                       ThreadGroupCountX,
    UINT                       ThreadGroupCountY,
    UINT                       ThreadGroupCountZ)
{
    o_Dispatch(pThis, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    OnAfterDrawOrDispatch(pThis);
}

// ---------------------------------------------------------------------------
// Helper: Resolve vtable slot from COM pointer
// ---------------------------------------------------------------------------
inline void* GetVTableMethod(void* pInterface, UINT index) {
    void** vtable = *reinterpret_cast<void***>(pInterface);
    return vtable[index];
}

} // anonymous namespace

// ===========================================================================
// BufferExtraction Public API Implementation
// ===========================================================================
namespace BufferExtraction {

bool Initialize(ID3D12Device* pDevice) {
    if (!pDevice) {
        Log::Error("BufferExtraction::Initialize called with null device.");
        return false;
    }

    g_d3d12Device = pDevice;

    if (g_hooksInstalled.exchange(true)) {
        Log::Info("BufferExtraction hooks already installed.");
        return true;
    }

    Log::Info("Installing ID3D12Device and ID3D12GraphicsCommandList hooks...");

    // Create a temporary allocator and command list to extract the CommandList vtable
    ComPtr<ID3D12CommandAllocator> tmpAlloc;
    HRESULT hr = pDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(tmpAlloc.GetAddressOf())
    );
    if (FAILED(hr)) {
        Log::Error("BufferExtraction: Failed to create dummy command allocator.");
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> tmpCmdList;
    hr = pDevice->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        tmpAlloc.Get(), nullptr,
        IID_PPV_ARGS(tmpCmdList.GetAddressOf())
    );
    if (FAILED(hr)) {
        Log::Error("BufferExtraction: Failed to create dummy command list.");
        return false;
    }
    tmpCmdList->Close();

    // ── 1. Hook ID3D12Device methods ──────────────────────────────────────
    // VTable Index 10: CreateGraphicsPipelineState
    void* pCreateGraphicsPSO = GetVTableMethod(pDevice, 10);
    if (MH_CreateHook(pCreateGraphicsPSO, &Hooked_CreateGraphicsPipelineState,
                      reinterpret_cast<void**>(&o_CreateGraphicsPipelineState)) == MH_OK) {
        MH_EnableHook(pCreateGraphicsPSO);
        Log::Info("Hooked ID3D12Device::CreateGraphicsPipelineState (vtable[10]).");
    } else {
        Log::Error("Failed to hook CreateGraphicsPipelineState!");
    }

    // VTable Index 11: CreateComputePipelineState
    void* pCreateComputePSO = GetVTableMethod(pDevice, 11);
    if (MH_CreateHook(pCreateComputePSO, &Hooked_CreateComputePipelineState,
                      reinterpret_cast<void**>(&o_CreateComputePipelineState)) == MH_OK) {
        MH_EnableHook(pCreateComputePSO);
        Log::Info("Hooked ID3D12Device::CreateComputePipelineState (vtable[11]).");
    } else {
        Log::Error("Failed to hook CreateComputePipelineState!");
    }

    // ── 2. Hook ID3D12GraphicsCommandList methods ─────────────────────────
    // VTable Index 12: DrawInstanced
    void* pDrawInstanced = GetVTableMethod(tmpCmdList.Get(), 12);
    if (MH_CreateHook(pDrawInstanced, &Hooked_DrawInstanced,
                      reinterpret_cast<void**>(&o_DrawInstanced)) == MH_OK) {
        MH_EnableHook(pDrawInstanced);
        Log::Info("Hooked ID3D12GraphicsCommandList::DrawInstanced (vtable[12]).");
    }

    // VTable Index 13: DrawIndexedInstanced
    void* pDrawIndexed = GetVTableMethod(tmpCmdList.Get(), 13);
    if (MH_CreateHook(pDrawIndexed, &Hooked_DrawIndexedInstanced,
                      reinterpret_cast<void**>(&o_DrawIndexedInstanced)) == MH_OK) {
        MH_EnableHook(pDrawIndexed);
        Log::Info("Hooked ID3D12GraphicsCommandList::DrawIndexedInstanced (vtable[13]).");
    }

    // VTable Index 14: Dispatch
    void* pDispatch = GetVTableMethod(tmpCmdList.Get(), 14);
    if (MH_CreateHook(pDispatch, &Hooked_Dispatch,
                      reinterpret_cast<void**>(&o_Dispatch)) == MH_OK) {
        MH_EnableHook(pDispatch);
        Log::Info("Hooked ID3D12GraphicsCommandList::Dispatch (vtable[14]).");
    }

    // VTable Index 25: SetPipelineState
    void* pSetPSO = GetVTableMethod(tmpCmdList.Get(), 25);
    if (MH_CreateHook(pSetPSO, &Hooked_SetPipelineState,
                      reinterpret_cast<void**>(&o_SetPipelineState)) == MH_OK) {
        MH_EnableHook(pSetPSO);
        Log::Info("Hooked ID3D12GraphicsCommandList::SetPipelineState (vtable[25]).");
    }

    // VTable Index 26: ResourceBarrier
    void* pBarrier = GetVTableMethod(tmpCmdList.Get(), 26);
    if (MH_CreateHook(pBarrier, &Hooked_ResourceBarrier,
                      reinterpret_cast<void**>(&o_ResourceBarrier)) == MH_OK) {
        MH_EnableHook(pBarrier);
        Log::Info("Hooked ID3D12GraphicsCommandList::ResourceBarrier (vtable[26]).");
    }

    // VTable Index 46: OMSetRenderTargets
    void* pOMSetRTs = GetVTableMethod(tmpCmdList.Get(), 46);
    if (MH_CreateHook(pOMSetRTs, &Hooked_OMSetRenderTargets,
                      reinterpret_cast<void**>(&o_OMSetRenderTargets)) == MH_OK) {
        MH_EnableHook(pOMSetRTs);
        Log::Info("Hooked ID3D12GraphicsCommandList::OMSetRenderTargets (vtable[46]).");
    }

    Log::Info("BufferExtraction initialized successfully.");
    return true;
}

void SetDevice(ID3D12Device* pDevice) {
    if (pDevice) {
        g_d3d12Device = pDevice;
    }
}

void OnExecuteCommandLists(
    ID3D12CommandQueue*       /*pQueue*/,
    UINT                      /*numCommandLists*/,
    ID3D12CommandList* const* /*ppCommandLists*/)
{
    // Submission monitoring if needed
}

ID3D12Resource* GetDepthBuffer()      { return g_depthSnapshot.Get(); }
ID3D12Resource* GetMotionVectors()    { return g_motionVectorSnapshot.Get(); }
ID3D12Resource* GetPreTonemapColor()  { return g_colorSnapshot.Get(); }

void ResetFrameState() {
    g_depthCapturedThisFrame.store(false, std::memory_order_relaxed);
    g_mvCapturedThisFrame.store(false, std::memory_order_relaxed);
    g_colorCapturedThisFrame.store(false, std::memory_order_relaxed);
}

void Shutdown() {
    g_depthSnapshot.Reset();
    g_motionVectorSnapshot.Reset();
    g_colorSnapshot.Reset();

    {
        std::lock_guard<std::recursive_mutex> lock(g_psoMutex);
        g_psoRegistry.clear();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_resourceMutex);
        g_trackedResources.clear();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_cmdListContextMutex);
        g_cmdListContexts.clear();
    }

    g_d3d12Device = nullptr;
    g_hooksInstalled = false;
    Log::Info("BufferExtraction shut down.");
}

} // namespace BufferExtraction
