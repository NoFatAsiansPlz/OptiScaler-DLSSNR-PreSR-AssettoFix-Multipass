// Reuses the existing OptiScaler MFG implementation and its attribution/license notices.
#include "AdaMfgUnlock.h"
#include "Common.h"
#include "../../external/nvapi/nvapi.h"
#include <framegen/dlssg/MfgUnlockPtx.h>
#include <framegen/dlssg/MfgUnlockPlugin.h>
#include <framegen/dlssg/MfgUnlockFlip.h>
#include <tlhelp32.h>
#include <mutex>
#include <vector>

namespace nr::AdaMfgUnlock {
namespace {
constexpr uint8_t kGeneratedFrames = 3; // Standalone UI supports up to 4x.
std::mutex mutex;
bool ada = false, active = false, providerReady = false, sessionProviderReady = false;
unsigned maximum = 2;
HMODULE retainedProvider = nullptr; // Only the NGX provider survives capture restarts.

struct ModuleRef {
    HMODULE value = nullptr;
    ~ModuleRef() { if (value) FreeLibrary(value); }
};

bool SelectedAdapterIsAda(ID3D12Device* device) {
    if (!device) return false;
    ModuleRef api{LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!api.value) { Log("Ada MFG: system NVAPI unavailable; unlock disabled."); return false; }
    using Query = void* (__cdecl*)(unsigned);
    auto query = reinterpret_cast<Query>(GetProcAddress(api.value, "nvapi_QueryInterface"));
    if (!query) return false;
    auto initialize = reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
    auto unload = reinterpret_cast<decltype(&NvAPI_Unload)>(query(0xd22bdd7e));
    auto enumerate = reinterpret_cast<decltype(&NvAPI_EnumPhysicalGPUs)>(query(0xe5ac921f));
    auto logical = reinterpret_cast<decltype(&NvAPI_GetLogicalGPUFromPhysicalGPU)>(query(0xadd604d1));
    auto info = reinterpret_cast<decltype(&NvAPI_GPU_GetLogicalGpuInfo)>(query(0x842b066e));
    auto arch = reinterpret_cast<decltype(&NvAPI_GPU_GetArchInfo)>(query(0xd8265d24));
    if (!initialize || !unload || !enumerate || !logical || !info || !arch || initialize() != NVAPI_OK) {
        Log("Ada MFG: NVAPI architecture/LUID query unavailable; unlock disabled."); return false;
    }
    const LUID selected = device->GetAdapterLuid();
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS]{};
    NvU32 count = 0;
    bool matched = false, result = false;
    if (enumerate(handles, &count) == NVAPI_OK && count <= NVAPI_MAX_PHYSICAL_GPUS) {
        for (NvU32 i = 0; i < count; ++i) {
            NvLogicalGpuHandle group = nullptr;
            LUID luid{};
            NV_LOGICAL_GPU_DATA data{};
            data.version = NV_LOGICAL_GPU_DATA_VER;
            data.pOSAdapterId = &luid;
            if (logical(handles[i], &group) != NVAPI_OK || info(group, &data) != NVAPI_OK ||
                luid.LowPart != selected.LowPart || luid.HighPart != selected.HighPart) continue;
            // A linked mixed-GPU adapter cannot be treated as a single known Ada GPU.
            if (data.physicalGpuCount != 1 || matched) { result = false; break; }
            matched = true;
            NV_GPU_ARCH_INFO architecture{};
            architecture.version = NV_GPU_ARCH_INFO_VER;
            result = arch(handles[i], &architecture) == NVAPI_OK &&
                     architecture.architecture_id == NV_GPU_ARCHITECTURE_AD100;
        }
    }
    unload();
    Log(result ? "Ada MFG: selected D3D12 adapter verified as Ada by NVAPI architecture and LUID."
               : "Ada MFG: selected adapter is not verified Ada; native frame generation unchanged.");
    return result;
}

// Enumerate only on the ordinary startup thread; never under the Windows loader lock.
HMODULE FindProvider() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()));
    if (snapshot.value == INVALID_HANDLE_VALUE) { snapshot.value = nullptr; return nullptr; }
    HMODULE candidate = nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot.value, &entry); more; more = Module32NextW(snapshot.value, &entry)) {
        const bool ngx = GetProcAddress(entry.hModule, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") != nullptr;
        if (!ngx || !MfgUnlock::Provider::ImageContains(entry.hModule, MfgUnlock::Provider::kMarker)) continue;
        if (candidate) { Log("Ada MFG: multiple loaded DLSS-G providers; unlock disabled."); return nullptr; }
        candidate = entry.hModule;
    }
    if (!candidate) Log("Ada MFG: no loaded DLSS-G provider after NGX initialization; using native FG.");
    return candidate;
}

// Exact instruction sequences from MfgUnlock.cpp; -1 is the branch displacement wildcard.
template<size_t N> uint8_t* Unique(HMODULE module, const int (&pattern)[N]) {
    uint8_t* found = nullptr;
    unsigned hits = 0;
    const bool valid = MfgUnlock::Provider::ForEachSection(module, [&](uint8_t* bytes, size_t size, DWORD flags) {
        if (!(flags & IMAGE_SCN_MEM_EXECUTE) || size < N) return;
        for (size_t i = 0; i <= size - N; ++i) {
            bool match = true;
            for (size_t j = 0; j < N; ++j)
                if (pattern[j] >= 0 && bytes[i+j] != pattern[j]) { match = false; break; }
            if (match) { ++hits; found = bytes + i; }
        }
    });
    return valid && hits == 1 ? found : nullptr;
}

struct Edit {
    uint8_t* address;
    std::vector<uint8_t> original, replacement;
};
void Add(std::vector<Edit>& edits, uint8_t* address, std::initializer_list<uint8_t> bytes) {
    edits.push_back({address, {address, address + bytes.size()}, bytes});
}
bool Gates(HMODULE module, std::vector<Edit>& edits) {
    constexpr int advertise[]{0xBB,1,0,0,0,0x41,0xB8,3,0,0,0,0x81,0xFF,0xB0,1,0,0,0x44,0x0F,0x4C,0xC3};
    constexpr int validate[]{0x3D,0xB0,1,0,0,0x7C,-1,0x83,0xFB,3,0x76};
    constexpr int advertise309[]{0x81,0xFD,0xB0,1,0,0,0x0F,0x8C,-1,-1,-1,-1,0xBF,5,0,0,0};
    constexpr int validate309[]{0x3D,0xB0,1,0,0,0x0F,0x93,0xC0};
    auto a = Unique(module, advertise), v = Unique(module, validate);
    auto a309 = Unique(module, advertise309), v309 = Unique(module, validate309);
    if (a309 && v309 && !a && !v) {
        Add(edits, a309 + 6, {0x0F,0x1F,0x44,0,0,0x90});
        Add(edits, a309 + 13, {kGeneratedFrames});
        Add(edits, v309 + 5, {0xB0,1,0x90});
    } else if (a && v && !a309 && !v309) {
        Add(edits, a + 7, {kGeneratedFrames});
        Add(edits, a + 17, {0x0F,0x1F,0x40,0});
        Add(edits, v + 5, {0x90,0x90});
        Add(edits, v + 9, {kGeneratedFrames});
    } else return false;
    return true;
}

// Ptx::Apply reports successful writes, so count its supported descriptors first. A partial
// redirection must not enable MFG: an unmodified alias would still produce repeated midpoints.
size_t TemporalDescriptorCount(HMODULE module) {
    using namespace MfgUnlock::Ptx;
    const uintptr_t start = reinterpret_cast<uintptr_t>(module);
    size_t imageSize = 0, count = 0;
    const uint8_t* selectedFat = nullptr;
    bool ambiguous = false;
    const bool valid = MfgUnlock::Provider::ForEachSection(module,
        [&](uint8_t* data, size_t size, DWORD flags) {
            if (!(flags & IMAGE_SCN_MEM_READ) || (flags & IMAGE_SCN_MEM_EXECUTE)) return;
            for (size_t off = 0; off + sizeof(uint64_t) <= size; off += sizeof(uint64_t)) {
                uint64_t pointer = 0;
                std::memcpy(&pointer, data + off, sizeof(pointer));
                if (pointer < start || pointer >= start + imageSize || start + imageSize - pointer < 1024) continue;
                const auto* fat = reinterpret_cast<const uint8_t*>(pointer);
                if (ReadU32(fat) != kFatbinMagic) continue;
                for (const auto& profile : kProfiles) {
                    uint64_t entry = 0, descriptor = 0;
                    const auto slot = reinterpret_cast<uintptr_t>(data + off);
                    if (!ReadRelativePointer(start, imageSize, slot, profile.entryNameOffset, entry) ||
                        !ReadRelativePointer(start, imageSize, slot, profile.descriptorNameOffset, descriptor) ||
                        !PointsToCString(start, imageSize, entry, profile.entryName) ||
                        !PointsToCString(start, imageSize, descriptor, profile.descriptorName)) continue;
                    const uint64_t declared = ReadU64(fat + 8);
                    if (declared > start + imageSize - pointer - kOuterHeader) continue;
                    const size_t total = static_cast<size_t>(declared) + kOuterHeader;
                    if (total < 1024 || total > (16u << 20) || FindProfile(fat, total) != &profile) continue;
                    if (selectedFat && selectedFat != fat) ambiguous = true;
                    selectedFat = fat;
                    ++count;
                    break;
                }
            }
        }, &imageSize);
    return valid && !ambiguous ? count : 0;
}

// Make every affected page writable before any gate is changed. Page deduplication preserves
// the original protection even when the advertise and validation instructions share a page.
class WritablePages {
    struct Page { void* address; size_t size; DWORD protection; };
    std::vector<Page> pages;
public:
    ~WritablePages() {
        for (auto it = pages.rbegin(); it != pages.rend(); ++it) {
            DWORD ignored{};
            if (!VirtualProtect(it->address, it->size, it->protection, &ignored)) {
                try { Log("Ada MFG: could not restore patched-page protection."); } catch (...) {}
            }
        }
    }
    bool Prepare(const std::vector<Edit>& edits) {
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        const uintptr_t pageSize = system.dwPageSize;
        for (const auto& edit : edits) {
            if (std::memcmp(edit.address, edit.original.data(), edit.original.size())) return false;
            uintptr_t start = reinterpret_cast<uintptr_t>(edit.address) & ~(pageSize - 1);
            const uintptr_t end = reinterpret_cast<uintptr_t>(edit.address) + edit.original.size();
            for (; start < end; start += pageSize) {
                void* page = reinterpret_cast<void*>(start);
                if (std::any_of(pages.begin(), pages.end(), [&](const Page& p) { return p.address == page; })) continue;
                DWORD old{};
                if (!VirtualProtect(page, pageSize, PAGE_EXECUTE_READWRITE, &old)) return false;
                pages.push_back({page, pageSize, old});
            }
        }
        return true;
    }
};

std::vector<Edit> pluginEdits;
HMODULE lastPlugin = nullptr;
std::wstring lastPluginPath;
unsigned lastMaximum = 2;

// Checking all saved replacement bytes (not just an HMODULE) handles unload/reload at the same base.
bool StillPatched(HMODULE module, const std::wstring& path) {
    if (module != lastPlugin || path != lastPluginPath || pluginEdits.empty()) return false;
    size_t imageSize = 0;
    if (!MfgUnlock::Provider::ForEachSection(module, [](uint8_t*, size_t, DWORD) {}, &imageSize)) return false;
    auto base = reinterpret_cast<uintptr_t>(module);
    for (const auto& edit : pluginEdits) {
        const auto address = reinterpret_cast<uintptr_t>(edit.address);
        if (address < base || address - base > imageSize || edit.replacement.size() > imageSize - (address - base)) return false;
        if (std::memcmp(edit.address, edit.replacement.data(), edit.replacement.size())) return false;
    }
    return true;
}
} // namespace

void ResetSession() {
    std::lock_guard lock(mutex);
    ada = false; active = false; sessionProviderReady = false; maximum = 2;
}

void PrepareProvider(ID3D12Device* device) {
    std::lock_guard lock(mutex);
    active = false; maximum = 2; sessionProviderReady = false;
    ada = SelectedAdapterIsAda(device);
    if (!ada) return;
    HMODULE module = FindProvider();
    if (!module) return;
    if (providerReady && module == retainedProvider) {
        sessionProviderReady = true;
        Log("Ada MFG: reusing the prepared DLSS-G provider across capture restart."); return;
    }
    if (retainedProvider) {
        Log("Ada MFG: provider preparation was incomplete or provider changed; restart the app before retrying."); return;
    }
    ModuleRef hold;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &hold.value)) return;
    std::vector<Edit> gates;
    if (!Gates(module, gates)) {
        Log("Ada MFG: provider capability/validation signatures unsupported or ambiguous; native FG retained."); return;
    }
    const size_t expectedDescriptors = TemporalDescriptorCount(module);
    if (!expectedDescriptors) {
        Log("Ada MFG: temporal descriptors unsupported or ambiguous; native FG retained."); return;
    }
    WritablePages pages;
    if (!pages.Prepare(gates)) {
        Log("Ada MFG: could not prepare both provider gates; native FG retained."); return;
    }
    MfgUnlock::Ptx::Result temporal;
    const bool corrected = MfgUnlock::Ptx::Apply(module, temporal);
    Log("Ada MFG: temporal correction: " + temporal.detail);
    if (!corrected || !temporal.redirected) return;
    // The PTX descriptor pointers now refer to process-lifetime memory; preserve their owner too.
    retainedProvider = hold.value; hold.value = nullptr;
    if (temporal.redirected != expectedDescriptors) {
        Log("Ada MFG: incomplete temporal correction; provider gates unchanged and MFG disabled."); return;
    }
    for (const auto& gate : gates) std::memcpy(gate.address, gate.replacement.data(), gate.replacement.size());
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    providerReady = true; sessionProviderReady = true;
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    Log("Ada MFG: provider gates and temporal kernel prepared: " + Narrow(path));
}

void PreparePlugin(void* getStateFunction) {
    std::lock_guard lock(mutex);
    active = false; maximum = 2;
    if (!ada || !sessionProviderReady || !getStateFunction) return;
    ModuleRef plugin;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(getStateFunction), &plugin.value)) return;
    wchar_t filename[32768]{};
    GetModuleFileNameW(plugin.value, filename, static_cast<DWORD>(std::size(filename)));
    const std::wstring path(filename);
    MfgUnlock::Plugin::CeilingSite ceiling;
    const auto ceilingFound = MfgUnlock::Plugin::FindCeilingSite(plugin.value, ceiling);
    if (ceilingFound != MfgUnlock::Plugin::FindResult::Found) {
        if (ceilingFound == MfgUnlock::Plugin::FindResult::None && StillPatched(plugin.value, path)) {
            active = true; maximum = lastMaximum;
            Log("Ada MFG: verified existing plugin patches for this capture session.");
        } else Log("Ada MFG: Streamline frame-count clamp unsupported or ambiguous; limited to native 2x.");
        return;
    }
    MfgUnlock::Flip::Plan flip;
    const auto flipFound = MfgUnlock::Flip::FindPlan(plugin.value, flip);
    if (flipFound != MfgUnlock::Flip::FindResult::Found || ceiling.compiled < 2) {
        Log(std::string("Ada MFG: software pacing unavailable; limited to native 2x: ") + MfgUnlock::Flip::Describe(flipFound));
        return;
    }
    std::vector<Edit> edits;
    Add(edits, ceiling.address + MfgUnlock::Plugin::kModRmOffset, {MfgUnlock::Plugin::kModRmPatched});
    for (const auto& site : flip.sites)
        edits.push_back({site.address, {std::begin(site.original), std::end(site.original)},
                                      {std::begin(site.replacement), std::end(site.replacement)}});
    WritablePages pages;
    if (!pages.Prepare(edits)) { Log("Ada MFG: plugin pages unavailable; limited to native 2x."); return; }
    if (MfgUnlock::Flip::Apply(flip) != MfgUnlock::Flip::ApplyResult::Patched ||
        MfgUnlock::Plugin::ApplyCeilingPatch(ceiling) != MfgUnlock::Plugin::ApplyResult::Patched) {
        for (const auto& edit : edits) std::memcpy(edit.address, edit.original.data(), edit.original.size());
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        Log("Ada MFG: plugin patch failed and was rolled back; limited to native 2x."); return;
    }
    lastPlugin = plugin.value; lastPluginPath = path; pluginEdits = std::move(edits);
    lastMaximum = std::min<unsigned>(kGeneratedFrames, ceiling.compiled) + 1;
    maximum = lastMaximum; active = true;
    Log(std::format("Ada MFG: active up to {}x; temporal correction and software pacing enabled ({} sites): {}",
                    maximum, flip.sites.size(), Narrow(path)));
    // plugin reference is released here: Streamline must unload fully during capture shutdown.
}

bool RequestedOnAda() { std::lock_guard lock(mutex); return ada; }
bool Active() { std::lock_guard lock(mutex); return active; }
unsigned MaximumMultiplier() { std::lock_guard lock(mutex); return maximum; }
} // namespace nr::AdaMfgUnlock
