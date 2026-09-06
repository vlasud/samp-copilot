#pragma once
//
// Shared vocabulary for the module. Everything lives in one process now, so
// this is a set of names rather than a wire format.
//
#include <cstdint>

#include <nlohmann/json.hpp>

namespace gtabot {

using json = nlohmann::json;

// Milliseconds since the Unix epoch.
std::int64_t NowMillis();

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
