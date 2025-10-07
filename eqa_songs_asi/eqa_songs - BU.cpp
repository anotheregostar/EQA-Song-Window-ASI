#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <stdio.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <algorithm>
#include "detours.h"
#include "eqmac.h"
#include "eqmac_functions.h"

// Minimal chat helper for this ASI: logs to debugger; safe no-op in release
static void print_chat(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);

	// Show something readable in the debugger
	OutputDebugStringA("[EQA Songs] ");
	OutputDebugStringA(buf);
	OutputDebugStringA("\n");

	// If you later have an in-game chat API, call it here instead.
}

// externs used by the /songs toggle (provided elsewhere in your project)
// If you already have these declared globally in another header, you can remove these lines.
//extern "C" DWORD WINAPI WritePrivateProfileStringA_tramp(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
//void __cdecl print_chat(const char* fmt, ...);

//------------------------------------------------------------------------
// Additions from eqgame.h
//------------------------------------------------------------------------

struct SpawnAppearance_Struct { // sizeof=0x8
    /*000*/ unsigned short spawn_id;
    /*002*/ unsigned short type;
    /*004*/ unsigned int parameter;
    /*008*/
};

// Custom Messaging Support
constexpr unsigned int SpawnAppearanceType_ClientDllMessage = 256;
void SendCustomSpawnAppearanceMessage(unsigned __int16 feature_id, unsigned __int16 feature_value, bool is_request);

// Song Window Support
__declspec(dllexport) class CShortBuffWindow* GetShortDurationBuffWindow();

//struct _EQBUFFINFO* GetStartBuffArray(class CBuffWindow* window);
struct _EQBUFFINFO* GetStartBuffArray(bool song_buffs);

//------------------------------------------------------------------------
// End of additions from eqgame.h
//------------------------------------------------------------------------

//------------------------------------------------------------------------
// Song Window and Handshake [Begin]
//------------------------------------------------------------------------

// ---------- Externs to your existing code in eqa_songs.cpp ----------
extern void ShortBuffWindow_InitUI(CDisplay* cdisplay);
extern void ShortBuffWindow_CleanUI();

// Show/hide helpers
extern void ShowBuffWindow_ActivateUI(char c);
extern void ShowBuffWindow_DeactivateUI();

extern void BuffstackingPatch_OnZone();
extern bool BuffstackingPatch_HandleHandshake(DWORD feature_id, DWORD feature_value, bool is_request);

extern void ApplySongWindowBytePatches();

// ---------- Globals referenced by handshake / rules ----------
bool Rule_Buffstacking_Patch_Enabled = false;
int  Rule_Max_Buffs                  = EQ_NUM_BUFFS; // raised by handshake
int  Rule_Num_Short_Buffs            = 0;            // set by handshake
int g_buffWindowTimersFontSize = 3; // default tooltip/overlay font size
bool g_bSongWindowAutoHide = false;


// ---------- Callback registries ----------
std::vector<std::function<void()>>            OnZoneCallbacks;
std::vector<std::function<void(CDisplay*)>>   InitGameUICallbacks;
std::vector<std::function<void()>>            CleanUpUICallbacks;
std::vector<std::function<void(char)>>        ActivateUICallbacks;
std::vector<std::function<void()>>            DeactivateUICallbacks;

// Custom SpawnAppearance (type 256) handlers
std::vector<std::function<bool(DWORD feature_id, DWORD feature_value, bool is_request)>> CustomSpawnAppearanceMessageHandlers;

// ---------- Tiny helpers used by WndNotification ----------
static inline bool CtrlPressed()  { return *(DWORD*)0x00809320 > 0; }
static inline bool AltPressed()   { return *(DWORD*)0x0080932C > 0; }
static inline bool ShiftPressed() { return *(DWORD*)0x0080931C > 0; }

// ---------- Patching helpers ----------
void PatchSwap(int target, BYTE* source, SIZE_T size, BYTE* buffer = nullptr)
{
    DWORD oldprotect;
    VirtualProtect((PVOID*)target, size, PAGE_EXECUTE_READWRITE, &oldprotect);
    if (buffer) memcpy((void*)buffer, (const void*)target, size);
    memcpy((void*)target, (const void*)source, size);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, size);
    VirtualProtect((PVOID*)target, size, oldprotect, &oldprotect);
}
// ---------- Custom SpawnAppearance sender/dispatcher + detour ----------
void SendCustomSpawnAppearanceMessage(unsigned __int16 feature_id, unsigned __int16 feature_value, bool is_request)
{
    DWORD id    = feature_id;
    DWORD value = feature_value;

    SpawnAppearance_Struct message{};
    message.type      = SpawnAppearanceType_ClientDllMessage; // server reads AppearanceType::ClientDllMessage
    message.spawn_id  = 0;
    message.parameter = (id << 16) | value;
    if (is_request) message.parameter &= 0x7FFFFFFFu;
    else            message.parameter |= 0x80000000u;

    // Uses Connection::SendMessage_ from eqmac_functions.h (OP 16629 is SpawnAppearance in this client)
    Connection::SendMessage_(16629, &message, sizeof(SpawnAppearance_Struct), 0);
}

void HandleCustomSpawnAppearanceMessage(SpawnAppearance_Struct* message)
{
    if (message->type == SpawnAppearanceType_ClientDllMessage && message->spawn_id == 0) {
        bool  is_request  = (message->parameter >> 31) == 0;
        DWORD feature_id  = (message->parameter >> 16) & 0x7FFFu;
        DWORD feature_val = (message->parameter      ) & 0xFFFFu;
        for (auto& handler : CustomSpawnAppearanceMessageHandlers) {
            if (handler(feature_id, feature_val, is_request)) return;
        }
    }
}

// eqgame HandleSpawnAppearanceMessage signature
typedef int(__thiscall* EQ_FUNCTION_TYPE_HandleSpawnAppearanceMessage)(void* this_ptr, int unk2, int opcode, SpawnAppearance_Struct* sa);
EQ_FUNCTION_TYPE_HandleSpawnAppearanceMessage HandleSpawnAppearanceMessage_Trampoline = nullptr;

int __fastcall HandleSpawnAppearanceMessage_Detour(void* this_ptr, int /*unused_edx*/, int unk2, int opcode, SpawnAppearance_Struct* sa)
{
    if (sa && sa->type >= SpawnAppearanceType_ClientDllMessage) {
        HandleCustomSpawnAppearanceMessage(sa);
        return 1; // handled
    }
    return HandleSpawnAppearanceMessage_Trampoline(this_ptr, unk2, opcode, sa);
}

// ---------- UI + zone lifecycle detours ----------
typedef void(__thiscall* EQ_FUNCTION_TYPE_EnterZone)(void* this_ptr, int hwnd);
EQ_FUNCTION_TYPE_EnterZone EnterZone_Trampoline = nullptr;
void __fastcall EnterZone_Detour(void* this_ptr, int /*unused_edx*/, int hwnd)
{
    EnterZone_Trampoline(this_ptr, hwnd);
    for (auto& cb : OnZoneCallbacks) cb();
}

typedef int(__thiscall* EQ_FUNCTION_TYPE_InitGameUI)(CDisplay* cdisplay);
EQ_FUNCTION_TYPE_InitGameUI InitGameUI_Trampoline = nullptr;
int __fastcall InitGameUI_Detour(CDisplay* cdisplay, int /*unused_edx*/)
{
    int res = InitGameUI_Trampoline(cdisplay);
    for (auto& cb : InitGameUICallbacks) cb(cdisplay);
    return res;
}

typedef void* (* EQ_FUNCTION_TYPE_CleanUpUI)(void);
EQ_FUNCTION_TYPE_CleanUpUI CleanUpUI_Trampoline = nullptr;
void* CleanUpUI_Detour()
{
    void* res = CleanUpUI_Trampoline();
    for (auto& cb : CleanUpUICallbacks) cb();
    return res;
}

typedef int(__stdcall* EQ_FUNCTION_TYPE_ActivateUI)(char);
EQ_FUNCTION_TYPE_ActivateUI ActivateUI_Trampoline = nullptr;
int __stdcall ActivateUI_Detour(char a1)
{
    int res = ActivateUI_Trampoline(a1);
    for (auto& cb : ActivateUICallbacks) cb(a1);
    return res;
}

typedef int (* EQ_FUNCTION_TYPE_DeactivateUI)(void);
EQ_FUNCTION_TYPE_DeactivateUI DeactivateUI_Trampoline = nullptr;
int DeactivateUI_Detour()
{
    int res = DeactivateUI_Trampoline();
    for (auto& cb : DeactivateUICallbacks) cb();
    return res;
}

// ---------- CEverQuest::InterpretCmd (for /songs toggle) ----------
struct EQPlayer; // forward

typedef int(__thiscall* EQ_FUNCTION_TYPE_CEverQuest__InterpretCmd)(void* this_ptr, EQPlayer* a1, char* a2);
EQ_FUNCTION_TYPE_CEverQuest__InterpretCmd EQMACMQ_REAL_CEverQuest__InterpretCmd = nullptr;

int __fastcall EQMACMQ_DETOUR_CEverQuest__InterpretCmd(void* this_ptr, void* /*not_used*/, EQPlayer* a1, char* a2)
{
	if (!a2) {
		return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, a1, a2);
	}

	// Normalize leading '//' to '/', like stock handler
	if (strncmp(a2, "//", 2) == 0) {
		memmove(a2, a2 + 1, strlen(a2));
	}

	if (strcmp(a2, "/songs") == 0) {
		g_bSongWindowAutoHide = !g_bSongWindowAutoHide;

		WritePrivateProfileStringA(
			"Defaults", "SongWindowAutoHide",
			g_bSongWindowAutoHide ? "TRUE" : "FALSE",
			"./eqclient.ini"
		);

		//// Persist to INI
		//WritePrivateProfileStringA_tramp(
		//	"Defaults", "SongWindowAutoHide",
		//	g_bSongWindowAutoHide ? "TRUE" : "FALSE",
		//	"./eqclient.ini"
		//);

		// Feedback
		print_chat("Song Window auto-hide: %s.", g_bSongWindowAutoHide ? "ON" : "OFF");

		// If we just turned auto-hide OFF, and the window is hidden, show it now
		if (!g_bSongWindowAutoHide && GetShortDurationBuffWindow() &&
			!GetShortDurationBuffWindow()->IsVisibile())
		{
			GetShortDurationBuffWindow()->Show(1, 1);
		}

		return 0; // handled
	}

	return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, a1, a2);
}


// ---------- Song label provider (Song1..Song15) ----------
typedef bool(__cdecl* EQ_FUNCTION_TYPE_GetLabelFromEQ)(int EqType, PEQCXSTR* str, bool* override_color, DWORD* color);
EQ_FUNCTION_TYPE_GetLabelFromEQ GetLabelFromEQ_Trampoline = nullptr;

bool __cdecl GetLabelFromEQ_Detour(int EqType, PEQCXSTR* str, bool* override_color, DWORD* color)
{
    switch (EqType) {
        case 135: case 136: case 137: case 138: case 139:
        case 140: case 141: case 142: case 143: case 144:
        case 145: case 146: case 147: case 148: case 149:
        {
            *override_color = false;
            if (EQ_OBJECT_CharInfo) {
                EQBUFFINFO& buff = ((PEQCHARINFO)EQ_OBJECT_CharInfo)->BuffsExt[EqType - 135];
                if (EQ_Spell::IsValidSpellIndex(buff.SpellId)) {
                    if (auto* spell = EQ_Spell::GetSpell(buff.SpellId)) {
                        EQ_CXStr_Set(str, spell->Name);
                        return true;
                    }
                }
            }
            EQ_CXStr_Set(str, "");
            return true;
        }
    }
    return GetLabelFromEQ_Trampoline(EqType, str, override_color, color);
}

// ===== Forward declarations needed by the hook installer below =====
//struct EQCHARINFO;
struct _EQBUFFINFO;

// function-pointer typedefs used in hook installer casts
typedef _EQBUFFINFO* (__thiscall* EQ_FUNCTION_TYPE_EQCharacter__GetBuff)(EQCHARINFO* /*this_char_info*/, int /*buff_slot*/);
typedef int(__thiscall* EQ_FUNCTION_TYPE_EQCharacter__GetMaxBuffs)(EQCHARINFO* /*this_ptr*/);

// Trampoline variables (declared here, defined later at ~864 and ~881)
extern _EQBUFFINFO* (__thiscall* EQCharacter__GetBuff_Trampoline)(EQCHARINFO* /*this_char_info*/, int /*buff_slot*/);
extern int(__thiscall* EQCharacter__GetMaxBuffs_Trampoline)(EQCHARINFO* /*this_ptr*/);

// Detour function prototypes (definitions are later in the file)
_EQBUFFINFO* __fastcall EQCharacter__GetBuff_Detour(EQCHARINFO* player, int /*unused*/, WORD buff_slot);
int          __fastcall EQCHARACTER__GetMaxBuffs_Detour(EQCHARINFO* player, int /*unused*/);

// potentially needed, testing
//extern thread_local bool ShortBuffSupport_ReturnSongBuffs;
// Forward declaration so we can read/restore the flag above its definition
extern thread_local bool ShortBuffSupport_ReturnSongBuffs;

// Forward-declare; we’ll define it later (after the handshake constants)
static inline void EnsureBuffstackingHandshakeSentOnce();

// WndNotification detour prototype (definition is later at ~890)
int __fastcall CBuffWindow__WndNotification_Detour(CBuffWindow* self, int /*unused*/, PEQCBUFFBUTTONWND sender, int type, int a4);

// Helpers referenced earlier in the file, defined later at ~855–861
_EQBUFFINFO* GetStartBuffArray(bool song_buffs);
void MakeGetBuffReturnSongs(bool enable);


//// ---------- CBuffWindow draw detours ----------
//typedef void (__fastcall* EQ_FUNCTION_TYPE_CBuffWindow__RefreshBuffDisplay)(CBuffWindow* this_ptr, void*);
//typedef int  (__fastcall* EQ_FUNCTION_TYPE_CBuffWindow__PostDraw)          (CBuffWindow* this_ptr, void*);
//typedef int  (__fastcall* EQ_FUNCTION_TYPE_EQ_Character__CastSpell)        (void* this_ptr, void* not_used, unsigned char a1, short a2, EQITEMINFO** a3, short a4);

// ---------- CBuffWindow draw trampolines (match eqmac_functions.h) ----------
//typedef void(__thiscall* EQ_FUNCTION_TYPE_CBuffWindow__RefreshBuffDisplay)(CBuffWindow* this_ptr);
//typedef int(__thiscall* EQ_FUNCTION_TYPE_CBuffWindow__PostDraw)          (CBuffWindow* this_ptr);
typedef int(__thiscall* EQ_FUNCTION_TYPE_EQ_Character__CastSpell)        (void* this_ptr,
	unsigned char gem, short spellId,
	EQITEMINFO** item, short unknown);

EQ_FUNCTION_TYPE_CBuffWindow__RefreshBuffDisplay EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay = nullptr;
EQ_FUNCTION_TYPE_CBuffWindow__PostDraw           EQMACMQ_REAL_CBuffWindow__PostDraw           = nullptr;
EQ_FUNCTION_TYPE_EQ_Character__CastSpell         EQMACMQ_REAL_EQ_Character__CastSpell         = nullptr;

void __fastcall EQMACMQ_DETOUR_CBuffWindow__RefreshBuffDisplay(CBuffWindow* this_ptr, void* /*not_used*/)
{
    auto* buffWindow = (PEQCBUFFWINDOW)this_ptr;
    auto* charInfo   = (PEQCHARINFO)EQ_OBJECT_CharInfo;
    if (!charInfo) return;

    bool is_song_window = (this_ptr == GetShortDurationBuffWindow());
    _EQBUFFINFO* buffs  = GetStartBuffArray(is_song_window);

    // make GetBuff read songs
    MakeGetBuffReturnSongs(is_song_window);
	EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay(this_ptr);
    MakeGetBuffReturnSongs(false);

    // (tooltips: inject tick countdown into tooltip + draw at cursor)
    for (size_t i = 0; i < EQ_NUM_BUFFS; ++i) {
        EQBUFFINFO& buff = buffs[i];
        if (!EQ_Spell::IsValidSpellIndex(buff.SpellId) || buff.BuffType == 0) continue;
        if (buff.Ticks == 0) continue;

        auto* btn = buffWindow->BuffButtonWnd[i];
        if (btn && btn->CSidlWnd.EQWnd.ToolTipText) {
            char buffTickTimeText[128];
            EQ_GetShortTickTimeString(buff.Ticks, buffTickTimeText, sizeof(buffTickTimeText));

            // Enlarge tooltip font and draw updated text at the button
            btn->CSidlWnd.EQWnd.FontPointer->Size = g_buffWindowTimersFontSize;

            char original[128];
            strncpy_s(original, sizeof(original), btn->CSidlWnd.EQWnd.ToolTipText->Text, _TRUNCATE);

            EQ_CXStr_Set(&btn->CSidlWnd.EQWnd.ToolTipText, buffTickTimeText);
            CXRect r = ((CXWnd*)btn)->GetScreenRect();
            ((CXWnd*)btn)->DrawTooltipAtPoint(r.X1, r.Y1);
            EQ_CXStr_Set(&btn->CSidlWnd.EQWnd.ToolTipText, original);
        }
    }

	// ---- Auto-show/auto-hide behaviour for the Song (Short Buff) window ----
	if (is_song_window) {
		int num_buffs = 0;
		for (size_t i = 0; i < EQ_NUM_BUFFS; ++i) {
			const EQBUFFINFO& b = buffs[i];
			if (EQ_Spell::IsValidSpellIndex(b.SpellId) && b.BuffType != 0) {
				++num_buffs;
			}
		}

		if (this_ptr->IsVisibile()) {
			// Visible: hide if empty and user chose auto-hide, or if feature is off
			if ((num_buffs == 0 && g_bSongWindowAutoHide) || Rule_Num_Short_Buffs == 0) {
				this_ptr->Show(0, 1);
			}
		}
		else {
			// Hidden: show as soon as we have any song buffs
			if (num_buffs > 0) {
				this_ptr->Show(1, 1);
			}
		}
	}
}

int __fastcall EQMACMQ_DETOUR_CBuffWindow__PostDraw(CBuffWindow* this_ptr, void* /*not_used*/)
{
	int result = EQMACMQ_REAL_CBuffWindow__PostDraw(this_ptr);
    auto* buffWindow = (PEQCBUFFWINDOW)this_ptr;
    auto* charInfo   = (PEQCHARINFO)EQ_OBJECT_CharInfo;
    if (!charInfo) return result;

    bool is_song_window = (this_ptr == GetShortDurationBuffWindow());
    _EQBUFFINFO* buffs  = GetStartBuffArray(is_song_window);

    for (size_t i = 0; i < EQ_NUM_BUFFS; ++i) {
        EQBUFFINFO& buff = buffs[i];
        if (!EQ_Spell::IsValidSpellIndex(buff.SpellId) || buff.BuffType == 0) continue;
        if (buff.Ticks == 0) continue;

        char buffTimeText[128];
        EQ_GetShortTickTimeString(buff.Ticks, buffTimeText, sizeof(buffTimeText));

        auto* btn = buffWindow->BuffButtonWnd[i];
        if (btn && btn->CSidlWnd.EQWnd.ToolTipText) {
            btn->CSidlWnd.EQWnd.FontPointer->Size = g_buffWindowTimersFontSize;

            char original[128];
            strncpy_s(original, sizeof(original), btn->CSidlWnd.EQWnd.ToolTipText->Text, _TRUNCATE);

            EQ_CXStr_Set(&btn->CSidlWnd.EQWnd.ToolTipText, buffTimeText);
            CXRect r = ((CXWnd*)btn)->GetScreenRect();
            ((CXWnd*)btn)->DrawTooltipAtPoint(r.X1, r.Y1);
            EQ_CXStr_Set(&btn->CSidlWnd.EQWnd.ToolTipText, original);
        }
    }

	// ---- Auto-show/auto-hide behaviour for the Song (Short Buff) window ----
	if (is_song_window) {
		int num_buffs = 0;
		for (size_t i = 0; i < EQ_NUM_BUFFS; ++i) {
			const EQBUFFINFO& b = buffs[i];
			if (EQ_Spell::IsValidSpellIndex(b.SpellId) && b.BuffType != 0) {
				++num_buffs;
			}
		}

		if (this_ptr->IsVisibile()) {
			// Visible: hide if empty and user chose auto-hide, or if feature is off
			if ((num_buffs == 0 && g_bSongWindowAutoHide) || Rule_Num_Short_Buffs == 0) {
				this_ptr->Show(0, 1);
			}
		}
		else {
			// Hidden: show as soon as we have any song buffs
			if (num_buffs > 0) {
				this_ptr->Show(1, 1);
			}
		}
	}

    return result;
}

int __fastcall EQMACMQ_DETOUR_EQ_Character__CastSpell(void* self, void* /*edx*/,
	unsigned char gem, short spellId, EQITEMINFO** item, short unknown)
{
	int r = EQMACMQ_REAL_EQ_Character__CastSpell(self, gem, spellId, item, unknown);

	// Refresh the song window so routing is visible immediately,
	// but restore the previous routing flag afterwards to avoid
	// leaking the 'song view' into the main buff window.
	if (auto* songWnd = (CBuffWindow*)GetShortDurationBuffWindow()) {
		const bool prev = ShortBuffSupport_ReturnSongBuffs;
		MakeGetBuffReturnSongs(true);
		if (EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay) {
			EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay(songWnd);
		}
		MakeGetBuffReturnSongs(prev);
	}

	return r;
}

//int __fastcall EQMACMQ_DETOUR_EQ_Character__CastSpell(void* self, void* /*edx*/,
//	unsigned char gem, short spellId,
//	EQITEMINFO** item, short unknown)
//{
//	int r = EQMACMQ_REAL_EQ_Character__CastSpell(self, gem, spellId, item, unknown);
//
//	// Force a song-window-aware refresh right after the cast so routing shows immediately.
//	if (auto* songWnd = (CBuffWindow*)GetShortDurationBuffWindow()) {
//		const bool prev = ShortBuffSupport_ReturnSongBuffs;  // save current state
//		MakeGetBuffReturnSongs(true);
//		if (EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay) {
//			EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay(songWnd);
//		}
//		MakeGetBuffReturnSongs(prev);                        // restore exactly
//	}
//	return r;
//}

//// potentially needed, testing
//int __fastcall EQMACMQ_DETOUR_EQ_Character__CastSpell(void* self, void* /*edx*/,
//	unsigned char gem, short spellId,
//	EQITEMINFO** item, short unknown)
//{
//	int r = EQMACMQ_REAL_EQ_Character__CastSpell(self, gem, spellId, item, unknown);
//
//	// Refresh the song window so routing is visible immediately,
//	// but restore the previous routing flag afterwards to avoid
//	// leaking the 'song view' into the main buff window.
//	if (auto* songWnd = (CBuffWindow*)GetShortDurationBuffWindow()) {
//		const bool prev = ShortBuffSupport_ReturnSongBuffs;  // save current state
//		MakeGetBuffReturnSongs(true);
//		EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay(songWnd);
//		MakeGetBuffReturnSongs(prev);                        // restore exactly
//	}
//	return r;
//}

// ---------- Hook installer ----------
static bool g_songHooksInstalledOnce = false;

void InitHooks_SongWindow()
{
    if (g_songHooksInstalledOnce) return;

    // (A) Core song-window detours already implemented elsewhere
    //    Addresses pulled from your working eqgame.cpp for this client build.
    EQCharacter__GetBuff_Trampoline     = (EQ_FUNCTION_TYPE_EQCharacter__GetBuff)     DetourFunction((PBYTE)0x004C465A, (PBYTE)EQCharacter__GetBuff_Detour);      // read buffs 16–30
    EQCharacter__GetMaxBuffs_Trampoline = (EQ_FUNCTION_TYPE_EQCharacter__GetMaxBuffs) DetourFunction((PBYTE)0x004C4637, (PBYTE)EQCHARACTER__GetMaxBuffs_Detour); // loops/stat calcs
    DetourFunction((PBYTE)0x00408FF1, (PBYTE)CBuffWindow__WndNotification_Detour); // clicking off songs, modifier logic

    // (B) OP_Buff loop-size fix
    ApplySongWindowBytePatches();

    // (C) Song button labels (Song1..Song15 captions)
    GetLabelFromEQ_Trampoline = (EQ_FUNCTION_TYPE_GetLabelFromEQ)DetourFunction((PBYTE)0x00436680, (PBYTE)GetLabelFromEQ_Detour);

    // (D) UI lifecycle + zone-in hooks (for building/destroying the short-buff window, sending handshakes)
    EnterZone_Trampoline    = (EQ_FUNCTION_TYPE_EnterZone)     DetourFunction((PBYTE)0x0053D2C4, (PBYTE)EnterZone_Detour);
    InitGameUI_Trampoline   = (EQ_FUNCTION_TYPE_InitGameUI)    DetourFunction((PBYTE)0x004A60B5, (PBYTE)InitGameUI_Detour);
    CleanUpUI_Trampoline    = (EQ_FUNCTION_TYPE_CleanUpUI)     DetourFunction((PBYTE)0x004A6EBC, (PBYTE)CleanUpUI_Detour);
    ActivateUI_Trampoline   = (EQ_FUNCTION_TYPE_ActivateUI)    DetourFunction((PBYTE)0x004A741B, (PBYTE)ActivateUI_Detour);
    DeactivateUI_Trampoline = (EQ_FUNCTION_TYPE_DeactivateUI)  DetourFunction((PBYTE)0x004A7705, (PBYTE)DeactivateUI_Detour);

	// (D.1) /songs toggle (CEverQuest::InterpretCmd)
	EQMACMQ_REAL_CEverQuest__InterpretCmd =
		(EQ_FUNCTION_TYPE_CEverQuest__InterpretCmd)DetourFunction(
			(PBYTE)EQ_FUNCTION_CEverQuest__InterpretCmd,  // from eqmac_functions.h
			(PBYTE)EQMACMQ_DETOUR_CEverQuest__InterpretCmd
		);

    // (E) Buff window render detours (tooltip/countdown & overlay)
    EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay = (EQ_FUNCTION_TYPE_CBuffWindow__RefreshBuffDisplay)DetourFunction((PBYTE)EQ_FUNCTION_CBuffWindow__RefreshBuffDisplay, (PBYTE)EQMACMQ_DETOUR_CBuffWindow__RefreshBuffDisplay);
    EQMACMQ_REAL_CBuffWindow__PostDraw           = (EQ_FUNCTION_TYPE_CBuffWindow__PostDraw)          DetourFunction((PBYTE)EQ_FUNCTION_CBuffWindow__PostDraw,           (PBYTE)EQMACMQ_DETOUR_CBuffWindow__PostDraw);
	
	// (E.1) One-shot refresh after cast so songs appear immediately
	EQMACMQ_REAL_EQ_Character__CastSpell =
		(EQ_FUNCTION_TYPE_EQ_Character__CastSpell)DetourFunction(
			(PBYTE)EQ_FUNCTION_EQ_Character__CastSpell,   // from eqmac_functions.h
			(PBYTE)EQMACMQ_DETOUR_EQ_Character__CastSpell
		);

	//// Potentially needed, testing
	//EQMACMQ_REAL_EQ_Character__CastSpell =
	//	(EQ_FUNCTION_TYPE_EQ_Character__CastSpell)DetourFunction(
	//		(PBYTE)EQ_FUNCTION_EQ_Character__CastSpell,   // or the raw address used in the PR
	//		(PBYTE)EQMACMQ_DETOUR_EQ_Character__CastSpell);

    // (F) Custom SpawnAppearance (type 256) plumbing
    HandleSpawnAppearanceMessage_Trampoline =
        (EQ_FUNCTION_TYPE_HandleSpawnAppearanceMessage)DetourFunction((PBYTE)0x004DF52A, (PBYTE)HandleSpawnAppearanceMessage_Detour);

    // (G) Register your lifecycle and handshake callbacks
	InitGameUICallbacks.push_back([](CDisplay* d) { ShortBuffWindow_InitUI(d); });
    CleanUpUICallbacks.push_back( []()          { ShortBuffWindow_CleanUI(); } );

	//ActivateUICallbacks.push_back([](char c) { ShowBuffWindow_ActivateUI(c); });
	//ActivateUICallbacks.push_back([](char c) {
	//	EnsureBuffstackingHandshakeSentOnce();  // prefer 15+ slots immediately after UI comes up
	//	ShowBuffWindow_ActivateUI(c);
	//	});

	ActivateUICallbacks.push_back([](char c) {
		// Negotiate short-buff support immediately on UI activation
		BuffstackingPatch_OnZone();
		ShowBuffWindow_ActivateUI(c);
		});

	DeactivateUICallbacks.push_back([]() {    ShowBuffWindow_DeactivateUI();   });

    OnZoneCallbacks.push_back(BuffstackingPatch_OnZone);
    CustomSpawnAppearanceMessageHandlers.push_back(BuffstackingPatch_HandleHandshake);

    g_songHooksInstalledOnce = true;
}

//static inline void EnsureBuffstackingHandshakeSentOnce()
//{
//    // If we already negotiated, do nothing.
//    if (Rule_Num_Short_Buffs > 0 || Rule_Buffstacking_Patch_Enabled)
//        return;
//
//    bool is_new_ui = *(BYTE*)0x8092D8 != 0;
//    if (is_new_ui)
//        SendCustomSpawnAppearanceMessage(CustomSpawnAppearanceMessage_BuffStackingPatchWithSongWindowHandshake, BSP_VERSION_1, true);
//    else
//        SendCustomSpawnAppearanceMessage(CustomSpawnAppearanceMessage_BuffStackingPatchWithoutSongWindowHandshake, BSP_VERSION_1, true);
//}


//------------------------------------------------------------------------
// Song Window and Handshake [End]
//------------------------------------------------------------------------

// Essential utility functions
void PatchA(LPVOID address, const void* dwValue, SIZE_T dwBytes) {
    unsigned long oldProtect;
    VirtualProtect((void*)address, dwBytes, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)address, dwValue, dwBytes);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, dwBytes);
    VirtualProtect((void*)address, dwBytes, oldProtect, &oldProtect);
}

// ---------------------------------------------------------------------------------
// BuffStacking Patches
// ---------------------------------------------------------------------------------
// - Makes Bard Selo's line stack with normal movement spells (Accel/Travel/Chorus).
// - Makes normal buffs able to land without having to temporarily pause bard songs. This was a client bug, because the reverse was already allowed (bard songs could land/stack with normal buffs).
// - Fixes broken checking that checked whether the target/caster was a bard, rather than just the spell being a bard song. Now we just care whether it's a bardsong.
//----------------------------------------------------------------------------------

// Handshake "opcodes" sent to OP_SpawnAppearance (these values must be implemented on the server)
constexpr WORD CustomSpawnAppearanceMessage_BuffStackingPatchWithSongWindowHandshake = 2;
constexpr WORD CustomSpawnAppearanceMessage_BuffStackingPatchWithoutSongWindowHandshake = 3;
constexpr WORD BSP_VERSION_1 = 1; // Buff Stacking feature flag sent to the server in the handshake

// Short Buff Window
CShortBuffWindow* ShortBuffWindow = nullptr;
// Set during ShortBuffWindow's refresh logic, so it reads from offset 15 (because it shares logic with CBuffWindow)
thread_local bool ShortBuffSupport_ReturnSongBuffs = false;

// -- [Handshake / Initialization] --

// Ensure we negotiate song-window slots ASAP on first UI activation
static inline void EnsureBuffstackingHandshakeSentOnce()
{
	// If we already negotiated, do nothing.
	if (Rule_Num_Short_Buffs > 0 || Rule_Buffstacking_Patch_Enabled)
		return;

	bool is_new_ui = *(BYTE*)0x8092D8 != 0;
	if (is_new_ui)
		SendCustomSpawnAppearanceMessage(CustomSpawnAppearanceMessage_BuffStackingPatchWithSongWindowHandshake, BSP_VERSION_1, true);
	else
		SendCustomSpawnAppearanceMessage(CustomSpawnAppearanceMessage_BuffStackingPatchWithoutSongWindowHandshake, BSP_VERSION_1, true);
}

void BuffstackingPatch_OnZone()
{
	// Send handshake message to enable the client/server buffstacking changes.
	bool is_new_ui = *(BYTE*)0x8092D8 != 0;
	if (is_new_ui)
		SendCustomSpawnAppearanceMessage(CustomSpawnAppearanceMessage_BuffStackingPatchWithSongWindowHandshake, BSP_VERSION_1, true);
	else
		SendCustomSpawnAppearanceMessage(CustomSpawnAppearanceMessage_BuffStackingPatchWithoutSongWindowHandshake, BSP_VERSION_1, true);
}

// Callback notification on server response to handshake
bool BuffstackingPatch_HandleHandshake(DWORD id, DWORD value, bool is_request)
{

	bool send_response = is_request;
	bool enabled = false;
	int enabled_songs = 0;

	if (id == CustomSpawnAppearanceMessage_BuffStackingPatchWithSongWindowHandshake)
	{
		if (value == BSP_VERSION_1)
		{
			enabled = true;
			enabled_songs = 6;
		}
		else
		{
			value = 0;
		}
	}
	else if (id == CustomSpawnAppearanceMessage_BuffStackingPatchWithoutSongWindowHandshake)
	{
		if (value == BSP_VERSION_1)
		{
			enabled = true;
			enabled_songs = 0;
		}
		else
		{
			value = 0;
		}
	}
	else
	{
		return false;
	}

	// Handshake Complete.
	Rule_Buffstacking_Patch_Enabled = enabled;
	Rule_Max_Buffs = EQ_NUM_BUFFS + enabled_songs;
	Rule_Num_Short_Buffs = enabled_songs;
	if (send_response)
	{
		SendCustomSpawnAppearanceMessage(id, value, false);
	}
	return true;
}

// -- [Helper Functions] --

// This helps us iterate in the right buff order when the song window is involved.
// Song window starts at logical buff offset 15, then loop back around to offset 0 after if no song slots are open.
inline int BSP_ToBuffSlot(int i, int start_offset, int modulo) {
	return (i + start_offset) % modulo;
}
// Fixed bug -- The old logic checked if the spell target was also a bard (obvious bug), rather than just spell's class.
bool BSP_IsStackBlocked(EQCHARINFO* player, _EQSPELLINFO* spell) {
	return (spell && spell->IsBardsong()) 
		? false
		: EQ_Character::IsStackBlocked(player, spell);
}
// Allows selos to stack with regular movement effects.
int BSP_SpellAffectIndex(_EQSPELLINFO* spell, int effectType)
{
	return (effectType == SE_MovementSpeed && spell->IsBeneficial() && spell->IsBardsong())
		? 0
		: EQ_Spell::SpellAffectIndex(spell, effectType);
}

// -- [Main Patch] --
// - This function replaces the client's buffstacking logic.
// - This is faithful to original implementation (and the server), but has our new bug fixes/modifications for stacking, and support for the song window.
// - All changes here need to be mirrored on the server.
_EQBUFFINFO* BSP_FindAffectSlot(EQCHARINFO* player, WORD spellid, _EQSPAWNINFO* caster, DWORD* result_buffslot, int dry_run)
{
	*result_buffslot = -1;
	if (!caster || !EQ_Spell::IsValidSpellIndex(spellid))
		return 0;

	EQSPELLINFO* new_spell = EQ_Spell::GetSpell(spellid);
	if (!new_spell || !caster->Type && BSP_IsStackBlocked(player, new_spell)) // [Patch:Main] See: IsStackBlocked
		return 0;

	int MaxTotalBuffs = Rule_Max_Buffs;
	int StartBuffOffset = 0;
	int MaxSelectableBuffs = EQ_NUM_BUFFS;

	// [Patch:SongWindow] If song window is enabled, songs can search those first
	if (Rule_Num_Short_Buffs > 0 && EQ_Spell::IsShortBuffBox(spellid)) {
		// Song: Start in slots 16+, then wrap around to 1-15 if no slot open.
		StartBuffOffset = EQ_NUM_BUFFS;
		// Song: Allow using all buff slots
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
	bool is_movement_effect = BSP_SpellAffectIndex(new_spell, SE_MovementSpeed) != 0; // [Patch:Main] Optimization - Caching the value.
	short old_effect_value;
	short new_effect_value;

	if (is_bard_song) // [Patch:Main] - Removed: caster->Class == BARD
	{
		for (int i = 0; i < MaxTotalBuffs; i++)
		{
			int buffslot = i; // This first loop is just checking for basic blocking, we can skip the buff/offset translation check
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
		int buffslot = BSP_ToBuffSlot(i, StartBuffOffset, MaxSelectableBuffs); // [Patch:SongWindow] Translates the 'i' value to the right buffslot order.
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
						&& BSP_SpellAffectIndex(new_spell, 67) == 0  // Eye of Zomm
						&& BSP_SpellAffectIndex(new_spell, 101) == 0 // Complete Heal
						&& BSP_SpellAffectIndex(new_spell, 113) == 0)    // Summon Horse
					{
						// overwrite same spell_id without removing first
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
				int buffslot = BSP_ToBuffSlot(i, StartBuffOffset, MaxSelectableBuffs); // [Patch:SongWindow] Translates the 'i' value to the right buffslot order
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
							return buff; // overwrite same spell without removing first
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
				return EQ_Character::GetBuff(player, first_open_buffslot);  // first empty slot, this is a DoT that will stack with itself because it's from another caster
			}
		}
	}

	if (false) // not entered here, jumped into with goto
	{
	STACK_OK_OVERWRITE_BUFF_IF_NEEDED:
		// if we have a result slot already, overwrite the result slot if something is there and it's not this spell_id
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
					int curbuff_slot = BSP_ToBuffSlot(curbuff_i, StartBuffOffset, MaxSelectableBuffs); // [Patch:SongWindow] Translates the 'curbuff_i' value to the right buffslot order
					
					while (1)
					{
						WORD buff_spell_id = EQ_Character::GetBuff(player, curbuff_slot)->SpellId;
						if (EQ_Spell::IsValidSpellIndex(buff_spell_id))
						{
							EQSPELLINFO* buff_spell = EQ_Spell::GetSpell(buff_spell_id);
							if (buff_spell && buff_spell->IsBeneficial()) // found a beneficial spell to overwrite
								break;
						}
						if (++curbuff_i >= MaxSelectableBuffs)
							return 0;
						curbuff_slot = BSP_ToBuffSlot(curbuff_i, StartBuffOffset, MaxSelectableBuffs); // [Patch:SongWindow] Translates the 'curbuff_i' value to the right buffslot order
					}
					if (!dry_run)
					{
						EQBUFFINFO* buff = EQ_Character::GetBuff(player, curbuff_slot); // overwriting a beneficial buff to make room for a detrimental one.
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
			*result_buffslot = cur_slotnum7_buffslot; // save first blank slot found
		}
	STACK_OK: // jump here when current buff and new buff don't interact to increment slot number and check next buff
		if (++cur_slotnum7 >= MaxSelectableBuffs)
		{
			goto STACK_OK_OVERWRITE_BUFF_IF_NEEDED;
		}
		cur_slotnum7_buffslot = BSP_ToBuffSlot(cur_slotnum7, StartBuffOffset, MaxSelectableBuffs); // [Patch:SongWindow] Translates the 'cur_slotnum7' value to the right buffslot order
	}
	if (is_bard_song && !old_spelldata->IsBardsong()) // [Patch:Main] Just checks 'is_bard_song' and not class
	{
		if (new_spell->IsBeneficial() && is_movement_effect && BSP_SpellAffectIndex(old_spelldata, SE_MovementSpeed) != 0
			|| new_spell->IsBeneficial() && is_movement_effect && BSP_SpellAffectIndex(old_spelldata, SE_Root) != 0)
		{
			goto BLOCK_BUFF_178; // [Patch:Main] This line isn't reachable, kept for consistency (formerly: "Bard Selos can't overwrite regular SoW type spell or rooting illusion")
		}

		// generally, bard songs stack with anything that's not a bard song
		if (is_bard_song)
			goto STACK_OK;
	}

	// [Patch:Main] Note - This section always reaches 'false', so SoW/Selos are not getting blocked here
	if (new_spell->IsBeneficial())
	{
		if (old_spelldata->IsBeneficial())
		{
			if (is_movement_effect)
			{
				if (BSP_SpellAffectIndex(old_spelldata, SE_MovementSpeed) != 0)
				{
					if (old_spelldata->IsBardsong() && !is_bard_song)
						goto BLOCK_BUFF_178; // regular SoW type spell can't overwrite bard Selos
				}
			}
		}
	}

	// below is a for loop that's kind of decomposed with gotos, comparing each effect slot
	effect_slot_num = 0;
	while (2)
	{
		BYTE old_buff_effect_id = old_spelldata->Attribute[effect_slot_num];
		if (old_buff_effect_id == SE_Blank) // blank effect slot in old spell, end of spell, don't check rest of slots
			goto STACK_OK;
		
		BYTE new_buff_effect_id = new_spell->Attribute[effect_slot_num];
		if (new_buff_effect_id == SE_Blank) // blank effect slot in new spell, end of spell, don't check rest of slots
			goto STACK_OK;

		if (new_buff_effect_id == SE_Lycanthropy || new_buff_effect_id == SE_Vampirism)
			goto BLOCK_BUFF_178;

		if ((!is_bard_song && old_spelldata->IsBardsong())
			|| old_buff_effect_id != new_buff_effect_id
			|| EQ_Spell::IsSPAIgnoredByStacking(new_buff_effect_id))
		{
			goto NEXT_ATTRIB_107; // ignore if different effect, ignored effect, or if the existing buff is a bard song
		}

		// at this point the effect ids are the same in this slot

		if (new_buff_effect_id == SE_CurrentHP || new_buff_effect_id == SE_ArmorClass)
		{
			if (new_spell->Base[effect_slot_num] >= 0)
				break;
			goto NEXT_ATTRIB_107; // if the new spell has a DoT or negative AC debuff in this effect slot, ignore for stacking
		}
		if (new_buff_effect_id == SE_CHA)
		{
			if (new_spell->Base[effect_slot_num] == 0 || old_spelldata->Base[effect_slot_num] == 0) // SE_CHA can be used as a spacer with 0 base
			{
			NEXT_ATTRIB_107:
				if (++effect_slot_num >= EQ_NUM_SPELL_EFFECTS)
					goto STACK_OK;
				continue;
			}
		}
		break;
	}

	// compare same effect id below

	if (new_spell->IsBeneficial() && (!old_spelldata->IsBeneficial() || BSP_SpellAffectIndex(old_spelldata, SE_Illusion) != 0)
		|| old_spelldata->Attribute[effect_slot_num] == SE_CompleteHeal // Donal's BP effect
		|| old_buff_spell_id >= 775 && old_buff_spell_id <= 785
		|| old_buff_spell_id >= 1200 && old_buff_spell_id <= 1250
		|| old_buff_spell_id >= 1900 && old_buff_spell_id <= 1924
		|| old_buff_spell_id == 2079 // ShapeChange65
		|| old_buff_spell_id == 2751 // Manaburn
		|| old_buff_spell_id == 756 // Resurrection Effects
		|| old_buff_spell_id == 757 // Resurrection Effect
		|| old_buff_spell_id == 836) // Diseased Cloud
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

// Entrypoint for Buff Patch
typedef _EQBUFFINFO* (__thiscall* EQ_FUNCTION_TYPE_EQCharacter__FindAffectSlot)(EQCHARINFO* this_ptr, WORD spellid, _EQSPAWNINFO* caster, DWORD* out_slot, int flag);
EQ_FUNCTION_TYPE_EQCharacter__FindAffectSlot EQCharacter__FindAffectSlot_Trampoline;
_EQBUFFINFO* __fastcall EQCharacter__FindAffectSlot_Detour(EQCHARINFO* player, int unused, WORD spellid, _EQSPAWNINFO* caster, DWORD* out_slot, int flag) {
	if (Rule_Buffstacking_Patch_Enabled) {
		return BSP_FindAffectSlot(player, spellid, caster, out_slot, flag);
	}
	return EQCharacter__FindAffectSlot_Trampoline(player, spellid, caster, out_slot, flag);
}

// ---------------------------------------------------------
// Buff Patch [Song Window]
// ---------------------------------------------------------

_EQBUFFINFO* GetStartBuffArray(bool song_buffs) {
	return song_buffs ? EQ_OBJECT_CharInfo->BuffsExt : EQ_OBJECT_CharInfo->Buff;
}
void MakeGetBuffReturnSongs(bool enabled) {
	ShortBuffSupport_ReturnSongBuffs = enabled;
}

// MaxBuffs is now increased when enabled (Rule_Max_Buffs)
typedef int(__thiscall* EQ_FUNCTION_TYPE_EQCharacter__GetMaxBuffs)(EQCHARINFO* this_ptr);
EQ_FUNCTION_TYPE_EQCharacter__GetMaxBuffs EQCharacter__GetMaxBuffs_Trampoline;
int __fastcall EQCHARACTER__GetMaxBuffs_Detour(EQCHARINFO* player, int unused)
{
	// While the song window is drawing, expose the expanded cap (15 + song slots).
	// While the main buff window draws, keep the classic 15 so songs don’t appear there.
	if (ShortBuffSupport_ReturnSongBuffs) {
		return Rule_Max_Buffs; // expanded cap used only while song window renders
	}

	EQSPAWNINFO* si = player ? player->SpawnInfo : nullptr;
	if (si) {
		if (si->Type == EQ_SPAWN_TYPE_PLAYER) return EQ_NUM_BUFFS; // 15 for main window
		if (si->Type == EQ_SPAWN_TYPE_NPC)    return 30;
	}
	return EQ_NUM_BUFFS;
}

// This didn't solve the problem, testing again
//{
//	// When the song window is drawing, we want the expanded count (15 + song slots).
//	// When the main buff window is drawing, keep it at the classic 15 so songs don't appear there.
//	if (ShortBuffSupport_ReturnSongBuffs) {
//		return Rule_Max_Buffs;          // expanded cap used by the song window path
//	}
//
//	// Original behavior for everyone else:
//	EQSPAWNINFO* spawn_info = player ? player->SpawnInfo : nullptr;
//	if (spawn_info) {
//		if (spawn_info->Type == EQ_SPAWN_TYPE_PLAYER) {
//			return EQ_NUM_BUFFS;        // classic 15 for the main buff window
//		}
//		if (spawn_info->Type == EQ_SPAWN_TYPE_NPC) {
//			return 30;                  // NPC cap unchanged
//		}
//	}
//	return EQ_NUM_BUFFS;
//}

//{
//	EQSPAWNINFO* spawn_info;
//	if (player && (spawn_info = player->SpawnInfo) != 0) {
//		if (spawn_info->Type == EQ_SPAWN_TYPE_PLAYER) {
//			return Rule_Max_Buffs;
//		}
//		if (spawn_info->Type == EQ_SPAWN_TYPE_NPC) {
//			return 30;
//		}
//	}
//	return EQ_NUM_BUFFS;
//}

// Helper to make ShortBuffWindow read from the song buff array.
typedef _EQBUFFINFO* (__thiscall* EQ_FUNCTION_TYPE_EQCharacter__GetBuff)(EQCHARINFO* this_char_info, int buff_slot);
EQ_FUNCTION_TYPE_EQCharacter__GetBuff EQCharacter__GetBuff_Trampoline;
_EQBUFFINFO* __fastcall EQCharacter__GetBuff_Detour(EQCHARINFO* player, int unused, WORD buff_slot) {
	if (ShortBuffSupport_ReturnSongBuffs && buff_slot < 15) {
		buff_slot += 15;
	}
	return EQCharacter__GetBuff_Trampoline(player, buff_slot);
}

// Hook that removes buffs or shows spell info when clicking the song window, and shows tooltips on mouseover
int __fastcall CBuffWindow__WndNotification_Detour(CBuffWindow* self, int unused, PEQCBUFFBUTTONWND sender, int type, int a4)
{
	// Shared hook with CBuffWindow
	// Use the right buff slot offset based on the window.
	bool is_song_window = (self == GetShortDurationBuffWindow());
	int start_buff_index = is_song_window ? 15 : 0;

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
	if (AltPressed())
		goto LABEL_11;
	for (int i = 0; i < EQ_NUM_BUFFS; i++) {
		if (self->Data.BuffButtonWnd[i] == sender) {
			if (EQ_Character::IsValidAffect(EQ_OBJECT_CharInfo, i + start_buff_index))
				EQ_Character::RemoveMyAffect(EQ_OBJECT_CharInfo, i + start_buff_index);
			return CSidlScreenWnd::WndNotification(self, sender, type, a4);
		}
	}
	return CSidlScreenWnd::WndNotification(self, sender, type, a4);
}

CShortBuffWindow* GetShortDurationBuffWindow() {
	return ShortBuffWindow;
}

void ShortBuffWindow_InitUI(CDisplay* cdisplay) {

	if (ShortBuffWindow)
		return;

	CShortBuffWindow* wnd = reinterpret_cast<CShortBuffWindow*>(HeapAlloc(*(HANDLE*)0x80B420, 0, sizeof(_EQCBUFFWINDOW)));
	if (wnd) {
		memset(wnd, 0, sizeof(_EQCBUFFWINDOW));

		// Need to patch the instruction to load our name 'ShortDurationBuffWindow' instead of 'BuffWindow'
		BYTE orig_inst[5];
		BYTE new_inst[5] = { 0x68, 0, 0 , 0, 0 };
		*(uintptr_t*)&new_inst[1] = (uintptr_t)CShortBuffWindow::NAME;

		PatchSwap(0x00408D5A, new_inst, 5, orig_inst);
		CBuffWindow::Consutrctor(wnd);
		PatchSwap(0x00408D5A, orig_inst, 5);

		ShortBuffWindow = wnd;
	}
}
void ShortBuffWindow_CleanUI() {
	if (ShortBuffWindow)
	{
		if (ShortBuffWindow->IsVisibile())
			ShortBuffWindow->Deactivate();
		if (ShortBuffWindow->HasCustomVTable())
			ShortBuffWindow->DeleteCustomVTable();
		ShortBuffWindow->Destroy();
	}
	ShortBuffWindow = nullptr;
}
void ShowBuffWindow_ActivateUI(char c) {
	if (ShortBuffWindow) {
		ShortBuffWindow->LoadIniInfo();
		ShortBuffWindow->Activate();
	}
}
void ShowBuffWindow_DeactivateUI() {
	if (ShortBuffWindow && ShortBuffWindow->IsVisibile()) {
		ShortBuffWindow->StoreIniInfo();
		ShortBuffWindow->Deactivate();
	}
}

void ApplySongWindowBytePatches() {
	// HandleWorldMessage (OP_Buff): Has a hardcoded for-loop of only 15 buffs. Switching to 30.
	// * '0x004E9F8E cmp 15' -> 'cmp 30'
	// [0x83 0xFF 0x0F] -> [0x83 0xFF 0x1E]
	BYTE patch[1] = { 0x1E };
	int address = 0x004E9F8E + 2; 
	PatchSwap(address, patch, 1);
}

// ---------------------------------------------------------------------------------------
// Buff Patch [End]
// ---------------------------------------------------------------------------------------

// ---------- InitHooks() to call the installer ----------

//extern void InitHooks_SongWindow();
//static bool g_songHooksInstalledOnce = false;

extern "C" __declspec(dllexport) void InitHooks()
{
    if (!g_songHooksInstalledOnce) {
        InitHooks_SongWindow();
        g_songHooksInstalledOnce = true;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hModule);
		InitHooks();  // guarded by g_songHooksInstalledOnce inside InitHooks_SongWindow()
	}
	return TRUE;
}
