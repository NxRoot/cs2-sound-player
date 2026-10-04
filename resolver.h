#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <Windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct SoundOffsets {
    uint32_t dwLocalPlayerController;
    uint32_t dwGameRules;
    uint32_t teamNum;                      // C_BaseEntity::m_iTeamNum
    uint32_t pawnIsAlive;                  // CCSPlayerController::m_bPawnIsAlive
    uint32_t pActionTrackingServices;      // CCSPlayerController::m_pActionTrackingServices
    uint32_t numRoundKills;                // CCSPlayerController_ActionTrackingServices::m_iNumRoundKills
    uint32_t totalRoundsPlayed;            // C_CSGameRules::m_totalRoundsPlayed
    uint32_t roundWinStatus;               // C_CSGameRules::m_iRoundWinStatus
    uint32_t freezePeriod;                 // C_CSGameRules::m_bFreezePeriod
};

namespace resolver {

class Process {
public:
    explicit Process(HANDLE h) : h_(h) {}
    bool ReadRaw(uintptr_t addr, void* out, size_t size) const {
        SIZE_T got = 0;
        return ReadProcessMemory(h_, reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
    }
    template <class T> T Read(uintptr_t addr) const { T v{}; ReadRaw(addr, &v, sizeof(T)); return v; }
    std::string ReadString(uintptr_t addr, size_t max = 128) const {
        if (!addr) return {};
        char buf[256]{};
        if (max > sizeof(buf) - 1) max = sizeof(buf) - 1;
        while (max && !ReadRaw(addr, buf, max)) max /= 2;
        if (!max) return {};
        buf[max] = '\0';
        return buf;
    }
    HANDLE Handle() const { return h_; }
private:
    HANDLE h_;
};

struct Module {
    uintptr_t base{};
    uint32_t size{};
    std::vector<uint8_t> image;
    bool Fits(uint32_t rva, size_t bytes) const { return rva + bytes > rva && rva + bytes <= image.size(); }
    template <class T> T At(uint32_t rva) const {
        T v{}; if (Fits(rva, sizeof(T))) std::memcpy(&v, image.data() + rva, sizeof(T)); return v;
    }
};

inline bool FindModule(const Process& p, const wchar_t* name, uintptr_t& base, uint32_t& size) {
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModulesEx(p.Handle(), mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return false;
    DWORD count = needed / sizeof(HMODULE);
    wchar_t buf[MAX_PATH];
    for (DWORD i = 0; i < count && i < _countof(mods); ++i) {
        if (!GetModuleBaseNameW(p.Handle(), mods[i], buf, MAX_PATH)) continue;
        if (_wcsicmp(buf, name) != 0) continue;
        MODULEINFO info{};
        if (!GetModuleInformation(p.Handle(), mods[i], &info, sizeof(info))) return false;
        base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        size = info.SizeOfImage;
        return true;
    }
    return false;
}

inline bool ReadModule(const Process& p, const wchar_t* name, Module& out) {
    if (!FindModule(p, name, out.base, out.size) || !out.size) return false;
    out.image.assign(out.size, 0);
    constexpr size_t kChunk = 0x10000;
    for (size_t at = 0; at < out.size; at += kChunk) {
        size_t want = (out.size - at < kChunk) ? out.size - at : kChunk;
        p.ReadRaw(out.base + at, out.image.data() + at, want);
    }
    return true;
}

inline bool ParseSignature(const char* text, std::vector<uint8_t>& bytes, std::vector<bool>& care) {
    for (const char* p = text; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { bytes.push_back(0); care.push_back(false); while (*p == '?') ++p; continue; }
        unsigned v = 0; int d = 0;
        for (; d < 2; ++d, ++p) {
            char c = *p;
            if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
            else break;
        }
        if (d != 2) return false;
        bytes.push_back(static_cast<uint8_t>(v)); care.push_back(true);
    }
    return !bytes.empty();
}

inline uint32_t FindCode(const Module& m, const char* sig) {
    std::vector<uint8_t> bytes; std::vector<bool> care;
    if (!ParseSignature(sig, bytes, care)) return 0;
    auto dos = m.At<IMAGE_DOS_HEADER>(0);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = m.At<IMAGE_NT_HEADERS64>(dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    uint32_t sectAt = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        auto sec = m.At<IMAGE_SECTION_HEADER>(sectAt + i * sizeof(IMAGE_SECTION_HEADER));
        if (!(sec.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t begin = sec.VirtualAddress, end = begin + sec.Misc.VirtualSize;
        if (end > m.image.size()) end = (uint32_t)m.image.size();
        if (end < begin + bytes.size()) continue;
        const uint8_t* data = m.image.data();
        for (uint32_t at = begin; at <= end - bytes.size(); ++at) {
            size_t j = 0;
            for (; j < bytes.size(); ++j) if (care[j] && data[at + j] != bytes[j]) break;
            if (j == bytes.size()) return at;
        }
    }
    return 0;
}

inline uint32_t RipTarget(const Module& m, uint32_t matchRva, uint32_t opAt) {
    return matchRva + opAt + 4 + m.At<int32_t>(matchRva + opAt);
}

inline uint32_t FindExport(const Module& m, const char* name) {
    auto dos = m.At<IMAGE_DOS_HEADER>(0);
    auto nt = m.At<IMAGE_NT_HEADERS64>(dos.e_lfanew);
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    auto exp = m.At<IMAGE_EXPORT_DIRECTORY>(dir.VirtualAddress);
    for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
        uint32_t nameRva = m.At<uint32_t>(exp.AddressOfNames + i * 4);
        if (!m.Fits(nameRva, 1)) continue;
        if (std::strcmp((const char*)(m.image.data() + nameRva), name) != 0) continue;
        uint16_t ord = m.At<uint16_t>(exp.AddressOfNameOrdinals + i * 2);
        return m.At<uint32_t>(exp.AddressOfFunctions + ord * 4);
    }
    return 0;
}

inline uintptr_t FindInterface(const Process& p, const Module& m, const char* name) {
    uint32_t ci = FindExport(m, "CreateInterface");
    if (!ci) return 0;
    uintptr_t listVar = m.base + RipTarget(m, ci, 3);
    uintptr_t reg = p.Read<uintptr_t>(listVar);
    for (int g = 0; reg && g < 1024; ++g) {
        uintptr_t fn = p.Read<uintptr_t>(reg);
        uintptr_t np = p.Read<uintptr_t>(reg + 0x08);
        uintptr_t nx = p.Read<uintptr_t>(reg + 0x10);
        if (p.ReadString(np) == name && fn) {
            int32_t disp = p.Read<int32_t>(fn + 3);
            return fn + 7 + disp;
        }
        reg = nx;
    }
    return 0;
}

namespace schema {
constexpr uint32_t kSystemTypeScopes = 0x190;
constexpr uint32_t kSystemRegistrations = 0x280;
constexpr uint32_t kScopeName = 0x008;
constexpr uint32_t kScopeClassBindings = 0x560;
constexpr uint32_t kHashBlocksAllocated = 0x00C;
constexpr uint32_t kHashPeakAllocated = 0x010;
constexpr uint32_t kHashFreeBlocks = 0x020;
constexpr uint32_t kHashBuckets = 0x060;
constexpr uint32_t kHashBucketCount = 256;
constexpr uint32_t kHashBucketStride = 0x18;
constexpr uint32_t kHashBucketUncommitted = 0x10;
constexpr uint32_t kBindingName = 0x008;
constexpr uint32_t kBindingFieldCount = 0x024;
constexpr uint32_t kBindingFields = 0x030;
constexpr uint32_t kFieldStride = 0x20;
constexpr uint32_t kFieldOffset = 0x010;

inline std::vector<uintptr_t> ClassBindings(const Process& p, uintptr_t hash) {
    std::vector<uintptr_t> out;
    int32_t alloc = p.Read<int32_t>(hash + kHashBlocksAllocated);
    int32_t peak = p.Read<int32_t>(hash + kHashPeakAllocated);
    if (alloc < 0 || alloc > 0x40000) return out;
    for (uint32_t b = 0; b < kHashBucketCount; ++b) {
        uintptr_t node = p.Read<uintptr_t>(hash + kHashBuckets + b * kHashBucketStride + kHashBucketUncommitted);
        for (int g = 0; node && g < 0x10000; ++g) {
            if (auto d = p.Read<uintptr_t>(node + 0x10)) out.push_back(d);
            if ((int32_t)out.size() >= alloc) break;
            node = p.Read<uintptr_t>(node + 0x08);
        }
    }
    uintptr_t blob = p.Read<uintptr_t>(hash + kHashFreeBlocks);
    for (int g = 0; blob && g < 0x10000; ++g) {
        if (auto d = p.Read<uintptr_t>(blob + 0x10)) out.push_back(d);
        if ((int32_t)out.size() >= peak) break;
        blob = p.Read<uintptr_t>(blob);
    }
    return out;
}
} // namespace schema

struct WantedField {
    const char* className;
    const char* fieldName;
    uint32_t SoundOffsets::*dest;
};

inline bool Resolve(HANDLE handle, SoundOffsets& out, std::string& err) {
    Process p(handle);
    Module client, schemasys;
    if (!ReadModule(p, L"client.dll", client) || !ReadModule(p, L"schemasystem.dll", schemasys)) {
        err = "client.dll / schemasystem.dll not loaded yet"; return false;
    }

    uintptr_t ss = FindInterface(p, schemasys, "SchemaSystem_001");
    if (!ss) { err = "SchemaSystem_001 not found"; return false; }
    if (p.Read<int32_t>(ss + schema::kSystemRegistrations) <= 0) {
        err = "schema not populated yet"; return false;
    }

    out = SoundOffsets{};

    // Schema fields
    static const WantedField kWanted[] = {
        {"C_BaseEntity",                              "m_iTeamNum",                 &SoundOffsets::teamNum},
        {"CCSPlayerController",                       "m_bPawnIsAlive",             &SoundOffsets::pawnIsAlive},
        {"CCSPlayerController",                       "m_pActionTrackingServices",  &SoundOffsets::pActionTrackingServices},
        {"CCSPlayerController_ActionTrackingServices","m_iNumRoundKills",           &SoundOffsets::numRoundKills},
        {"C_CSGameRules",                             "m_totalRoundsPlayed",        &SoundOffsets::totalRoundsPlayed},
        {"C_CSGameRules",                             "m_iRoundWinStatus",          &SoundOffsets::roundWinStatus},
        {"C_CSGameRules",                             "m_bFreezePeriod",            &SoundOffsets::freezePeriod},
    };

    int32_t scopeCount = p.Read<int32_t>(ss + schema::kSystemTypeScopes);
    uintptr_t scopeArr = p.Read<uintptr_t>(ss + schema::kSystemTypeScopes + 0x08);
    int resolved = 0;

    for (int32_t i = 0; i < scopeCount && i < 64 && resolved < _countof(kWanted); ++i) {
        uintptr_t scope = p.Read<uintptr_t>(scopeArr + i * 8);
        if (!scope || p.ReadString(scope + schema::kScopeName, 64) != "client.dll") continue;
        for (uintptr_t binding : schema::ClassBindings(p, scope + schema::kScopeClassBindings)) {
            std::string cn = p.ReadString(p.Read<uintptr_t>(binding + schema::kBindingName), 96);
            if (cn.empty()) continue;
            for (const WantedField& w : kWanted) {
                if (out.*(w.dest) || cn != w.className) continue;
                int16_t fc = p.Read<int16_t>(binding + schema::kBindingFieldCount);
                uintptr_t fields = p.Read<uintptr_t>(binding + schema::kBindingFields);
                for (int16_t f = 0; f < fc && fields; ++f) {
                    uintptr_t field = fields + f * schema::kFieldStride;
                    if (p.ReadString(p.Read<uintptr_t>(field), 64) != w.fieldName) continue;
                    out.*(w.dest) = p.Read<uint32_t>(field + schema::kFieldOffset);
                    ++resolved;
                    break;
                }
            }
        }
        break;
    }

    for (const WantedField& w : kWanted) {
        if (!(out.*(w.dest))) { err = std::string("missing: ") + w.className + "::" + w.fieldName; return false; }
    }

    // Client globals
    struct Global { const char* name; const char* sig; uint32_t opAt; uint32_t SoundOffsets::*dest; };
    static const Global kGlobals[] = {
        {"dwLocalPlayerController", "48 8B 05 ?? ?? ?? ?? 41 89 BE", 3, &SoundOffsets::dwLocalPlayerController},
        {"dwGameRules", "F6 C1 01 0F 85 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 4D 85", 12, &SoundOffsets::dwGameRules},
    };
    for (const Global& g : kGlobals) {
        uint32_t match = FindCode(client, g.sig);
        if (!match) { err = std::string("outdated sig: ") + g.name; return false; }
        out.*(g.dest) = RipTarget(client, match, g.opAt);
    }

    err.clear();
    return true;
}

} // namespace resolver
