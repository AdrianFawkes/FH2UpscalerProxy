// ============================================================================
// dllmain.cpp — Entry point for the FH2 DLSS Proxy (dxgi.dll)
// ============================================================================
//
// When the game (or the Azimuth DX12 emulator) starts, it links against
// "dxgi.dll".  Windows finds OUR copy first because it sits in the same
// directory as the executable.  This DllMain initializes the proxy layer
// and installs hooks once the real DXGI interfaces have been created.
//
// Boot sequence:
//   1. DLL_PROCESS_ATTACH
//      → Logger init
//      → Load real dxgi.dll from System32 (DXGIProxy::Initialize)
//   2. The game calls CreateDXGIFactory / CreateDXGIFactory2
//      → We forward to the real DLL
//   3. The game creates a D3D12 device and swap chain
//      → We detect the first Present call and install hooks
//   4. Hooks intercept Present and ExecuteCommandLists for DLSS injection
//   5. DLL_PROCESS_DETACH
//      → Hooks::Remove, DXGIProxy::Shutdown, Logger::Shutdown
//
// ============================================================================

#include <Windows.h>
#include "dxgi_proxy.h"
#include "hooks.h"
#include "dlss_manager.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// We need to install hooks lazily because at DLL_PROCESS_ATTACH time the
// game hasn't created the D3D12 device or swap chain yet.
//
// Strategy: we hook CreateDXGIFactory2 (our own exported trampoline) to
// intercept the IDXGIFactory and then hook IDXGIFactory::CreateSwapChain*
// to grab the swap chain and command queue the moment they're created.
//
// For simplicity, a background thread polls for the first swap chain.
// ---------------------------------------------------------------------------

static HANDLE g_hookThread = nullptr;
static bool   g_running    = true;

static DWORD WINAPI HookInstallThread(LPVOID /*param*/) {
    Log::Info("Hook-install thread started. Waiting for D3D12 swap chain...");

    // We need to find the swap chain that the game creates.
    // The simplest cross-game approach: periodically try to find an
    // IDXGISwapChain via the DXGI debug interface, or use the
    // CreateSwapChain wrapper approach below.
    //
    // Here we use a simple polling loop that creates a temporary D3D12
    // device + swap chain to grab vtable addresses, then hooks them.
    // This is the "dummy device" technique used by many overlay / mod
    // injectors (e.g. ReShade, Special K, DXVK).

    // Wait for the game to initialise its window
    Sleep(3000);

    // ── Create a temporary DXGI factory to get vtable addresses ──────────
    HMODULE realDXGI = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    HMODULE d3d12Lib = LoadLibraryW(L"d3d12.dll");
    if (!realDXGI || !d3d12Lib) {
        Log::Error("Could not load d3d12.dll or real dxgi.dll for dummy device.");
        return 1;
    }

    using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(
        IUnknown* pAdapter, D3D_FEATURE_LEVEL MinFL, REFIID riid, void** ppDevice);
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID riid, void** ppFactory);

    auto fnCreateDevice = reinterpret_cast<PFN_D3D12CreateDevice>(
        GetProcAddress(d3d12Lib, "D3D12CreateDevice"));
    auto fnCreateFactory = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(realDXGI, "CreateDXGIFactory1"));

    if (!fnCreateDevice || !fnCreateFactory) {
        Log::Error("Could not resolve D3D12CreateDevice or CreateDXGIFactory1.");
        return 1;
    }

    // Create a temporary D3D12 device
    ID3D12Device* tmpDevice = nullptr;
    HRESULT hr = fnCreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&tmpDevice));
    if (FAILED(hr) || !tmpDevice) {
        Log::Error("Dummy D3D12CreateDevice failed!");
        return 1;
    }

    // Create a command queue
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tmpQueue = nullptr;
    hr = tmpDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&tmpQueue));
    if (FAILED(hr)) {
        Log::Error("Dummy CreateCommandQueue failed!");
        tmpDevice->Release();
        return 1;
    }

    // Create a temporary DXGI factory and swap chain
    IDXGIFactory1* tmpFactory = nullptr;
    fnCreateFactory(IID_PPV_ARGS(&tmpFactory));

    // We need a temporary window for the swap chain
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.lpszClassName  = L"FH2_DLSS_Dummy";
    RegisterClassExW(&wc);
    HWND tmpHwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                    0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scDesc{};
    scDesc.BufferCount       = 2;
    scDesc.BufferDesc.Width  = 100;
    scDesc.BufferDesc.Height = 100;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.OutputWindow      = tmpHwnd;
    scDesc.SampleDesc        = {1, 0};
    scDesc.Windowed          = TRUE;

    IDXGISwapChain* tmpSwapChain = nullptr;
    hr = tmpFactory->CreateSwapChain(tmpQueue, &scDesc, &tmpSwapChain);
    if (FAILED(hr) || !tmpSwapChain) {
        Log::Error("Dummy CreateSwapChain failed!");
        tmpQueue->Release();
        tmpDevice->Release();
        tmpFactory->Release();
        DestroyWindow(tmpHwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    Log::Info("Dummy swap chain created — extracting vtable addresses.");

    // ── Install hooks using the vtable addresses from the dummy objects ──
    // MinHook patches the actual function code, so the hooks will trigger
    // for ALL swap chains and command queues, including the game's real ones.
    Hooks::Install(tmpSwapChain, tmpQueue);

    // Cleanup dummy objects
    tmpSwapChain->Release();
    tmpQueue->Release();
    tmpDevice->Release();
    tmpFactory->Release();
    DestroyWindow(tmpHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Log::Info("Hook-install thread finished.");
    return 0;
}

// ===========================================================================
// DllMain
// ===========================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);

            // Initialize the logger first so everything else can log
            Log::Init();

            Log::Info("========================================");
            Log::Info(" FH2 DLSS Proxy — DLL_PROCESS_ATTACH");
            Log::Info("========================================");

            // Load the real system dxgi.dll and resolve exports
            if (!DXGIProxy::Initialize()) {
                Log::Error("FATAL: DXGIProxy::Initialize() failed. Aborting.");
                return FALSE;
            }

            // Spawn a background thread to install hooks once D3D12 is ready
            g_hookThread = CreateThread(nullptr, 0, HookInstallThread, nullptr, 0, nullptr);
            if (!g_hookThread) {
                Log::Warn("Could not create hook-install thread.");
            }

            break;
        }

        case DLL_PROCESS_DETACH: {
            g_running = false;
            if (g_hookThread) {
                WaitForSingleObject(g_hookThread, 5000);
                CloseHandle(g_hookThread);
                g_hookThread = nullptr;
            }

            Hooks::Remove();
            DXGIProxy::Shutdown();

            Log::Info("FH2 DLSS Proxy — DLL_PROCESS_DETACH. Goodbye!");
            Log::Shutdown();
            break;
        }
    }
    return TRUE;
}
