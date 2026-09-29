#pragma once
#include <windows.h>
#include <nvsdk_ngx.h>
// Test-only dispatcher binding. Runtime has already initialized this NGX instance.
// No game loading, interception or compatibility patching is linked into the check.
struct NVNGXProxy {
    static bool IsDx12Inited() { return GetModuleHandleW(L"_nvngx.dll") != nullptr; }
    static bool InitDx12(ID3D12Device*) { return IsDx12Inited(); }
#define NGX_ENTRY(Name) static auto Name() { return reinterpret_cast<decltype(&NVSDK_NGX_##Name)>(GetProcAddress(GetModuleHandleW(L"_nvngx.dll"), "NVSDK_NGX_" #Name)); }
    NGX_ENTRY(D3D12_GetCapabilityParameters)
    NGX_ENTRY(D3D12_DestroyParameters)
    NGX_ENTRY(D3D12_CreateFeature)
    NGX_ENTRY(D3D12_ReleaseFeature)
    NGX_ENTRY(D3D12_EvaluateFeature)
#undef NGX_ENTRY
};
