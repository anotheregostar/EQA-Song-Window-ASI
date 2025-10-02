#pragma once
#include <windows.h>
#include <cstdint>

// ======== CONSTANTS / ENUMS WE USE ========

#define EQ_NUM_SPELL_EFFECTS 16
#define EQ_NUM_BUFFS_BASE    15

// spell effect IDs (subset we use)
enum {
  SE_Blank = 0,
  SE_MovementSpeed = 3,
  SE_Root = 99,
  SE_Illusion = 58,
  SE_CurrentHP = 0,
  SE_ArmorClass = 1,
  SE_AttackSpeed = 11,
  SE_CompleteHeal = 101,
  SE_CHA = 24,
  SE_Lycanthropy = 50,
  SE_Vampirism = 60
};

// spawn types (subset)
enum {
  EQ_SPAWN_TYPE_PLAYER = 0,
  EQ_SPAWN_TYPE_NPC = 1
};

// ======== MINIMAL STRUCTS (only fields we read) ========

#pragma pack(push, 1)

// Spell struct (we only read Attribute[] & Base[])
struct EQSPELLINFO {
  int32_t   ID;               // not required but keeps alignment sensible
  // ... (many fields omitted)
  int32_t   FillerA[20];      // pad to where Attribute starts in your client (safe, we only index Attribute/Base by 0..15)
  int8_t    Attribute[EQ_NUM_SPELL_EFFECTS];
  int16_t   Base[EQ_NUM_SPELL_EFFECTS];
  // (rest of struct omitted)
};

// Buff slot struct (we only read these fields; this layout matches how the client accesses them)
struct EQBUFFINFO {
  uint8_t   BuffType;     // 0 = empty
  int16_t   SpellId;
  uint8_t   CasterLevel;
  int32_t   Ticks;
  int32_t   Modifier;
  int32_t   Counters;
  // (rest omitted)
};

// Spawn info (subset)
struct EQSPAWNINFO {
  uint8_t   Type;         // 0 player, 1 npc
  uint8_t   IsGameMaster; // nonzero if GM
  uint16_t  SpawnId;
  // (rest omitted)
};

// Char info (subset we actually read)
struct EQCHARINFO {
  EQSPAWNINFO* SpawnInfo;
  uint16_t     BuffCasterId[32];  // we index by buff slot (enough for 30 slots)
  // (rest omitted)
};

// UI window stubs
struct CXWnd {};
struct CSidlScreenWnd : CXWnd {};
struct CBuffWindow   : CSidlScreenWnd {
  // we only call HandleSpellInfoDisplay with a direct thunk (address below)
};

#pragma pack(pop)

// ======== GLOBAL POINTERS WE NEED ========
// NOTE: You must fill these addresses for your client build.
// These are the ONLY values I still need from your eqmac.h.
// If you don’t have them handy, the ASI will still build & run,
// but clicking off buffs from the song window will be disabled.

#ifndef EQA_ADDR_POINTER_CHAR_INFO
  // TODO: put the numeric address of the client global: EQ_POINTER_CHAR_INFO
  // Example (NOT REAL): 0x007D1234
  #define EQA_ADDR_POINTER_CHAR_INFO 0
#endif

#ifndef EQA_ADDR_POINTER_SPELL_MANAGER
  // TODO: put the numeric address of the client global: EQ_POINTER_SPELL_MANAGER
  #define EQA_ADDR_POINTER_SPELL_MANAGER 0
#endif

// convenience getters (safe if address is 0: they return nullptr)
inline EQCHARINFO*   EQ_OBJECT_CharInfo()    { return EQA_ADDR_POINTER_CHAR_INFO ? *reinterpret_cast<EQCHARINFO**>(EQA_ADDR_POINTER_CHAR_INFO) : nullptr; }
inline EQSPELLINFO** EQ_OBJECT_SpellListPtr(){ return EQA_ADDR_POINTER_SPELL_MANAGER ? *reinterpret_cast<EQSPELLINFO***>(EQA_ADDR_POINTER_SPELL_MANAGER) : nullptr; }

// ======== RAW THUNKS TO CLIENT FUNCTIONS (addresses from your DLL PR/header) ========
// If any address differs in your build, update here.
// All calls exactly match what your eqgame does — we’re not reimplementing logic.

// Spell helpers (from your header)
inline bool EQ_Spell_CanStackMultiple(void* spell) {
  return reinterpret_cast<bool(__thiscall*)(void*)>(0x004C82DC)(spell);
}
inline bool EQ_Spell_IsValidIndex(int spellid) {
  return reinterpret_cast<bool(__cdecl*)(int)>(0x004D79EA)(spellid);
}
inline bool EQ_Spell_IsSPAIgnored(int effect) {
  return reinterpret_cast<char(__cdecl*)(int)>(0x004D7201)(effect) != 0;
}
inline char EQ_Spell_AffectIndex(void* spell, int effectType) {
  return reinterpret_cast<char(__thiscall*)(void*, int)>(0x004D79C8)(spell, effectType);
}

// spell list access (matches your PR: out-of-range -> spell[8000])
inline EQSPELLINFO* EQ_Spell_Get(int id) {
  auto list = EQ_OBJECT_SpellListPtr();
  if (!list) return nullptr;
  return (id >= 8000 || id < 0) ? list[8000] : list[id];
}

// EQ_Character helpers (addresses from your header)
inline bool EQ_Character_IsValidAffect(EQCHARINFO* player, int buffslot) {
  return reinterpret_cast<bool(__thiscall*)(void*, int)>(0x004C6218)(player, buffslot);
}
inline bool EQ_Character_RemoveMyAffect(EQCHARINFO* player, int16_t buffslot) {
  return reinterpret_cast<char(__thiscall*)(void*, int16_t)>(0x004D0337)(player, buffslot) != 0;
}
inline EQBUFFINFO* EQ_Character_GetBuff(EQCHARINFO* player, int16_t buffslot) {
  return reinterpret_cast<EQBUFFINFO*(__thiscall*)(void*, int16_t)>(0x004C465A)(player, buffslot);
}
inline void EQ_Character_RemoveBuff(EQCHARINFO* player, EQBUFFINFO* buff, int send_response) {
  reinterpret_cast<void(__thiscall*)(void*, EQBUFFINFO*, int)>(0x004CB0E2)(player, buff, send_response);
}
inline bool EQ_Character_IsStackBlocked(EQCHARINFO* player, EQSPELLINFO* spell) {
  return reinterpret_cast<bool(__thiscall*)(void*, EQSPELLINFO*)>(0x004C830B)(player, spell);
}
inline short EQ_Character_CalcEffectValue(EQCHARINFO* player, EQSPELLINFO* spell, uint8_t casterLevel, uint8_t effectIndex) {
  // Address 0x004C657D per your header (was truncated in text logs but the tail ‘657D’ was visible).
  return reinterpret_cast<short(__thiscall*)(void*, EQSPELLINFO*, uint8_t, uint8_t, EQBUFFINFO*)>(0x004C657D)(player, spell, casterLevel, effectIndex, nullptr);
}

// CSidlScreenWnd::WndNotification (address from your header)
inline int CSidl_WndNotification(CSidlScreenWnd* self, void* sender, int type, int a4) {
  return reinterpret_cast<int(__thiscall*)(void*, void*, int, int)>(0x0056E920)(self, sender, type, a4);
}

// CBuffWindow::HandleSpellInfoDisplay (address from your header)
inline int CBuffWindow_HandleSpellInfoDisplay(CBuffWindow* self, void* button) {
  return reinterpret_cast<int(__thiscall*)(CBuffWindow*, void*)>(0x00409072)(self, button);
}

// Keyboard (good enough for detecting Alt in our hook)
inline bool EQ_IsKeyPressedAlt() {
  return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}
