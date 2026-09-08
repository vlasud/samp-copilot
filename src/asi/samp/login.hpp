#pragma once
//
// The server's password dialog, answered from a file the player keeps.
//
// A server that asks for a password asks a person, and a person is exactly
// what is missing when this module is driven by something that is not one.
// So the password lives in bot.login next to the module, the player puts it
// there, and this types it into the dialog the way a hand would: one
// character a frame through the system's own input, then Enter.
//
// It is deliberately narrow. Only a dialog the server marked as a password
// input is answered, only before the character has spawned for the first
// time, only once in a session, and only while the game's window is the one
// in front. A bank PIN asked later in the evening is not the account
// password and must not be given one.
//
// The password never reaches the log, and never leaves the machine. bot.login
// is in .gitignore. Plain text is accepted; so is the output of PowerShell's
// ConvertFrom-SecureString, which ties the file to the Windows account that
// wrote it and is worth the one command it costs.
//
#include <string>

namespace gtabot::samp {

// Game thread, once a frame: types the password when the moment is the one
// described above. Does nothing at all when there is no bot.login.
void WatchLogin();

// Types it now, into whatever password dialog is on screen, whether or not
// the character has spawned before. Returns what happened, for the caller
// to read back.
std::string LoginNow();

// For the status line and the readiness summary.
bool LoginConfigured();
bool LoginSent();
std::string LoginLine();

}  // namespace gtabot::samp
