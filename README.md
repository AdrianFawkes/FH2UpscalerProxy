# FH2 DLSS Proxy — DXGI Proxy + NVIDIA DLSS/DLAA for Forza Horizon 2 (Azimuth)

## Overview

This project is a **dxgi.dll proxy** that intercepts DirectX 12 calls made by
Forza Horizon 2 (running via the Azimuth DX12 emulator) and injects NVIDIA DLSS
super-resolution or DLAA anti-aliasing.

### Architecture

```
Game EXE ──loads──▶ [OUR dxgi.dll] ──forwards──▶ [System32\dxgi.dll]
                          │
                          ├─ Hooks IDXGISwapChain::Present
                          ├─ Hooks ID3D12CommandQueue::ExecuteCommandLists
                          ├─ Hooks IDXGISwapChain::ResizeBuffers
                          │
                          ├─ Extracts depth buffer & motion vectors
                          ├─ Applies Halton(2,3) sub-pixel jitter
                          └─ Runs DLSS evaluation via NGX SDK
```

---

## Prerequisites

| Component | Version | Download |
|-----------|---------|----------|
| Visual Studio 2022 | 17.x | [Download](https://visualstudio.microsoft.com/) |
| Windows SDK | 10.0.22621+ | Included with VS Installer |
| CMake | 3.20+ | [Download](https://cmake.org/download/) |
| NVIDIA DLSS SDK | 3.7+ | [Developer Portal](https://developer.nvidia.com/dlss) |
| MinHook | 1.3.3 | [GitHub](https://github.com/TsudaKageworthy/minhook/releases) |
| nvngx_dlss.dll | Matching SDK | Included in DLSS SDK `lib\Windows_x86_64\rel\` |

### GPU Requirement
- **NVIDIA RTX 2080 Ti** (Turing, SM 7.5) — fully supported for DLSS 2/3 & DLAA.

---

## Directory Structure

```
FH2_DLSS_Proxy/
├── CMakeLists.txt              # Build configuration
├── dxgi.def                    # Module definition (exports)
├── include/
│   ├── buffer_extraction.h
│   ├── dlss_manager.h
│   ├── dxgi_proxy.h
│   ├── hooks.h
│   ├── jitter.h
│   └── logger.h
├── src/
│   ├── dllmain.cpp             # DLL entry point
│   ├── dxgi_proxy.cpp          # Real DXGI forwarding
│   ├── hooks.cpp               # MinHook vtable hooks
│   ├── dlss_manager.cpp        # NGX/DLSS integration
│   ├── buffer_extraction.cpp   # Depth + MV extraction
│   ├── jitter.cpp              # Halton sub-pixel jitter
│   └── logger.cpp              # File logger
├── deps/
│   ├── minhook/                # MinHook source (vendored)
│   │   ├── include/MinHook.h
│   │   └── src/...
│   └── ngx_sdk/                # NVIDIA NGX SDK
│       ├── include/
│       │   ├── nvsdk_ngx.h
│       │   ├── nvsdk_ngx_defs.h
│       │   ├── nvsdk_ngx_params.h
│       │   └── nvsdk_ngx_helpers.h
│       └── lib/x64/
│           └── nvsdk_ngx_d.lib
└── lib/                        # (empty — for additional libs)
```

---

## Setup Steps

### 1. Install MinHook Source

```powershell
# Clone or download MinHook 1.3.3
git clone https://github.com/TsudaKageyu/minhook.git deps/minhook
```

After cloning, verify you have:
- `deps/minhook/include/MinHook.h`
- `deps/minhook/src/MinHook.c`
- `deps/minhook/src/buffer.c`
- `deps/minhook/src/trampoline.c`
- `deps/minhook/src/hde/` (folder with HDE disassembler)

### 2. Install NVIDIA DLSS SDK

1. Download from https://developer.nvidia.com/dlss
2. Extract and copy:
   - All headers from `sdk/include/` → `deps/ngx_sdk/include/`
   - `nvsdk_ngx_d.lib` from `sdk/lib/Windows_x86_64/x86_64/` → `deps/ngx_sdk/lib/x64/`

### 3. Build with CMake + Visual Studio 2022

```powershell
# From the FH2_DLSS_Proxy directory:
mkdir build
cd build

# Generate VS 2022 solution (64-bit)
cmake .. -G "Visual Studio 17 2022" -A x64

# Build Release
cmake --build . --config Release
```

The output `dxgi.dll` will be in `build/bin/Release/`.

### 4. Alternative: Open in Visual Studio Directly

1. Open Visual Studio 2022
2. File → Open → CMake...
3. Select `FH2_DLSS_Proxy/CMakeLists.txt`
4. Set configuration to `x64-Release`
5. Build → Build All

---

## Deployment

1. Copy `build/bin/Release/dxgi.dll` to the **game's executable directory**
   (same folder as the Azimuth emulator EXE or FH2 executable).

2. Copy `nvngx_dlss.dll` from the DLSS SDK's
   `lib/Windows_x86_64/rel/` directory to the **same game directory**.

3. Launch the game normally. The proxy DLL loads automatically.

4. Check `fh2_dlss_proxy.log` in the game directory for status.

---

## How It Works

### DLL Proxy Chain

```
Game calls CreateDXGIFactory2()
    → Our dxgi.dll intercepts (exported via .def)
    → Forwards to C:\Windows\System32\dxgi.dll
    → Returns the real factory to the game
```

### Hook Installation (Dummy Device Technique)

On `DLL_PROCESS_ATTACH`, a background thread:
1. Waits 3 seconds for the game to initialize
2. Creates a **temporary** D3D12 device, command queue, and swap chain
3. Reads the **vtable addresses** from these dummy COM objects
4. Uses MinHook to patch the actual function code at those addresses
5. Destroys the dummy objects — the hooks persist globally

This means when the **game's real** swap chain calls `Present()`, our hook fires.

### DLSS Evaluation Pipeline

Each frame in the `Present` hook:

```
1. Jitter::GetCurrentOffset()  →  Read current Halton(2,3) sample
2. BufferExtraction provides   →  Depth buffer + Motion vectors
3. DLSSManager::Evaluate()     →  NGX_D3D12_EVALUATE_DLSS_EXT()
4. Copy DLSS output            →  Back buffer overwrite
5. Jitter::AdvanceFrame()      →  Next sample in sequence
6. Original Present()          →  Flip to screen
```

### Sub-Pixel Jitter

The Halton sequence with bases 2 and 3 produces 32 well-distributed samples:

```
Frame 0:  (+0.000, +0.000)
Frame 1:  (+0.000, -0.167)
Frame 2:  (-0.250, +0.167)
Frame 3:  (+0.250, -0.333)
...
```

These are injected into the projection matrix:
```
proj[2][0] += jitterX * 2.0 / renderWidth
proj[2][1] += jitterY * 2.0 / renderHeight
```

> **Note**: Injecting the jitter into FH2's actual projection matrix requires
> either a constant buffer hook or a shader patch. The `Jitter::GetJitterMatrix()`
> function provides the offset matrix — you'll need to find the game's view/projection
> CBV and multiply it in.

---

## Configuration

### Switching Between DLAA and DLSS Quality

In `dlss_manager.cpp`, the default mode is **DLAA** (native resolution AA).
To switch to DLSS Quality mode, call:

```cpp
DLSSManager::SetMode(DLSSManager::Mode::Quality);
```

before `DLSSManager::Initialize()`, or add a config file reader.

| Mode | Render Scale | RTX 2080 Ti @ 4K |
|------|-------------|-------------------|
| DLAA | 100% (native) | Best quality, slight perf cost |
| Quality | ~66% | Good quality, noticeable perf gain |
| Balanced | ~58% | Balanced |
| Performance | ~50% | High perf, some quality loss |

---

## TODO / Customization Required

### 🔴 Critical — Buffer Extraction

The depth buffer and motion vector extraction in `buffer_extraction.cpp` uses
**placeholder logic**.  You must:

1. Run a **PIX** or **RenderDoc** frame capture of FH2
2. Identify the exact `ExecuteCommandLists` call indices for:
   - The depth/Z-prepass (update `kDepthPassIndex`)
   - The motion blur velocity pass (update `kMotionVectorPassIndex`)
3. Identify the `ID3D12Resource*` pointers for the depth and MV textures
4. Hook `ID3D12Device::CreateCommittedResource` to capture them by format/size

### 🟡 Important — Jitter Injection

The jitter offsets are computed but need to be **applied to FH2's projection matrix**.
Options:
- Hook the constant buffer update that writes the view-projection matrix
- Patch the vertex shader to apply the jitter offset
- Hook `ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView`

### 🟢 Optional — Motion Vector Format

FH2's motion blur pass may output velocity in a non-standard format or coordinate
space.  DLSS expects motion vectors in **pixel units** (screen-space).  You may need
a compute shader to convert them.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Game crashes on launch | Real dxgi.dll not found | Verify System32 path |
| Log says "DLSS not supported" | Wrong GPU or missing nvngx_dlss.dll | Copy nvngx_dlss.dll next to EXE |
| Black screen | DLSS output not copied to back buffer | Check resource states/formats |
| No visual change | Buffer extraction not finding resources | Run PIX capture, update pass indices |
| Jitter but no DLSS | Evaluate() getting null inputs | Verify SetColorInput/SetDepthInput called |

---

## License

This project is provided for educational and modding purposes.
NVIDIA DLSS SDK usage is subject to NVIDIA's license agreement.
