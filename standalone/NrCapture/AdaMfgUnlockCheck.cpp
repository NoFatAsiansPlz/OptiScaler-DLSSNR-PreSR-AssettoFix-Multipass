// Standalone host-only regression check. Do not link AdaMfgUnlock.cpp separately.
// Real provider arguments are mapped with DONT_RESOLVE_DLL_REFERENCES; no vendor entry point runs.
#include <windows.h>
#include <cstdio>
#include <string>

static unsigned protectCalls = 0, failProtectCall = 0;
static BOOL WINAPI CheckVirtualProtect(LPVOID address, SIZE_T size, DWORD flags, PDWORD previous) {
    if (failProtectCall && ++protectCalls == failProtectCall) {
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    return VirtualProtect(address, size, flags, previous);
}
#define VirtualProtect CheckVirtualProtect
#include "AdaMfgUnlock.cpp"
#undef VirtualProtect

namespace nr {
void Log(const std::string& message) { std::printf("%s\n", message.c_str()); }
std::string Narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), size, nullptr, nullptr);
    return text;
}
}

static unsigned failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (false)

namespace {
using namespace nr::AdaMfgUnlock;
const uint8_t advertise[]{0xBB,1,0,0,0,0x41,0xB8,3,0,0,0,0x81,0xFF,0xB0,1,0,0,0x44,0x0F,0x4C,0xC3};
const uint8_t validate[]{0x3D,0xB0,1,0,0,0x7C,0x12,0x83,0xFB,3,0x76};
const uint8_t advertise309[]{0x81,0xFD,0xB0,1,0,0,0x0F,0x8C,0x11,0x22,0x33,0x44,0xBF,5,0,0,0};
const uint8_t validate309[]{0x3D,0xB0,1,0,0,0x0F,0x93,0xC0};

struct Image {
    uint8_t* bytes = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Image() {
        if (!bytes) throw std::runtime_error("fixture allocation failed");
        std::memset(bytes, 0x90, 0x3000);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes);
        dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = 0x3000;
        auto* section = IMAGE_FIRST_SECTION(nt);
        section->VirtualAddress = 0x1000; section->Misc.VirtualSize = 0x2000;
        section->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    }
    ~Image() { VirtualFree(bytes, 0, MEM_RELEASE); }
    HMODULE module() const { return reinterpret_cast<HMODULE>(bytes); }
    template<size_t N> void Put(size_t offset, const uint8_t (&sequence)[N]) { std::memcpy(bytes + offset, sequence, N); }
};

void GateFixtures() {
    Image legacy;
    legacy.Put(0x1100, advertise); legacy.Put(0x2100, validate);
    std::vector<Edit> edits;
    CHECK(Gates(legacy.module(), edits)); CHECK(edits.size() == 4);
    if (edits.size() == 4) {
        CHECK(edits[0].address == legacy.bytes + 0x1107 && edits[0].replacement == std::vector<uint8_t>{3});
        CHECK(edits[1].address == legacy.bytes + 0x1111 && edits[1].replacement == (std::vector<uint8_t>{0x0F,0x1F,0x40,0}));
        CHECK(edits[2].address == legacy.bytes + 0x2105 && edits[2].replacement == (std::vector<uint8_t>{0x90,0x90}));
        CHECK(edits[3].address == legacy.bytes + 0x2109 && edits[3].replacement == std::vector<uint8_t>{3});
    }
    Image current;
    current.Put(0x1100, advertise309); current.Put(0x2100, validate309);
    edits.clear(); CHECK(Gates(current.module(), edits)); CHECK(edits.size() == 3);
    if (edits.size() == 3) {
        CHECK(edits[0].address == current.bytes + 0x1106 && edits[0].replacement == (std::vector<uint8_t>{0x0F,0x1F,0x44,0,0,0x90}));
        CHECK(edits[1].address == current.bytes + 0x110D && edits[1].replacement == std::vector<uint8_t>{3});
        CHECK(edits[2].address == current.bytes + 0x2105 && edits[2].replacement == (std::vector<uint8_t>{0xB0,1,0x90}));
    }
    for (bool modern : {false, true}) {
        Image missing;
        if (modern) missing.Put(0x1100, advertise309); else missing.Put(0x1100, advertise);
        edits.clear(); CHECK(!Gates(missing.module(), edits)); CHECK(edits.empty());
        Image duplicate;
        if (modern) { duplicate.Put(0x1100, advertise309); duplicate.Put(0x1200, advertise309); duplicate.Put(0x2100, validate309); }
        else { duplicate.Put(0x1100, advertise); duplicate.Put(0x1200, advertise); duplicate.Put(0x2100, validate); }
        edits.clear(); CHECK(!Gates(duplicate.module(), edits)); CHECK(edits.empty());
    }
    edits.clear(); CHECK(Gates(legacy.module(), edits));
    const std::vector<uint8_t> before(legacy.bytes, legacy.bytes + 0x3000);
    DWORD ignored = 0;
    CHECK(VirtualProtect(legacy.bytes + 0x1000, 0x2000, PAGE_EXECUTE_READ, &ignored));
    protectCalls = 0; failProtectCall = 2;
    { WritablePages pages; CHECK(!pages.Prepare(edits)); }
    failProtectCall = 0;
    CHECK(std::memcmp(before.data(), legacy.bytes, before.size()) == 0);
    MEMORY_BASIC_INFORMATION memory{};
    CHECK(VirtualQuery(legacy.bytes + 0x1000, &memory, sizeof(memory)) != 0);
    CHECK(memory.Protect == PAGE_EXECUTE_READ); // First page was restored after second-page failure.
    ada = true; active = true; sessionProviderReady = true; maximum = 4; providerReady = true;
    ResetSession();
    CHECK(!RequestedOnAda() && !Active() && MaximumMultiplier() == 2 && !sessionProviderReady);
    CHECK(providerReady); // Process-owned provider state is intentionally retained.
    providerReady = false;
}
}

int wmain(int argc, wchar_t** argv) {
    GateFixtures();
    for (int i = 1; i < argc; ++i) {
        HMODULE provider = LoadLibraryExW(argv[i], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        CHECK(provider != nullptr);
        if (!provider) continue;
        std::vector<nr::AdaMfgUnlock::Edit> edits;
        CHECK(nr::AdaMfgUnlock::Gates(provider, edits));
        const size_t expected = nr::AdaMfgUnlock::TemporalDescriptorCount(provider);
        CHECK(expected > 0);
        MfgUnlock::Ptx::Result result;
        CHECK(MfgUnlock::Ptx::Apply(provider, result));
        CHECK(result.redirected == expected);
        CHECK(nr::AdaMfgUnlock::TemporalDescriptorCount(provider) == 0);
        std::printf("Provider %s: expected=%zu redirected=%zu\n", nr::Narrow(argv[i]).c_str(), expected, result.redirected);
        FreeLibrary(provider);
    }
    std::printf("Ada MFG host checks: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
