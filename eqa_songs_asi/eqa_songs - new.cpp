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

// Sent on zone entry to the server.
// Server uses this to tell the user if they are out of date.
// Increment if we make significant changes that we want to track.
// Server uses Quarm:WarnDllVersionBelow to warn clients below a specific threshold.
#ifndef DLL_VERSION
#define DLL_VERSION 1
#endif
#define DLL_VERSION_MESSAGE_ID 4 // Matches ClientFeature::CodeVersion == 4 on the Server, do not change.

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

struct _EQBUFFINFO* GetStartBuffArray(bool song_buffs);
void MakeGetBuffReturnSongs(bool enabled);

//------------------------------------------------------------------------
// End of additions from eqgame.h
//------------------------------------------------------------------------


//------------------------------------------------------------------------
// Song Window and Handshake [Begin]
//------------------------------------------------------------------------

// ---------- Globals referenced by handshake / rules ----------
bool Rule_Buffstacking_Patch_Enabled = false;
int  Rule_Num_Short_Buffs = 0;            // set by handshake
int  Rule_Max_Buffs = EQ_NUM_BUFFS; // raised by handshake
int g_buffWindowTimersFontSize = 3; // default tooltip/overlay font size
bool g_bSongWindowAutoHide = false;

// ---------- Tiny helpers used by WndNotification ----------
static inline bool CtrlPressed() { return *(DWORD*)0x00809320 > 0; }
static inline bool AltPressed() { return *(DWORD*)0x0080932C > 0; }
static inline bool ShiftPressed() { return *(DWORD*)0x0080931C > 0; }

// ---------- Detour Originals ----------
EQ_FUNCTION_TYPE_CBuffWindow__RefreshBuffDisplay  EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay = NULL;
EQ_FUNCTION_TYPE_CBuffWindow__PostDraw            EQMACMQ_REAL_CBuffWindow__PostDraw = NULL;
EQ_FUNCTION_TYPE_EQ_Character__CastSpell EQMACMQ_REAL_EQ_Character__CastSpell = NULL;
EQ_FUNCTION_TYPE_CEverQuest__InterpretCmd EQMACMQ_REAL_CEverQuest__InterpretCmd = NULL;

// ---------- Callback registries ----------
// Callbacks run on zone
std::vector<std::function<void()>> OnZoneCallbacks;
std::vector<std::function<void(CDisplay*)>> InitGameUICallbacks;
std::vector<std::function<void()>> DeactivateUICallbacks;
std::vector<std::function<void(char)>> ActivateUICallbacks;
std::vector<std::function<void()>> CleanUpUICallbacks;

// Callbacks run on custom messages received via OP_SpawnAppearance
std::vector<std::function<bool(DWORD feature_id, DWORD feature_value, bool is_request)>> CustomSpawnAppearanceMessageHandlers;

// ---------- Patching helpers ----------
// copies target original value to buffer, then copies source to the target
void PatchSwap(int target, BYTE* source, SIZE_T size, BYTE* buffer = nullptr)
{
	DWORD oldprotect;
	VirtualProtect((PVOID*)target, size, PAGE_EXECUTE_READWRITE, &oldprotect);
	if (buffer)
		memcpy((void*)buffer, (const void*)target, size);
	memcpy((void*)target, (const void*)source, size);
	FlushInstructionCache(GetCurrentProcess(), (void*)target, size);
	VirtualProtect((PVOID*)target, size, oldprotect, &oldprotect);
}

// ---------- Print to Chat Window Wrapper ----------
typedef void(__thiscall* PrintChat)(int this_ptr, const char* data, short color, bool un);
void print_chat(const char* format, ...)
{
	static PrintChat print_chat_internal = (PrintChat)0x537f99;
	va_list argptr;
	char buffer[512];
	va_start(argptr, format);
	vsnprintf(buffer, 511, format, argptr);
	va_end(argptr);
	print_chat_internal(*(int*)0x809478, buffer, 0, true);
}

// ---------- Callback helpers ----------

// Helper - Executes all callbacks in 'OnZoneCallbacks'
typedef void(__thiscall* EQ_FUNCTION_TYPE_EnterZone)(void* this_ptr, int hwnd);
EQ_FUNCTION_TYPE_EnterZone EnterZone_Trampoline;
void __fastcall EnterZone_Detour(void* this_ptr, int unused, int hwnd) {
	EnterZone_Trampoline(this_ptr, hwnd);
	for (auto& callback : OnZoneCallbacks) {
		callback();
	}
}

// Helper - Executes all callbacks in 'InitGameUICallbacks'
typedef int(__thiscall* EQ_FUNCTION_TYPE_InitGameUI)(CDisplay* cdisplay);
EQ_FUNCTION_TYPE_InitGameUI InitGameUI_Trampoline;
int __fastcall InitGameUI_Detour(CDisplay* cdisplay, int unused)
{
	int res = InitGameUI_Trampoline(cdisplay);
	for (auto& callback : InitGameUICallbacks) {
		callback(cdisplay);
	}
	return res;
}

// Helper - Executes all callbacks in 'CleanUpUICallbacks'
typedef void* (*EQ_FUNCTION_TYPE_CleanUpUI)(void);
EQ_FUNCTION_TYPE_CleanUpUI CleanUpUI_Trampoline;
void* CleanUpUI_Detour()
{
	void* res = CleanUpUI_Trampoline();
	for (auto& callback : CleanUpUICallbacks) {
		callback();
	}
	return res;
}

// Helper - Executes all callbacks in 'ActivateUICallbacks'
typedef int(__stdcall* EQ_FUNCTION_TYPE_ActivateUI)(char a1);
EQ_FUNCTION_TYPE_ActivateUI ActivateUI_Trampoline;
int __stdcall ActivateUI_Detour(char a1)
{
	int res = ActivateUI_Trampoline(a1);
	for (auto& callback : ActivateUICallbacks) {
		callback(a1);
	}
	return res;
}

// Helper - Executes all callbacks in 'DeactivateUICallbacks'
typedef int(*EQ_FUNCTION_TYPE_DeactivateUI)(void);
EQ_FUNCTION_TYPE_DeactivateUI DeactivateUI_Trampoline;
int DeactivateUI_Detour() {
	int res = DeactivateUI_Trampoline();
	for (auto& callback : DeactivateUICallbacks) {
		callback();
	}
	return res;
}

// Helper - Sends custom key/value data to the server using OP_SpawnAppearance (type = 256)
void SendCustomSpawnAppearanceMessage(unsigned __int16 feature_id, unsigned __int16 feature_value, bool is_request) {

	DWORD id = feature_id;
	DWORD value = feature_value;

	SpawnAppearance_Struct message;
	message.type = SpawnAppearanceType_ClientDllMessage; // AppearanceType::ClientDllMessage on server
	message.spawn_id = 0;
	message.parameter = (id << 16) | value;
	if (is_request)
		message.parameter &= 0x7FFFFFFFu;
	else
		message.parameter |= 0x80000000u;
	reinterpret_cast<void(__cdecl*)(int* connection, DWORD opcode, void* buffer, DWORD size, int unknown)>(0x54e51a)((int*)0x7952fc, 16629, &message, sizeof(SpawnAppearance_Struct), 0); // Connection::SendMessage(..)
}

// Helper - Executes all callback handlers for custom SpawnAppearanceMessages
void HandleCustomSpawnAppearanceMessage(SpawnAppearance_Struct* message)
{
	// TODO: Maybe in the future we could encode data into spawn_id field too, but let's keep it simple for now.
	if (message->type == SpawnAppearanceType_ClientDllMessage && message->spawn_id == 0) {
		bool is_request = (message->parameter >> 31) == 0;
		DWORD feature_id = message->parameter >> 16 & 0x7FFFu;
		DWORD feature_value = message->parameter & 0xFFFFu;
		for (auto& handler : CustomSpawnAppearanceMessageHandlers) {
			if (handler(feature_id, feature_value, is_request)) {
				return;
			}
		}
	}
}
// Hook to delegate to HandleCustomSpawnAppearanceMessage
typedef int(__thiscall* EQ_FUNCTION_TYPE_HandleSpawnAppearanceMessage)(void* this_ptr, int unk2, int opcode, SpawnAppearance_Struct* sa);
EQ_FUNCTION_TYPE_HandleSpawnAppearanceMessage HandleSpawnAppearanceMessage_Trampoline;
int __fastcall HandleSpawnAppearanceMessage_Detour(void* this_ptr, int unused_edx, int unk2, int opcode, SpawnAppearance_Struct* sa) {
	if (sa->type >= SpawnAppearanceType_ClientDllMessage) {
		HandleCustomSpawnAppearanceMessage(sa);
		return 1;
	}
	return HandleSpawnAppearanceMessage_Trampoline(this_ptr, unk2, opcode, sa);
}

typedef bool(__cdecl* EQ_FUNCTION_TYPE_GetLabelFromEQ)(int, PEQCXSTR*, bool*, DWORD*);
EQ_FUNCTION_TYPE_GetLabelFromEQ GetLabelFromEQ_Trampoline;
bool __cdecl GetLabelFromEQ_Detour(int EqType, PEQCXSTR* str, bool* override_color, DWORD* color)
{
	switch (EqType) {
	case 135: // Song1
	case 136: // Song2
	case 137: // Song3
	case 138: // Song4
	case 139: // Song5
	case 140: // Song6
	case 141: // Song7
	case 142: // Song8
	case 143: // Song9
	case 144: // Song10
	case 145: // Song11
	case 146: // Song12
	case 147: // Song13
	case 148: // Song14
	case 149: // Song15
		*override_color = false;
		if (EQ_OBJECT_CharInfo) {
			EQBUFFINFO& buff = EQ_OBJECT_CharInfo->BuffsExt[EqType - 135];
			if (EQ_Spell::IsValidSpellIndex(buff.SpellId)) {
				EQSPELLINFO* spell = EQ_Spell::GetSpell(buff.SpellId);
				if (spell) {
					EQ_CXStr_Set(str, spell->Name);
					return true;
				}
			}
		}
		EQ_CXStr_Set(str, "");
		return true;
	}
	return GetLabelFromEQ_Trampoline(EqType, str, override_color, color);
}


void __fastcall EQMACMQ_DETOUR_CBuffWindow__RefreshBuffDisplay(CBuffWindow* this_ptr, void* not_used)
{
	PEQCBUFFWINDOW buffWindow = (PEQCBUFFWINDOW)this_ptr;
	PEQCHARINFO charInfo = (PEQCHARINFO)EQ_OBJECT_CharInfo;

	if (charInfo == NULL)
	{
		return;
	}

	// Supports ShortBuffWindow(Songs) and BuffWindow, which use different buff offsets
	bool is_song_window = (this_ptr == GetShortDurationBuffWindow());
	_EQBUFFINFO* buffs = GetStartBuffArray(is_song_window);

	MakeGetBuffReturnSongs(is_song_window);
	EQMACMQ_REAL_CBuffWindow__RefreshBuffDisplay(this_ptr);
	MakeGetBuffReturnSongs(false);

	int num_buffs = 0;

	// -- Standard Dll Support Buff Text / Timer --
	for (size_t i = 0; i < EQ_NUM_BUFFS; i++)
	{
		EQBUFFINFO& buff = buffs[i];
		if (!EQ_Spell::IsValidSpellIndex(buff.SpellId) || buff.BuffType == 0)
		{
			continue;
		}
		num_buffs++;

		int buffTicks = buff.Ticks;

		if (buffTicks == 0)
		{
			continue;
		}

		PEQCBUFFBUTTONWND buffButtonWnd = buffWindow->BuffButtonWnd[i];

		if (buffButtonWnd && buffButtonWnd->CSidlWnd.EQWnd.ToolTipText)
		{
			char buffTickTimeText[128];
			EQ_GetTickTimeString(buffTicks, buffTickTimeText, sizeof(buffTickTimeText));

			char buffTimeText[128];
			_snprintf_s(buffTimeText, sizeof(buffTimeText), _TRUNCATE, " (%s)", buffTickTimeText);

			EQ_CXStr_Append(&buffButtonWnd->CSidlWnd.EQWnd.ToolTipText, buffTimeText);
		}
	}

	if (is_song_window)
	{
		if (this_ptr->IsVisibile())
		{
			if (num_buffs == 0 && (g_bSongWindowAutoHide || Rule_Num_Short_Buffs == 0)) // Visible, but support is disabled or auto-hide
				this_ptr->Show(0, 1);
			return;
		}
		if (num_buffs > 0)
		{
			// Not visible and we have buffs. Show.
			this_ptr->Show(1, 1);
		}
	}
}

int __fastcall EQMACMQ_DETOUR_CBuffWindow__PostDraw(CBuffWindow* this_ptr, void* not_used)
{

	int result = EQMACMQ_REAL_CBuffWindow__PostDraw(this_ptr);
	PEQCBUFFWINDOW buffWindow = (PEQCBUFFWINDOW)this_ptr;
	PEQCHARINFO charInfo = (PEQCHARINFO)EQ_OBJECT_CharInfo;
	if (charInfo == NULL)
	{
		return result;
	}

	bool is_song_window = (this_ptr == GetShortDurationBuffWindow());
	_EQBUFFINFO* buffs = GetStartBuffArray(is_song_window); // Song Window Support

	for (size_t i = 0; i < EQ_NUM_BUFFS; i++)
	{
		EQBUFFINFO& buff = buffs[i];

		if (!EQ_Spell::IsValidSpellIndex(buff.SpellId) || buff.BuffType == 0)
		{
			continue;
		}

		int buffTicks = buff.Ticks;
		if (buffTicks == 0)
		{
			continue;
		}
		char buffTimeText[128];
		EQ_GetShortTickTimeString(buffTicks, buffTimeText, sizeof(buffTimeText));

		PEQCBUFFBUTTONWND buffButtonWnd = buffWindow->BuffButtonWnd[i];

		if (buffButtonWnd && buffButtonWnd->CSidlWnd.EQWnd.ToolTipText)
		{
			buffButtonWnd->CSidlWnd.EQWnd.FontPointer->Size = g_buffWindowTimersFontSize;

			char originalToolTipText[128];
			strncpy_s(originalToolTipText, sizeof(originalToolTipText), buffButtonWnd->CSidlWnd.EQWnd.ToolTipText->Text, _TRUNCATE);

			EQ_CXStr_Set(&buffButtonWnd->CSidlWnd.EQWnd.ToolTipText, buffTimeText);

			CXRect relativeRect = ((CXWnd*)buffButtonWnd)->GetScreenRect();

			((CXWnd*)buffButtonWnd)->DrawTooltipAtPoint(relativeRect.X1, relativeRect.Y1);

			EQ_CXStr_Set(&buffButtonWnd->CSidlWnd.EQWnd.ToolTipText, originalToolTipText);

			buffButtonWnd->CSidlWnd.EQWnd.FontPointer->Size = EQ_FONT_SIZE_DEFAULT;
		}
	}

	return result;
}

int __fastcall EQMACMQ_DETOUR_CEverQuest__InterpretCmd(void* this_ptr, void* not_used, class EQPlayer* a1, char* a2)
{
	if (a1 == NULL || a2 == NULL)
	{
		return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, a1, a2);
	}

	if (strlen(a2) == 0)
	{
		return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, NULL, NULL);
	}

	// double slashes not needed, convert "//" to "/" by removing first character
	if (strncmp(a2, "//", 2) == 0)
	{
		memmove(a2, a2 + 1, strlen(a2));
	}
	//if (strcmp(a2, "/fps") == 0) {
	//	// enable fps indicator
	//	if (eqgfxMod) {
	//		if (*(BYTE*)(eqgfxMod + 0x00A4F770) == 0)
	//			*(BYTE*)(eqgfxMod + 0x00A4F770) = 1;
	//		else
	//			*(BYTE*)(eqgfxMod + 0x00A4F770) = 0;
	//	}
	//	return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, NULL, NULL);
	//}

	//if (strcmp(a2, "/rfps") == 0) {
	//	LoadIniSettings();

	//	return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, NULL, NULL);
	//}

	//if (strcmp(a2, "/rnpcdata") == 0) {
	//	InitRaceShortCodeMap();
	//	return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, NULL, NULL);
	//}

	else if (strcmp(a2, "/songs") == 0) {
		g_bSongWindowAutoHide = !g_bSongWindowAutoHide;
		WritePrivateProfileStringA("Defaults", "SongWindowAutoHide", g_bSongWindowAutoHide ? "TRUE" : "FALSE", "./eqclient.ini");
		print_chat("Song Window auto-hide: %s.", g_bSongWindowAutoHide ? "ON" : "OFF");
		if (!g_bSongWindowAutoHide && GetShortDurationBuffWindow() && !GetShortDurationBuffWindow()->IsVisibile()) {
			GetShortDurationBuffWindow()->Show(1, 1);
		}
		return 0;
	}

	//else if ((strcmp(a2, "/raiddump") == 0) || (strcmp(a2, "/outputfile raid") == 0)) {
	//	// beginning of raid structure
	//	DWORD raid_ptr = 0x007914D0;
	//	DWORD name_ptr = raid_ptr + 72;
	//	DWORD level_ptr = raid_ptr + 136;
	//	DWORD class_ptr = raid_ptr + 144;
	//	DWORD is_leader_ptr = raid_ptr + 275;
	//	DWORD group_num_ptr = raid_ptr + 276;

	//	CHAR RaidLeader[64];
	//	CHAR CharName[64];
	//	CHAR Class[64];
	//	CHAR Level[8];
	//	int i = 0;
	//	if (*(BYTE*)(raid_ptr) == 1) {
	//		memcpy(RaidLeader, (char*)(0x794FA0), 64);
	//		char v50[64];
	//		char v51[256];
	//		time_t a2;
	//		a2 = time(0);
	//		struct tm* v4;
	//		v4 = localtime(&a2);
	//		sprintf(
	//			v50,
	//			"%04d%02d%02d-%02d%02d%02d",
	//			v4->tm_year + 1900,
	//			v4->tm_mon + 1,
	//			v4->tm_mday,
	//			v4->tm_hour,
	//			v4->tm_min,
	//			v4->tm_sec);
	//		sprintf(v51, "RaidRoster-%s.txt", v50);
	//		FILE* result;
	//		result = fopen(v51, "w");
	//		if (result != NULL) {

	//			while (*(BYTE*)(raid_ptr) == 1) {
	//				memcpy(CharName, (char*)(name_ptr), 64);
	//				memcpy(Level, (char*)(level_ptr), 8);
	//				memcpy(Class, (char*)(class_ptr), 64);
	//				bool group_leader = (bool)*(CHAR*)(is_leader_ptr);
	//				int group_num = (int)*(CHAR*)(group_num_ptr);
	//				group_num++;
	//				std::string type = "";
	//				if (group_leader)
	//					type = "Group Leader";
	//				if (strcmp(CharName, RaidLeader) == 0)
	//					type = "Raid Leader";
	//				raid_ptr++;
	//				name_ptr += 208;
	//				level_ptr += 208;
	//				class_ptr += 208;
	//				is_leader_ptr += 208;
	//				group_num_ptr += 208;

	//				fprintf(result, "%d\t%s\t%s\t%s\t%s\t%s\n", group_num, CharName, Level, Class, type.c_str(), "");
	//			}
	//			fclose(result);
	//		}
	//	}
	//	return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, NULL, NULL);
	//}

	return EQMACMQ_REAL_CEverQuest__InterpretCmd(this_ptr, a1, a2);
}

// ---- CEverQuest::InterpretCmd detour (ONLY adds /songs) ----
struct EQPlayer; // forward decl; matches your project’s type

typedef int(__thiscall* EQ_FUNCTION_TYPE_CEverQuest__InterpretCmd)(void* this_ptr, EQPlayer* a1, char* a2);
static EQ_FUNCTION_TYPE_CEverQuest__InterpretCmd EQ_InterpretCmd_Trampoline = nullptr;

int __fastcall EQ_InterpretCmd_Detour(void* this_ptr, void* /*unused_edx*/, EQPlayer* a1, char* a2)
{
	// Chain if nothing to do
	if (!a2 || !*a2) {
		return EQ_InterpretCmd_Trampoline(this_ptr, a1, a2);
	}

	// Convert '//' to '/' for parity with stock behavior
	if (strncmp(a2, "//", 2) == 0) {
		memmove(a2, a2 + 1, strlen(a2));
	}

	// Our only addition: /songs
	if (strcmp(a2, "/songs") == 0) {
		g_bSongWindowAutoHide = !g_bSongWindowAutoHide;

		//WritePrivateProfileStringA_tramp("Defaults", "SongWindowAutoHide", g_bSongWindowAutoHide ? "TRUE" : "FALSE", "./eqclient.ini");
		WritePrivateProfileStringA("Defaults", "SongWindowAutoHide",
			g_bSongWindowAutoHide ? "TRUE" : "FALSE", "./eqclient.ini");
		print_chat("Song Window auto-hide: %s.", g_bSongWindowAutoHide ? "ON" : "OFF");
		if (!g_bSongWindowAutoHide && GetShortDurationBuffWindow() && !GetShortDurationBuffWindow()->IsVisibile()) {
			GetShortDurationBuffWindow()->Show(1, 1);
		}
		return 0;
	}

	// Everything else goes to original
	return EQ_InterpretCmd_Trampoline(this_ptr, a1, a2);
}

void SendDllVersion_OnZone()
{
	SendCustomSpawnAppearanceMessage(DLL_VERSION_MESSAGE_ID, DLL_VERSION, true);
}

// Re-sends the DllVersion if the server requested it from us
bool HandleDllVersionRequest(DWORD id, DWORD value, bool is_request)
{
	if (id == DLL_VERSION_MESSAGE_ID)
	{
		if (is_request)
		{
			SendCustomSpawnAppearanceMessage(DLL_VERSION_MESSAGE_ID, DLL_VERSION, false);
		}
		return true;
	}
	return false;
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
	EQSPAWNINFO* spawn_info;
	if (player && (spawn_info = player->SpawnInfo) != 0) {
		if (spawn_info->Type == EQ_SPAWN_TYPE_PLAYER) {
			return Rule_Max_Buffs;
		}
		if (spawn_info->Type == EQ_SPAWN_TYPE_NPC) {
			return 30;
		}
	}
	return EQ_NUM_BUFFS;
}

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

void CheckClientMiniMods()
{
	char szResult[255];
	char szDefault[255];
	sprintf(szDefault, "%s", "NONE");
	DWORD error = GetPrivateProfileStringA("Defaults", "SongWindowAutoHide", szDefault, szResult, 255, "./eqclient.ini");
	if (strcmp(szResult, "TRUE") == 0) // True
	{
		g_bSongWindowAutoHide = true;
	}
	else if (strcmp(szResult, "NONE") == 0) // Not found
	{
		g_bSongWindowAutoHide = false;
		WritePrivateProfileStringA("Defaults", "SongWindowAutoHide",
			g_bSongWindowAutoHide ? "TRUE" : "FALSE",
			"./eqclient.ini");
	}
	else // Default off
	{
		g_bSongWindowAutoHide = false;
	}
}

//void CheckClientMiniMods()
//{
//	char szResult[255];
//	char szDefault[255];
//	error = GetPrivateProfileStringA("Defaults", "SongWindowAutoHide", szDefault, szResult, 255, "./eqclient.ini");
//	if (strcmp(szResult, "TRUE") == 0) // True
//	{
//		g_bSongWindowAutoHide = true;
//	}
//	else if (strcmp(szResult, "NONE") == 0) // Not found
//	{
//		g_bSongWindowAutoHide = false;
//		WritePrivateProfileStringA_tramp("Defaults", "SongWindowAutoHide", "FALSE", "./eqclient.ini");
//	}
//	else // Default off
//	{
//		g_bSongWindowAutoHide = false;
//	}
//}

void InitHooks()
{
	// Supports additional labels (Song Window, for now). Zeal handles most others.
	GetLabelFromEQ_Trampoline = (EQ_FUNCTION_TYPE_GetLabelFromEQ)DetourFunction((PBYTE)0x436680, (PBYTE)GetLabelFromEQ_Detour);

	// Helper hooks that run callbacks
	EnterZone_Trampoline = (EQ_FUNCTION_TYPE_EnterZone)DetourFunction((PBYTE)0x53D2C4, (PBYTE)EnterZone_Detour); // OnZone callbacks
	HandleSpawnAppearanceMessage_Trampoline = (EQ_FUNCTION_TYPE_HandleSpawnAppearanceMessage)DetourFunction((PBYTE)0x004DF52A, (PBYTE)HandleSpawnAppearanceMessage_Detour); // OnSpawnAppearance(256) callbacks
	InitGameUI_Trampoline = (EQ_FUNCTION_TYPE_InitGameUI)DetourFunction((PBYTE)0x004a60b5, (PBYTE)InitGameUI_Detour);
	CleanUpUI_Trampoline = (EQ_FUNCTION_TYPE_CleanUpUI)DetourFunction((PBYTE)0x004A6EBC, (PBYTE)CleanUpUI_Detour);
	ActivateUI_Trampoline = (EQ_FUNCTION_TYPE_ActivateUI)DetourFunction((PBYTE)0x004A741B, (PBYTE)ActivateUI_Detour);
	DeactivateUI_Trampoline = (EQ_FUNCTION_TYPE_DeactivateUI)DetourFunction((PBYTE)0x4A7705, (PBYTE)DeactivateUI_Detour);

	// Sends DLL_VERSION to the server on zone-in
	OnZoneCallbacks.push_back(SendDllVersion_OnZone);
	CustomSpawnAppearanceMessageHandlers.push_back(HandleDllVersionRequest);

	// [BuffStackingPatch:Main]
	EQCharacter__FindAffectSlot_Trampoline = (EQ_FUNCTION_TYPE_EQCharacter__FindAffectSlot)DetourFunction((PBYTE)0x004C7A3E, (PBYTE)EQCharacter__FindAffectSlot_Detour);
	OnZoneCallbacks.push_back(BuffstackingPatch_OnZone);
	CustomSpawnAppearanceMessageHandlers.push_back(BuffstackingPatch_HandleHandshake);

	// [BuffStackingPacth:SongWindow]
	EQCharacter__GetBuff_Trampoline = (EQ_FUNCTION_TYPE_EQCharacter__GetBuff)DetourFunction((PBYTE)0x004C465A, (PBYTE)EQCharacter__GetBuff_Detour); // Supports reading buffs 16-30 in Song Window
	EQCharacter__GetMaxBuffs_Trampoline = (EQ_FUNCTION_TYPE_EQCharacter__GetMaxBuffs)DetourFunction((PBYTE)0x004C4637, (PBYTE)EQCHARACTER__GetMaxBuffs_Detour); // Uses 16+ buffs for buff loops (stat calcs etc)
	DetourFunction((PBYTE)0x00408FF1, (PBYTE)CBuffWindow__WndNotification_Detour); // Handles clicking off buffs 16+ on song window
	ApplySongWindowBytePatches(); // Fixes OP_Buff to work on all 30 slots
	InitGameUICallbacks.push_back(ShortBuffWindow_InitUI); // Loads Song window
	ActivateUICallbacks.push_back(ShowBuffWindow_ActivateUI);
	CleanUpUICallbacks.push_back(ShortBuffWindow_CleanUI);
	DeactivateUICallbacks.push_back(ShowBuffWindow_DeactivateUI);
}