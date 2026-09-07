#pragma once
//
// Shared vocabulary for the module. Everything lives in one process now, so
// this is a set of names rather than a wire format.
//
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace gtabot {

using json = nlohmann::json;

// Milliseconds since the Unix epoch.
std::int64_t NowMillis();

// Makes bytes read out of the client safe to put in JSON.
//
// SA-MP 0.3.7 is an ANSI client: chat, dialogs and names are single bytes in
// whatever code page the server speaks, which on a Russian server is CP1251.
// nlohmann refuses to serialise anything that is not valid UTF-8, so a single
// Cyrillic letter read straight out of memory would throw and take the whole
// response with it. Bytes that already form valid UTF-8 are left alone -
// which covers all ASCII - and anything else is decoded as CP1251.
std::string ToUtf8(const std::string& bytes);

// Directory this module was loaded from, with a trailing separator. Everything
// the mod writes goes there: it is the one path that is certain to exist and
// certain to be the one the user is looking at.
std::string ModuleDirectory();

// What an agent can ask the game to do. Kinds the module does not implement
// yet answer with an explicit failure rather than a success.
namespace action {
inline constexpr const char* kChatSend      = "chat_send";
inline constexpr const char* kDialogRespond = "dialog_respond";
inline constexpr const char* kKeySet        = "key_set";
inline constexpr const char* kMoveTo        = "move_to";
inline constexpr const char* kStop          = "stop";
inline constexpr const char* kVehicleEnter  = "vehicle_enter";
inline constexpr const char* kVehicleExit   = "vehicle_exit";
}  // namespace action

}  // namespace gtabot
