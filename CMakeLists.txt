cmake_minimum_required(VERSION 3.20)
project(FH2_DLSS_Proxy VERSION 1.0.0 LANGUAGES C CXX)

# ── Compiler Settings ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")  # /MD or /MDd

# ── Output: always produce "dxgi.dll" ─────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── MinHook (vendored source build) ───────────────────────────────────────────
file(GLOB MINHOOK_SOURCES
    "${CMAKE_SOURCE_DIR}/deps/minhook/src/*.c"
    "${CMAKE_SOURCE_DIR}/deps/minhook/src/hde/*.c"
)
add_library(minhook STATIC ${MINHOOK_SOURCES})
target_include_directories(minhook PUBLIC "${CMAKE_SOURCE_DIR}/deps/minhook/include")
target_include_directories(minhook PRIVATE "${CMAKE_SOURCE_DIR}/deps/minhook/src" "${CMAKE_SOURCE_DIR}/deps/minhook/src/hde")

# ── NVIDIA NGX SDK paths ─────────────────────────────────────────────────────
set(NGX_SDK_DIR "${CMAKE_SOURCE_DIR}/deps/ngx_sdk")

# ── Dear ImGui sources ───────────────────────────────────────────────────────
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/deps/imgui")
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)

# ── Proxy DLL target ─────────────────────────────────────────────────────────
set(PROXY_SOURCES
    src/dllmain.cpp
    src/dxgi_proxy.cpp
    src/hooks.cpp
    src/dlss_manager.cpp
    src/buffer_extraction.cpp
    src/jitter.cpp
    src/logger.cpp
    src/imgui_menu.cpp
)

add_library(dxgi SHARED ${PROXY_SOURCES} ${IMGUI_SOURCES} dxgi.def)

target_include_directories(dxgi PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${NGX_SDK_DIR}/include"
    "${CMAKE_SOURCE_DIR}/deps/minhook/include"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

target_link_libraries(dxgi PRIVATE
    minhook
    d3d12
    dxguid
)

set(NGX_LIB "${NGX_SDK_DIR}/lib/x64/nvsdk_ngx_d.lib")
if(EXISTS "${NGX_LIB}")
    message(STATUS "Linking NGX lib: ${NGX_LIB}")
    target_link_libraries(dxgi PRIVATE "${NGX_LIB}")
else()
    message(STATUS "NGX import lib not found; fallback to dynamic load.")
    target_compile_definitions(dxgi PRIVATE NGX_DYNAMIC_LOAD=1)
endif()

target_compile_definitions(dxgi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Ensure the output DLL is named exactly "dxgi.dll"
set_target_properties(dxgi PROPERTIES
    OUTPUT_NAME "dxgi"
    PREFIX ""
    SUFFIX ".dll"
)
