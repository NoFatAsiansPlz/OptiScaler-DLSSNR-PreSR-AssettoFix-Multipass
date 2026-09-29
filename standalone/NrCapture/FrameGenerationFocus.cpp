#include "FrameGenerationFocus.h"
#include <atomic>
namespace nr {
namespace {
std::atomic<HWND> outputWindow{nullptr};
HWND WINAPI ForegroundForStreamline() {
    const HWND window = outputWindow.load(std::memory_order_acquire);
    DWORD process = 0;
    if (window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId())
        return window;
    return GetForegroundWindow();
}
bool Replace(void** slot, void* expected, void* replacement) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    const bool changed = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), protection, &ignored);
    return changed;
}
}
FrameGenerationFocus::FrameGenerationFocus(void* commonFunction) {
    // Resolve the actual common plugin through its API address, including driver-supplied
    // versions. No filename guessing, instruction patches, or version-specific offsets.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(commonFunction), &common))
        Check(HRESULT_FROM_WIN32(GetLastError()), "Locate Streamline common plugin");
    const auto base = reinterpret_cast<BYTE*>(common);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress) {
        auto entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (; entry->Name && !slot; ++entry) {
            if (!entry->OriginalFirstThunk) continue;
            auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
            auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(name->Name), "GetForegroundWindow") == 0) {
                    slot = reinterpret_cast<void**>(&addresses->u1.Function);
                    break;
                }
            }
        }
    }
    original = reinterpret_cast<void*>(&GetForegroundWindow);
    if (!slot || !Replace(slot, original, reinterpret_cast<void*>(&ForegroundForStreamline))) {
        FreeLibrary(common); common = nullptr; slot = nullptr;
        throw std::runtime_error("This Streamline version cannot use the app-local background FG adapter.");
    }
    Log("DLSS FG background adapter installed in this process's Streamline common import table.");
}
FrameGenerationFocus::~FrameGenerationFocus() {
    Disable();
    if (slot && !Replace(slot, reinterpret_cast<void*>(&ForegroundForStreamline), original))
        Log("Could not restore Streamline focus import; adapter remains inactive.");
    if (common) FreeLibrary(common);
}
void FrameGenerationFocus::Enable(HWND window) {
    DWORD process = 0;
    if (!window || !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId())
        throw std::runtime_error("Background FG requires an output window owned by this application.");
    outputWindow.store(window, std::memory_order_release);
}
void FrameGenerationFocus::Disable() { outputWindow.store(nullptr, std::memory_order_release); }
}
