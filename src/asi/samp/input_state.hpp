#pragma once
//
// SA-MP's own switch on the game's input.
//
// SA-MP does not ask the game to ignore the keyboard and mouse; it removes
// them. CGame::SetCursorMode (samp.dll+0x9BD30) overwrites, in gta_sa.exe,
// the call to AffectPadFromKeyBoard inside CPad::UpdatePads (0x541DF5) with
// NOPs, the call that reads the DirectInput mouse inside CPad::UpdateMouse
// (0x53F417) with NOPs and its success test with a forced branch, and puts a
// `ret` over 0x6194A0 - and shows a cursor. That is a dialog, the chat line,
// a text-draw selection. Mode 0 puts the bytes back, but only through
// CGame::ProcessInputEnabling (+0x9BC10) once a small delay has counted
// down; when that never happens the game is left exactly as the lock
// looks: keys in the game's own key table, nothing in the pad, DirectInput
// never asked, an arrow on the screen, and nothing but alt-tab to shake it.
//
// So the state is read - the mode and delay in CGame (samp.dll+0x21A10C,
// fields +0x55 and +0x59) and the bytes themselves - written on the input
// line, logged when it changes, and when SA-MP has kept the input off while
// the player has held a movement key for a good while in the active window,
// switched back on the way SA-MP itself does it: SetCursorMode(0, true)
// followed by ProcessInputEnabling.
//
#include <string>

namespace gtabot::samp {

struct InputSwitch {
  bool valid = false;        // samp.dll and its CGame found
  int  mode  = 0;            // CGame::m_nCursorMode
  int  delay = 0;            // frames left before input is put back
  bool keyboard_off = false; // the AffectPadFromKeyBoard call is NOPed
  bool mouse_off    = false; // the DirectInput read in UpdateMouse is NOPed
  bool handler_off  = false; // 0x6194A0 is a ret
  // samp.dll+0x21A130: above ten, SA-MP's window procedure passes nothing on.
  int  gate = 0;
  // CLocalPlayer: active (spawned), wasted, cleared to spawn, and the flag
  // behind "Returning to class selection after next death".
  bool player_known = false;
  int  active = 0, wasted = 0, cleared = 0, return_to_class = 0;
};

InputSwitch ReadInputSwitch();

// CNetGame::m_nGameState in words: "connected", "connecting", "waiting to
// join", "restarting", "not connected". Anything driving this from outside
// needs to tell a kick from a slow load.
std::string ConnectionState();

// Worker thread, every quarter second: logs changes, and restores the input
// when SA-MP has left it off against the player's evident wish.
void WatchSampInput();

// Game thread: SetCursorMode(0, true) then ProcessInputEnabling on SA-MP's
// CGame. Logs what it found before and after.
void ForceInputOn(const char* why);

// For the input line.
std::string InputSwitchLine();

// Whether the game's input is off for a reason that is not ours to
// override: SA-MP has a dialog, chat line or text-draw up (cursor mode), the
// server has frozen the player (CPad::DisablePlayerControls), the local
// player is not spawned, or the game's own menu is open. While this holds
// the walker and the keyboard fallbacks write nothing into the pad - a
// character that moves while frozen or in a dialog is what a server's
// anti-cheat is built to notice. Game thread; cheap enough for every frame.
bool InputLegitimatelyOff(const char** why);

}  // namespace gtabot::samp
