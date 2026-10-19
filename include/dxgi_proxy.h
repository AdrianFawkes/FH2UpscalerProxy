#pragma once
// ============================================================================
// dxgi_proxy.h — DXGI function forwarding declarations
// ============================================================================
#include <Windows.h>

namespace DXGIProxy {

// Load the real system dxgi.dll and resolve all function pointers.
bool Initialize();

// Unload the real DLL handle.
void Shutdown();

} // namespace DXGIProxy
