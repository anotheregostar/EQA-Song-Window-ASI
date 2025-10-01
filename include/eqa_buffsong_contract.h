#pragma once

// ===== wire contract (matches your DLL PRs) =====
#define EQA_DLL_VERSION              1
#define EQA_DLL_VERSION_MESSAGE_ID   4

#define EQA_HS_WITH_SONGS_ID         2
#define EQA_HS_NO_SONGS_ID           3
#define EQA_BSP_FEATURE_VERSION_1    1

// ===== client constants used by the detours =====
#define EQA_EQ_NUM_BUFFS_BASE        15
#define EQA_SONG_SLOTS               6

// ===== fixed RVAs (eqgame.exe 32-bit) =====
// NOTE: these are RVAs from module base (GetModuleHandle("eqgame.exe") + RVA)
#define ADDR_EQCharacter_FindAffectSlot   0x004C7A3E
#define ADDR_EQCharacter_GetBuff          0x004C465A
#define ADDR_EQCharacter_GetMaxBuffs      0x004C4637
#define ADDR_CBuffWindow_WndNotification  0x00408FF1
#define ADDR_OP_BuffLoopBound_cmp15       0x004E9F8E    // write at +2 to change 0x0F -> 0x1E
#define ADDR_NewUiFlagByte                0x008092D8    // UI (old/new) flag byte
