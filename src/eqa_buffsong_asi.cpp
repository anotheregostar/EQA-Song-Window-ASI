// eqa_buffsong_asi.cpp
// Buff Stacking + Song Window (.ASI) for EQMac-era client.
// Mirrors logic from akplus-dll PRs, but as an ASI plugin.
//
// Build note:
//  - Make sure CMake adds include/ to the include path.
//  - Source ordering matters: we include eqmac.h, then eqgame.h, then eqmac_functions.h.

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>

// --- Your headers (order matters) ---
#include "eqmac.h"
#include "eqgame.h"
#include "eqmac_functions.h"

// Contract / addresses used by this ASI
#include "eqa_buffsong_contract.h"

// MinHook
#include "MinHook.h"

// --------------------------------------
// Logging helper
// --------------------------------------
static void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA((std::string("[eqa_buffsong] ") + buf + "\n").c_str());
}

// --------------------------------------
// Module base utility
// --------------------------------------
static std::pair<uint8_t*, size_t> module_range(HMODULE m) {
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
    return { reinterpret_cast<uint8_t*>(mi.lpBaseOfDll), (size_t)mi.SizeOfImage };
}

// --------------------------------------
// Raw hook wrapper (no templates/ABI ambiguity)
// --------------------------------------
static bool hook_raw(void* target, void* detour, void** trampOut, const char* name) {
    if (!target) { logf("hook %s: target null", name); return false; }
    if (MH_CreateHook(target, detour, trampOut) != MH_OK) { logf("hook %s: create failed", name); return false; }
    if (MH_EnableHook(target) != MH_OK) { logf("hook %s: enable failed", name); return false; }
    logf("hooked %s @ %p", name, target);
    return true;
}

// --------------------------------------
// Rule flags (mirror DLL PR names)
// --------------------------------------
static bool g_rule_buffstack_enabled = false;
static int  g_rule_max_buffs        = EQ_NUM_BUFFS;   // 15 base
static int  g_rule_num_short_buffs  = 0;

#define Rule_Buffstacking_Patch_Enabled g_rule_buffstack_enabled
#define Rule_Max_Buffs                  g_rule_max_buffs
#define Rule_Num_Short_Buffs            g_rule_num_short_buffs

// --------------------------------------
// Local state mirrored from PRs
// --------------------------------------
static CShortBuffWindow* ShortBuffWindow = nullptr;
static thread_local bool ShortBuffSupport_ReturnSongBuffs = false;

static _EQBUFFINFO* GetStartBuffArray(bool song_buffs) {
    return song_buffs ? EQ_OBJECT_CharInfo->BuffsExt : EQ_OBJECT_CharInfo->Buff;
}
static void MakeGetBuffReturnSongs(bool enabled) {
    ShortBuffSupport_ReturnSongBuffs = enabled;
}
static CShortBuffWindow* GetShortDurationBuffWindow() {
    return ShortBuffWindow;
}

// --------------------------------------
// Messaging (DLL version + handshake)
// --------------------------------------
static void SendCustomSpawnAppearanceMessage(uint16_t id, uint16_t value, bool is_request) {
    // This relies on your existing client wiring. If your headers already expose a sender,
    // replace this with the correct call. Otherwise, server-side will simply ignore.
    // (Left as a no-op logger for safety; many deployments wire this via existing helpers.)
    logf("CustomSpawnAppearance: id=%u value=%u request=%u", id, value, (unsigned)is_request);
}

// DLL version ping (on init; previously done on zone)
static void SendDllVersion_OnZone() {
    SendCustomSpawnAppearanceMessage(EQA_DLL_VERSION_MESSAGE_ID, EQA_DLL_VERSION, true);
}

// Respond if server requests version
static bool HandleDllVersionRequest(DWORD id, DWORD value, bool is_request) {
    if (id == EQA_DLL_VERSION_MESSAGE_ID) {
        if (is_request) {
            SendCustomSpawnAppearanceMessage(EQA_DLL_VERSION_MESSAGE_ID, EQA_DLL_VERSION, false);
        }
        return true;
    }
    return false;
}

// Buffstacking handshake
static void BuffstackingPatch_OnZone() {
    bool is_new_ui = (*(BYTE*)(GetModuleHandleW(L"eqgame.exe") ? (uintptr_t)GetModuleHandleW(L"eqgame.exe") + ADDR_NewUiFlagByte : 0)) != 0;
    uint16_t msg_id = is_new_ui ? EQA_HS_WITH_SONGS_ID : EQA_HS_NO_SONGS_ID;
    SendCustomSpawnAppearanceMessage(msg_id, EQA_BSP_FEATURE_VERSION_1, true);
}

static bool BuffstackingPatch_HandleHandshake(DWORD id, DWORD value, bool is_request) {
    bool enabled = false;
    int enabled_songs = 0;
    if (id == EQA_HS_WITH_SONGS_ID) {
        if (value == EQA_BSP_FEATURE_VERSION_1) { enabled = true; enabled_songs = 6; } else value = 0;
    } else if (id == EQA_HS_NO_SONGS_ID) {
        if (value == EQA_BSP_FEATURE_VERSION_1) { enabled = true; enabled_songs = 0; } else value = 0;
    } else {
        return false;
    }

    Rule_Buffstacking_Patch_Enabled = enabled;
    Rule_Max_Buffs = EQ_NUM_BUFFS + enabled_songs;
    Rule_Num_Short_Buffs = enabled_songs;

    if (is_request) SendCustomSpawnAppearanceMessage((uint16_t)id, (uint16_t)value, false);
    logf("Handshake applied: enabled=%d short=%d max=%d", (int)enabled, enabled_songs, Rule_Max_Buffs);
    return true;
}

// --------------------------------------
// Helpers (match PR semantics)
// --------------------------------------
inline int BSP_ToBuffSlot(int i, int start_offset, int modulo) { return (i + start_offset) % modulo; }

static bool BSP_IsStackBlocked(EQCHARINFO* player, _EQSPELLINFO* spell) {
    return (spell && spell->IsBardsong()) ? false : EQ_Character::IsStackBlocked(player, spell);
}

static int BSP_SpellAffectIndex(_EQSPELLINFO* spell, int effectType) {
    // allow Selo’s to stack with regular movement effects
    return (effectType == SE_MovementSpeed && spell->IsBeneficial() && spell->IsBardsong())
         ? 0
         : EQ_Spell::SpellAffectIndex(spell, effectType);
}

// --------------------------------------
// Main BSP logic (verbatim from your PR, structurally identical)
// --------------------------------------
extern "C" _EQBUFFINFO* BSP_FindAffectSlot(EQCHARINFO* player, WORD spellid, _EQSPAWNINFO* caster, DWORD* result_buffslot, int dry_run)
{
    *result_buffslot = -1;
    if (!caster || !EQ_Spell::IsValidSpellIndex(spellid))
        return 0;

    EQSPELLINFO* new_spell = EQ_Spell::GetSpell(spellid);
    if (!new_spell || !caster->Type && BSP_IsStackBlocked(player, new_spell))
        return 0;

    int MaxTotalBuffs = Rule_Max_Buffs;
    int StartBuffOffset = 0;
    int MaxSelectableBuffs = EQ_NUM_BUFFS;

    if (Rule_Num_Short_Buffs > 0 && EQ_Spell::IsShortBuffBox(spellid)) {
        StartBuffOffset = EQ_NUM_BUFFS;
        MaxSelectableBuffs = MaxTotalBuffs;
    }

    WORD old_buff_spell_id = 0;
    EQBUFFINFO* old_buff = 0;
    EQSPELLINFO* old_spelldata = 0;
    int cur_slotnum7 = 0;
    int cur_slotnum7_buffslot = BSP_ToBuffSlot(cur_slotnum7, StartBuffOffset, MaxSelectableBuffs);
    BYTE new_buff_effect_id2 = 0;
    int effect_slot_num = 0;
    bool no_slot_found_yet = true;
    bool old_effect_is_negative_or_zero = false;
    bool old_effect_value_is_negative_or_zero = false;
    bool is_bard_song = new_spell->IsBardsong();
    bool is_movement_effect = BSP_SpellAffectIndex(new_spell, SE_MovementSpeed) != 0;
    short old_effect_value;
    short new_effect_value;

    if (is_bard_song)
    {
        for (int i = 0; i < MaxTotalBuffs; i++)
        {
            int buffslot = i;
            EQBUFFINFO* buff = EQ_Character::GetBuff(player, buffslot);
            if (buff->BuffType)
            {
                WORD buff_spell_id = buff->SpellId;
                if (EQ_Spell::IsValidSpellIndex(buff_spell_id))
                {
                    EQSPELLINFO* buff_spell = EQ_Spell::GetSpell(buff_spell_id);
                    if (buff_spell
                        && !buff_spell->IsBardsong()
                        && !buff_spell->IsBeneficial()
                        && new_spell->IsBeneficial()
                        && is_movement_effect
                        && (BSP_SpellAffectIndex(buff_spell, SE_MovementSpeed) != 0 || BSP_SpellAffectIndex(buff_spell, SE_Root) != 0))
                    {
                        *result_buffslot = -1;
                        return 0;
                    }
                }
            }
        }
    }

    bool can_multi_stack = EQ_Spell::CanSpellStackMultipleTimes(new_spell);
    bool spell_id_already_affecting_target = false;

    for (int i = 0; i < MaxSelectableBuffs; i++) {
        int buffslot = BSP_ToBuffSlot(i, StartBuffOffset, MaxSelectableBuffs);
        EQBUFFINFO* buff = EQ_Character::GetBuff(player, buffslot);
        if (buff->BuffType)
        {
            WORD buff_spell_id = buff->SpellId;
            if (buff_spell_id == spellid)
            {
                EQSPAWNINFO* SpawnInfo = player->SpawnInfo;
                if (!SpawnInfo || caster->Type != EQ_SPAWN_TYPE_PLAYER || SpawnInfo->Type != EQ_SPAWN_TYPE_NPC)
                    goto OVERWRITE_SAME_SPELL_WITHOUT_REMOVING_FIRST;
                if (buff_spell_id == 2755) // Lifeburn
                    can_multi_stack = false;
                if (!can_multi_stack || caster->SpawnId == player->BuffCasterId[buffslot])
                {
                OVERWRITE_SAME_SPELL_WITHOUT_REMOVING_FIRST:
                    if (caster->Level >= buff->CasterLevel
                        && BSP_SpellAffectIndex(new_spell, 67) == 0
                        && BSP_SpellAffectIndex(new_spell, 101) == 0
                        && BSP_SpellAffectIndex(new_spell, 113) == 0)
                    {
                        *result_buffslot = buffslot;
                        return buff;
                    }
                    else
                    {
                        *result_buffslot = -1;
                        return 0;
                    }
                }
                spell_id_already_affecting_target = true;
            }
        }
    }

    if (can_multi_stack)
    {
        if (spell_id_already_affecting_target)
        {
            int first_open_buffslot = -1;
            for (int i = 0; i < MaxSelectableBuffs; i++) {
                int buffslot = BSP_ToBuffSlot(i, StartBuffOffset, MaxSelectableBuffs);
                EQBUFFINFO* buff = EQ_Character::GetBuff(player, buffslot);
                if (buff->BuffType)
                {
                    if (spellid == buff->SpellId && player->SpawnInfo)
                    {
                        WORD buff_caster_id = player->BuffCasterId[buffslot];
                        EQSPAWNINFO* buff_caster = nullptr;
                        if (buff_caster_id && buff_caster_id < 0x1388u)
                            buff_caster = EQPlayer::GetSpawn(buff_caster_id);
                        if (!buff_caster || buff_caster == player->SpawnInfo)
                        {
                            *result_buffslot = buffslot;
                            return buff;
                        }
                    }
                }
                else if (first_open_buffslot == -1)
                {
                    first_open_buffslot = buffslot;
                }
            }
            if (first_open_buffslot != -1)
            {
                *result_buffslot = first_open_buffslot;
                return EQ_Character::GetBuff(player, first_open_buffslot);
            }
        }
    }

    if (false)
    {
    STACK_OK_OVERWRITE_BUFF_IF_NEEDED:
        if (*result_buffslot != -1)
        {
            EQBUFFINFO* buff = EQ_Character::GetBuff(player, *result_buffslot);
            if (!dry_run && buff->BuffType && spellid != buff->SpellId)
            {
                EQ_Character::RemoveBuff(player, buff, 0);
            }
            return buff;
        }
        if (!new_spell->IsBeneficial())
        {
            EQSPAWNINFO* self = player->SpawnInfo;
            if (self)
            {
                if (!self->IsGameMaster)
                {
                    int curbuff_i = 0;
                    int curbuff_slot = BSP_ToBuffSlot(curbuff_i, StartBuffOffset, MaxSelectableBuffs);

                    while (1)
                    {
                        WORD buff_spell_id = EQ_Character::GetBuff(player, curbuff_slot)->SpellId;
                        if (EQ_Spell::IsValidSpellIndex(buff_spell_id))
                        {
                            EQSPELLINFO* buff_spell = EQ_Spell::GetSpell(buff_spell_id);
                            if (buff_spell && buff_spell->IsBeneficial())
                                break;
                        }
                        if (++curbuff_i >= MaxSelectableBuffs)
                            return 0;
                        curbuff_slot = BSP_ToBuffSlot(curbuff_i, StartBuffOffset, MaxSelectableBuffs);
                    }
                    if (!dry_run)
                    {
                        EQBUFFINFO* buff = EQ_Character::GetBuff(player, curbuff_slot);
                        EQ_Character::RemoveBuff(player, buff, 0);
                    }
                    *result_buffslot = curbuff_slot;
                    goto RETURN_RESULT_SLOTNUM_194;
                }
            }
        }
        return 0;
    }

    while (1)
    {
        old_buff = EQ_Character::GetBuff(player, cur_slotnum7_buffslot);
        if (!old_buff->BuffType)
            goto STACK_OK4;

        old_buff_spell_id = old_buff->SpellId;
        if (EQ_Spell::IsValidSpellIndex(old_buff_spell_id))
        {
            old_spelldata = EQ_Spell::GetSpell(old_buff_spell_id);
            if (old_spelldata)
                break;
        }
        old_buff->BuffType = 0;
        old_buff->SpellId = -1;
        old_buff->CasterLevel = 0;
        old_buff->Ticks = 0;
        old_buff->Modifier = 0;
        old_buff->Counters = 0;
    STACK_OK4:
        no_slot_found_yet = *result_buffslot == -1;
    STACK_OK3:
        if (no_slot_found_yet)
        {
        STACK_OK2:
            *result_buffslot = cur_slotnum7_buffslot;
        }
    STACK_OK:
        if (++cur_slotnum7 >= MaxSelectableBuffs)
        {
            goto STACK_OK_OVERWRITE_BUFF_IF_NEEDED;
        }
        cur_slotnum7_buffslot = BSP_ToBuffSlot(cur_slotnum7, StartBuffOffset, MaxSelectableBuffs);
    }
    if (is_bard_song && !old_spelldata->IsBardsong())
    {
        if (new_spell->IsBeneficial() && is_movement_effect && BSP_SpellAffectIndex(old_spelldata, SE_MovementSpeed) != 0
            || new_spell->IsBeneficial() && is_movement_effect && BSP_SpellAffectIndex(old_spelldata, SE_Root) != 0)
        {
            goto BLOCK_BUFF_178;
        }

        if (is_bard_song)
            goto STACK_OK;
    }

    if (new_spell->IsBeneficial())
    {
        if (old_spelldata->IsBeneficial())
        {
            if (is_movement_effect)
            {
                if (BSP_SpellAffectIndex(old_spelldata, SE_MovementSpeed) != 0)
                {
                    if (old_spelldata->IsBardsong() && !is_bard_song)
                        goto BLOCK_BUFF_178;
                }
            }
        }
    }

    effect_slot_num = 0;
    while (2)
    {
        BYTE old_buff_effect_id = old_spelldata->Attribute[effect_slot_num];
        if (old_buff_effect_id == SE_Blank)
            goto STACK_OK;

        BYTE new_buff_effect_id = new_spell->Attribute[effect_slot_num];
        if (new_buff_effect_id == SE_Blank)
            goto STACK_OK;

        if (new_buff_effect_id == SE_Lycanthropy || new_buff_effect_id == SE_Vampirism)
            goto BLOCK_BUFF_178;

        if ((!is_bard_song && old_spelldata->IsBardsong())
            || old_buff_effect_id != new_buff_effect_id
            || EQ_Spell::IsSPAIgnoredByStacking(new_buff_effect_id))
        {
            if (++effect_slot_num >= EQ_NUM_SPELL_EFFECTS)
                goto STACK_OK;
            continue;
        }

        if (new_buff_effect_id == SE_CurrentHP || new_buff_effect_id == SE_ArmorClass)
        {
            if (new_spell->Base[effect_slot_num] >= 0)
                break;
            if (++effect_slot_num >= EQ_NUM_SPELL_EFFECTS)
                goto STACK_OK;
            continue;
        }
        if (new_buff_effect_id == SE_CHA)
        {
            if (new_spell->Base[effect_slot_num] == 0 || old_spelldata->Base[effect_slot_num] == 0)
            {
            NEXT_ATTRIB_107:
                if (++effect_slot_num >= EQ_NUM_SPELL_EFFECTS)
                    goto STACK_OK;
                continue;
            }
        }
        break;
    }

    if (new_spell->IsBeneficial() && (!old_spelldata->IsBeneficial() || BSP_SpellAffectIndex(old_spelldata, SE_Illusion) != 0)
        || old_spelldata->Attribute[effect_slot_num] == SE_CompleteHeal
        || old_buff_spell_id >= 775 && old_buff_spell_id <= 785
        || old_buff_spell_id >= 1200 && old_buff_spell_id <= 1250
        || old_buff_spell_id >= 1900 && old_buff_spell_id <= 1924
        || old_buff_spell_id == 2079
        || old_buff_spell_id == 2751
        || old_buff_spell_id == 756
        || old_buff_spell_id == 757
        || old_buff_spell_id == 836)
    {
        goto BLOCK_BUFF_178;
    }

    old_effect_value = EQ_Character::CalcSpellEffectValue(player, old_spelldata, old_buff->CasterLevel, effect_slot_num, 0);
    new_effect_value = EQ_Character::CalcSpellEffectValue(player, new_spell, caster->Level, effect_slot_num, 0);

    if (spellid == 1620 || spellid == 1816 || spellid == 833 || old_buff_spell_id == 1814)
        new_effect_value = -1;
    if (old_buff_spell_id == 1620 || old_buff_spell_id == 1816 || old_buff_spell_id == 833 || old_buff_spell_id == 1814)
        old_effect_value = -1;
    old_effect_is_negative_or_zero = old_effect_value <= 0;
    if (old_effect_value >= 0)
    {
    OVERWRITE_INCREASE_WITH_DECREASE_137:
        if (!old_effect_is_negative_or_zero && new_effect_value < 0)
            goto OVERWRITE_INCREASE_WITH_DECREASE_166;
        bool is_disease_cloud = (spellid == 836);
        if (new_spell->Attribute[effect_slot_num] == SE_AttackSpeed)
        {
            if (new_effect_value < 100 && new_effect_value <= old_effect_value)
                goto OVERWRITE_150;
            if (old_effect_value <= 100)
                goto BLOCKED_BUFF_151;
            if (new_effect_value >= 100)
            {
            OVERWRITE_IF_GREATER_BLOCK_OTHERWISE_149:
                if (new_effect_value >= old_effect_value)
                    goto OVERWRITE_150;
            BLOCKED_BUFF_151:
                if (!is_disease_cloud)
                    goto BLOCK_BUFF_178;
                if (!new_spell->IsBeneficial() && !old_spelldata->IsBeneficial())
                {
                    *result_buffslot = cur_slotnum7_buffslot;
                    if (!dry_run && spellid != old_buff->SpellId)
                        goto OVERWRITE_REMOVE_FIRST_170;
                    goto RETURN_RESULT_SLOTNUM_194;
                }
                if (*result_buffslot == -1)
                    goto STACK_OK2;
                no_slot_found_yet = EQ_Character::GetBuff(player, *result_buffslot)->BuffType == 0;
                goto STACK_OK3;
            }
        OVERWRITE_150:
            is_disease_cloud = 1;
            goto BLOCKED_BUFF_151;
        }
        old_effect_value_is_negative_or_zero = old_effect_value <= 0;
        if (old_effect_value < 0)
        {
            if (new_effect_value <= old_effect_value)
                goto OVERWRITE_150;
            old_effect_value_is_negative_or_zero = old_effect_value <= 0;
        }
        if (old_effect_value_is_negative_or_zero)
            goto BLOCKED_BUFF_151;
        goto OVERWRITE_IF_GREATER_BLOCK_OTHERWISE_149;
    }
    if (new_effect_value <= 0)
    {
        old_effect_is_negative_or_zero = old_effect_value <= 0;
        goto OVERWRITE_INCREASE_WITH_DECREASE_137;
    }
OVERWRITE_INCREASE_WITH_DECREASE_166:
    new_buff_effect_id2 = new_spell->Attribute[effect_slot_num];
    if (new_buff_effect_id2 != SE_MovementSpeed)
    {
        if (new_buff_effect_id2 != SE_CurrentHP || old_effect_value >= 0 || new_effect_value <= 0)
            goto USE_CURRENT_BUFF_SLOT;
    BLOCK_BUFF_178:
        *result_buffslot = -1;
        return 0;
    }
    if (new_effect_value >= 0)
        goto BLOCK_BUFF_178;
USE_CURRENT_BUFF_SLOT:
    *result_buffslot = cur_slotnum7_buffslot;
    if (!dry_run && spellid != old_buff->SpellId)
    {
    OVERWRITE_REMOVE_FIRST_170:
        EQBUFFINFO* buff = EQ_Character::GetBuff(player, cur_slotnum7_buffslot);
        EQ_Character::RemoveBuff(player, buff, 0);
        return buff;
    }
RETURN_RESULT_SLOTNUM_194:
    return EQ_Character::GetBuff(player, *result_buffslot);
}

// --------------------------------------
// Detours
// --------------------------------------

// EQCharacter::FindAffectSlot
typedef _EQBUFFINFO* (__thiscall* tFindAffectSlot)(EQCHARINFO*, WORD, _EQSPAWNINFO*, DWORD*, int);
static tFindAffectSlot EQCharacter__FindAffectSlot_Trampoline = nullptr;

static _EQBUFFINFO* __fastcall EQCharacter__FindAffectSlot_Detour(EQCHARINFO* player, int /*unused*/, WORD spellid, _EQSPAWNINFO* caster, DWORD* out_slot, int flag) {
    if (Rule_Buffstacking_Patch_Enabled) {
        return BSP_FindAffectSlot(player, spellid, caster, out_slot, flag);
    }
    return EQCharacter__FindAffectSlot_Trampoline ? EQCharacter__FindAffectSlot_Trampoline(player, spellid, caster, out_slot, flag) : nullptr;
}

// EQCharacter::GetMaxBuffs
typedef int(__thiscall* tGetMaxBuffs)(EQCHARINFO*);
static tGetMaxBuffs EQCharacter__GetMaxBuffs_Trampoline = nullptr;

static int __fastcall EQCHARACTER__GetMaxBuffs_Detour(EQCHARINFO* player, int /*unused*/) {
    EQSPAWNINFO* spawn_info;
    if (player && (spawn_info = player->SpawnInfo) != 0) {
        if (spawn_info->Type == EQ_SPAWN_TYPE_PLAYER) return Rule_Max_Buffs;
        if (spawn_info->Type == EQ_SPAWN_TYPE_NPC)    return 30;
    }
    return EQ_NUM_BUFFS;
}

// EQCharacter::GetBuff
typedef _EQBUFFINFO* (__thiscall* tGetBuff)(EQCHARINFO*, int);
static tGetBuff EQCharacter__GetBuff_Trampoline = nullptr;

static _EQBUFFINFO* __fastcall EQCharacter__GetBuff_Detour(EQCHARINFO* player, int /*unused*/, int buff_slot) {
    if (ShortBuffSupport_ReturnSongBuffs && buff_slot < 15) {
        buff_slot += 15;
    }
    return EQCharacter__GetBuff_Trampoline ? EQCharacter__GetBuff_Trampoline(player, buff_slot) : nullptr;
}

// CBuffWindow::WndNotification
using tWndNotification = int(__thiscall*)(CSidlScreenWnd*, void*, int, int);
static tWndNotification CBuffWindow__WndNotification_Trampoline = nullptr;

static int __fastcall CBuffWindow__WndNotification_Detour(CBuffWindow* self, int /*unused*/, void* sender, int type, int a4)
{
    bool is_song_window = (self == GetShortDurationBuffWindow());
    int  start_buff_index = is_song_window ? 15 : 0;

    if (type != 1)
    {
        if (type != 23 && type != 25)
            return CSidlScreenWnd::WndNotification(self, sender, type, a4);
LABEL_11:
        MakeGetBuffReturnSongs(is_song_window);
        self->HandleSpellInfoDisplay(sender);
        MakeGetBuffReturnSongs(false);
        return CSidlScreenWnd::WndNotification(self, sender, type, a4);
    }
    if (EQ_IsKeyPressedAlt())
        goto LABEL_11;

    // Click-off (requires CharInfo)
    if (EQ_OBJECT_CharInfo) {
        for (int i = 0; i < EQ_NUM_BUFFS; i++) {
            if (self->Data.BuffButtonWnd[i] == sender) {
                int slot = i + start_buff_index;
                if (EQ_Character::IsValidAffect(EQ_OBJECT_CharInfo, slot))
                    EQ_Character::RemoveMyAffect(EQ_OBJECT_CharInfo, (short)slot);
                return CSidlScreenWnd::WndNotification(self, sender, type, a4);
            }
        }
    }
    return CSidlScreenWnd::WndNotification(self, sender, type, a4);
}

// --------------------------------------
// OP_Buff for-loop bound patch: cmp 15 -> cmp 30
// --------------------------------------
static void apply_op_buff_patch(uint8_t* base) {
    uint8_t* addr = base + ADDR_OP_BuffLoopBound_cmp15 + 2; // byte that holds 0x0F
    DWORD oldp{};
    if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldp)) {
        if (*addr == 0x0F) {
            *addr = 0x1E;
            logf("Patched OP_Buff bound 15->30 @ %p", addr);
        } else {
            logf("OP_Buff bound already != 15 (0x%02X); skipping", *addr);
        }
        VirtualProtect(addr, 1, oldp, &oldp);
    } else {
        logf("VirtualProtect failed for OP_Buff patch");
    }
}

// --------------------------------------
// Init
// --------------------------------------
static DWORD WINAPI init_thread(LPVOID)
{
    MH_Initialize();

    HMODULE h = GetModuleHandleW(L"eqgame.exe");
    auto [base, size] = module_range(h);
    if (!base) { logf("eqgame.exe base not found"); return 0; }

    // Resolve originals (RVA from contract header)
    EQCharacter__FindAffectSlot_Trampoline = (tFindAffectSlot)(base + ADDR_EQCharacter_FindAffectSlot);
    EQCharacter__GetBuff_Trampoline        = (tGetBuff)       (base + ADDR_EQCharacter_GetBuff);
    EQCharacter__GetMaxBuffs_Trampoline    = (tGetMaxBuffs)   (base + ADDR_EQCharacter_GetMaxBuffs);
    CBuffWindow__WndNotification_Trampoline= (tWndNotification)(base + ADDR_CBuffWindow_WndNotification);

    // Install hooks
    hook_raw((void*)EQCharacter__FindAffectSlot_Trampoline,
             (void*)&EQCharacter__FindAffectSlot_Detour,
             (void**)&EQCharacter__FindAffectSlot_Trampoline,
             "EQCharacter::FindAffectSlot");

    hook_raw((void*)EQCharacter__GetBuff_Trampoline,
             (void*)&EQCharacter__GetBuff_Detour,
             (void**)&EQCharacter__GetBuff_Trampoline,
             "EQCharacter::GetBuff");

    hook_raw((void*)EQCharacter__GetMaxBuffs_Trampoline,
             (void*)&EQCHARACTER__GetMaxBuffs_Detour,
             (void**)&EQCharacter__GetMaxBuffs_Trampoline,
             "EQCharacter::GetMaxBuffs");

    hook_raw((void*)CBuffWindow__WndNotification_Trampoline,
             (void*)&CBuffWindow__WndNotification_Detour,
             (void**)&CBuffWindow__WndNotification_Trampoline,
             "CBuffWindow::WndNotification");

    // Apply single-byte patch for OP_Buff
    apply_op_buff_patch(base);

    // Send version + handshake immediately (so zone hooks aren’t required)
    SendDllVersion_OnZone();
    BuffstackingPatch_OnZone();

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
