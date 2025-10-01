// src/eqa_buffsong_asi.cpp
// eqa_buffsong: Buff Stacking + Song Window ASI scaffold
// TODOs remain as before: wire signatures, paste detour bodies, hook send/recv.

#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>
#include <optional>
#include <array>
#include <format>
#include <cassert>

#include "MinHook.h"

// --------------------------- Logging ---------------------------
static void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(std::format("[eqa_buffsong] {}\n", buf).c_str());
}

// --------------------------- Module helpers ---------------------------
static std::pair<uint8_t*, size_t> getModuleRange(HMODULE m) {
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
    return { reinterpret_cast<uint8_t*>(mi.lpBaseOfDll), static_cast<size_t>(mi.SizeOfImage) };
}

// Simple AOB scanner ("8B ?? 89 54 ?? ??")
static uint8_t* find_pattern(uint8_t* base, size_t size, std::string_view pat) {
    std::vector<int> bytes; bytes.reserve(128);
    for (size_t i=0; i<pat.size();) {
        if (pat[i]==' ') { ++i; continue; }
        if (pat[i]=='?') { bytes.push_back(-1); ++i; if (i<pat.size() && pat[i]=='?') ++i; }
        else { int b = std::stoi(std::string(pat.substr(i,2)), nullptr, 16); bytes.push_back(b); i+=2; }
    }
    for (size_t i=0; i+bytes.size() <= size; ++i) {
        bool ok=true;
        for (size_t j=0; j<bytes.size(); ++j) {
            int b = bytes[j];
            if (b!=-1 && base[i+j] != uint8_t(b)) { ok=false; break; }
        }
        if (ok) return base+i;
    }
    return nullptr;
}

// --------------------------- Handshake constants ---------------------------
static constexpr uint16_t DLL_VERSION = 1;
static constexpr uint16_t DLL_VERSION_MESSAGE_ID = 4;

static constexpr uint16_t CSM_BuffStackingPatchHandshake                   = 1;
static constexpr uint16_t CSM_BuffStackingPatchWithSongWindowHandshake     = 2;
static constexpr uint16_t CSM_BuffStackingPatchWithoutSongWindowHandshake  = 3;

static constexpr uint16_t BSP_VERSION_V2 = 2;
static constexpr uint16_t BSP_VERSION_V1 = 1;
static constexpr uint16_t BSP_FEATURE_1  = 1;

// Runtime “rules” toggled by handshake
static bool Rule_Buffstacking_Patch_Enabled = false;
static int  Rule_Max_Buffs                  = 15;
static int  Rule_Num_Short_Buffs            = 0;

// --------------------------- Target pointers (fill via sig-scan or fixed RVAs) ---------------------------
struct Targets {
    void (*SendCustomSpawnAppearance)(uint16_t id, uint16_t value, bool is_request) = nullptr;
    void*  EQZoneInfo__Ctor = nullptr;

    bool (*Orig_HandleSpawnAppearance)(uint32_t id, uint32_t value, bool is_request) = nullptr;

    int  (*Orig_EQCharacter__FindAffectSlot)(void* self, int spellId) = nullptr;
    void (*Orig_CBuffWindow__RefreshBuffDisplay)(void* self) = nullptr;

    uint8_t* pNewUiFlag = nullptr;
} g;

static void try_init_fixed_addresses() {
    HMODULE h = GetModuleHandleW(L"eqgame.exe");
    auto [base, size] = getModuleRange(h);
    if (!base) return;
    // Optional: assign known-good pointers for your exact eqgame build here.
}

static void try_sigscan_addresses() {
    HMODULE h = GetModuleHandleW(L"eqgame.exe");
    auto [base, size] = getModuleRange(h);
    if (!base) return;
    // Add your AOBs here and assign to g.*
}

// --------------------------- OnZone callbacks ---------------------------
using OnZoneCB = void(*)();
static std::vector<OnZoneCB> g_onZone;

static void SendCustomSpawnAppearanceMessage(uint16_t id, uint16_t value, bool is_request) {
    if (g.SendCustomSpawnAppearance) {
        g.SendCustomSpawnAppearance(id, value, is_request);
    } else {
        logf("NOTE: SendCustomSpawnAppearance unresolved (id=%u val=%u req=%u)", id, value, (unsigned)is_request);
    }
}

static void SendDllVersion_OnZone() {
    SendCustomSpawnAppearanceMessage(DLL_VERSION_MESSAGE_ID, DLL_VERSION, true);
}

static void BuffstackingPatch_OnZone() {
    bool is_new_ui = g.pNewUiFlag ? (*g.pNewUiFlag != 0) : false;

    if (is_new_ui) {
        SendCustomSpawnAppearanceMessage(CSM_BuffStackingPatchHandshake, BSP_VERSION_V2, true);
        SendCustomSpawnAppearanceMessage(CSM_BuffStackingPatchWithSongWindowHandshake, BSP_FEATURE_1, true);
    } else {
        SendCustomSpawnAppearanceMessage(CSM_BuffStackingPatchHandshake, BSP_VERSION_V1, true);
        SendCustomSpawnAppearanceMessage(CSM_BuffStackingPatchWithoutSongWindowHandshake, BSP_FEATURE_1, true);
    }
}

static bool BuffstackingPatch_HandleHandshake(uint32_t id, uint32_t value, bool is_request) {
    bool send_response = is_request;
    bool enabled = false;
    int  enabled_songs = 0;

    if (id == CSM_BuffStackingPatchHandshake) {
        if (value >= BSP_VERSION_V2) { send_response |= value > BSP_VERSION_V2; value = BSP_VERSION_V2; }
        else if (value == BSP_VERSION_V1) { /* ok */ }
        else { value = 0; }
        return false; // sub-handshake will set flags
    }
    else if (id == CSM_BuffStackingPatchWithSongWindowHandshake) {
        enabled = (value == BSP_FEATURE_1); enabled_songs = enabled ? 6 : 0;
    }
    else if (id == CSM_BuffStackingPatchWithoutSongWindowHandshake) {
        enabled = (value == BSP_FEATURE_1); enabled_songs = 0;
    }
    else return false;

    Rule_Buffstacking_Patch_Enabled = enabled;
    Rule_Max_Buffs = 15 + enabled_songs;
    Rule_Num_Short_Buffs = enabled_songs;

    if (send_response) {
        SendCustomSpawnAppearanceMessage((uint16_t)id, (uint16_t)value, false);
    }
    logf("Handshake applied: enabled=%d, short_songs=%d, respond=%d", (int)enabled, enabled_songs, (int)send_response);
    return true;
}

// --------------------------- Hooks ---------------------------
using tEQZoneInfoCtor = void(__thiscall*)(void* self);
static tEQZoneInfoCtor s_EQZoneInfoCtor_Orig = nullptr;
static void __fastcall EQZoneInfoCtor_Hook(void* self, void*) {
    s_EQZoneInfoCtor_Orig(self);
    for (auto fn : g_onZone) fn();
}

// Optional dispatcher hook
static bool __stdcall SpawnAppearance_Dispatch_Hook(uint32_t id, uint32_t value, bool is_request) {
    if (BuffstackingPatch_HandleHandshake(id, value, is_request)) return true;
    return g.Orig_HandleSpawnAppearance ? g.Orig_HandleSpawnAppearance(id, value, is_request) : false;
}

// Detours to paste from PR
using tFindAffectSlot = int(__thiscall*)(void*, int);
static tFindAffectSlot s_FindAffectSlot_Orig = nullptr;
static int __fastcall FindAffectSlot_Hook(void* self, void*, int spellId) {
    // TODO: paste PR body here; use Rule_* flags above.
    return s_FindAffectSlot_Orig ? s_FindAffectSlot_Orig(self, spellId) : 0;
}

using tCBuffWindow_Refresh = void(__thiscall*)(void*);
static tCBuffWindow_Refresh s_CBuffRefresh_Orig = nullptr;
static void __fastcall CBuffWindow_Refresh_Hook(void* self, void*) {
    // TODO: paste PR body here (song window / auto-hide tweaks).
    if (s_CBuffRefresh_Orig) s_CBuffRefresh_Orig(self);
}

// --------------------------- MinHook helpers ---------------------------
template <typename T>
static bool hook(void* target, T detour, T* trampolineOut, const char* name) {
    if (!target) { logf("hook %s: target null", name); return false; }
    MH_CreateHook(target, reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(trampolineOut));
    if (MH_EnableHook(target) == MH_OK) { logf("hooked %s", name); return true; }
    logf("hook %s failed", name); return false;
}

// --------------------------- Init ---------------------------
static DWORD WINAPI init_thread(LPVOID) {
    if (MH_Initialize() != MH_OK) return 0;

    try_init_fixed_addresses();
    try_sigscan_addresses();

    g_onZone.push_back(SendDllVersion_OnZone);
    g_onZone.push_back(BuffstackingPatch_OnZone);

    if (g.EQZoneInfo__Ctor) {
        hook(g.EQZoneInfo__Ctor, &EQZoneInfoCtor_Hook, &s_EQZoneInfoCtor_Orig, "EQZoneInfo__EQZoneInfo");
    } else {
        logf("WARN: EQZoneInfo__Ctor unresolved; OnZone callbacks disabled.");
    }

    // hook(SpawnAppearanceDispatcherAddr, &SpawnAppearance_Dispatch_Hook, &g.Orig_HandleSpawnAppearance, "SpawnAppearance dispatcher");

    if (g.Orig_EQCharacter__FindAffectSlot) {
        hook(reinterpret_cast<void*>(g.Orig_EQCharacter__FindAffectSlot),
             &FindAffectSlot_Hook, &s_FindAffectSlot_Orig, "EQCharacter__FindAffectSlot");
    }
    if (g.Orig_CBuffWindow__RefreshBuffDisplay) {
        hook(reinterpret_cast<void*>(g.Orig_CBuffWindow__RefreshBuffDisplay),
             &CBuffWindow_Refresh_Hook, &s_CBuffRefresh_Orig, "CBuffWindow__RefreshBuffDisplay");
    }

    logf("init complete.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, &init_thread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_Uninitialize();
    }
    return TRUE;
}
