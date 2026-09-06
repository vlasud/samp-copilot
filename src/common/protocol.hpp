#pragma once
//
// Wire protocol between bot.asi (inside gta_sa.exe) and gta-mcp.exe.
//
// Transport is a byte-stream named pipe carrying newline-delimited JSON. One
// JSON object per line, no embedded newlines. Both sides must tolerate unknown
// fields and unknown message types so the two halves can be updated apart.
//
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace gtabot::proto {

using json = nlohmann::json;

// Bumped on any breaking change. The server rejects a client that does not
// match, rather than guessing at a half-understood payload.
inline constexpr int kVersion = 1;

inline constexpr const char* kPipeName = R"(\\.\pipe\gtabot)";

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------

namespace msg {
// asi -> server
inline constexpr const char* kHello    = "hello";     // handshake, sent first
inline constexpr const char* kSnapshot = "snapshot";  // periodic world state
inline constexpr const char* kEvent    = "event";     // chat line, dialog, ...
inline constexpr const char* kResult   = "result";    // outcome of an action
inline constexpr const char* kLog      = "log";       // diagnostics

// server -> asi
inline constexpr const char* kHelloAck = "hello_ack";
inline constexpr const char* kAction   = "action";    // do something in-game
}  // namespace msg

// `event` subtypes, carried in payload["kind"].
namespace event {
inline constexpr const char* kChat        = "chat";
inline constexpr const char* kDialogShow  = "dialog_show";
inline constexpr const char* kDialogClose = "dialog_close";
inline constexpr const char* kConnected   = "connected";
inline constexpr const char* kDisconnected= "disconnected";
inline constexpr const char* kDeath       = "death";
}  // namespace event

// `action` kinds, carried in payload["kind"].
namespace action {
inline constexpr const char* kChatSend      = "chat_send";
inline constexpr const char* kDialogRespond = "dialog_respond";
inline constexpr const char* kKeySet        = "key_set";
inline constexpr const char* kMoveTo        = "move_to";
inline constexpr const char* kStop          = "stop";
inline constexpr const char* kVehicleEnter  = "vehicle_enter";
inline constexpr const char* kVehicleExit   = "vehicle_exit";
// Diagnostics: proves the module can read the client's memory.
inline constexpr const char* kProbeMemory   = "probe_memory";
}  // namespace action

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

struct Envelope {
  int         v  = kVersion;
  std::string type;
  // Correlates an `action` with the `result` it produced. 0 = unsolicited.
  std::uint64_t id = 0;
  // Milliseconds since the Unix epoch, stamped by the sender.
  std::int64_t  ts = 0;
  json          payload = json::object();
};

inline void to_json(json& j, const Envelope& e) {
  j = json{{"v", e.v}, {"type", e.type}, {"id", e.id}, {"ts", e.ts},
           {"payload", e.payload}};
}

inline void from_json(const json& j, Envelope& e) {
  e.v       = j.value("v", 0);
  e.type    = j.value("type", std::string{});
  e.id      = j.value("id", std::uint64_t{0});
  e.ts      = j.value("ts", std::int64_t{0});
  e.payload = j.value("payload", json::object());
}

std::int64_t NowMillis();

// Builds an envelope with `ts` already stamped.
Envelope Make(std::string type, json payload, std::uint64_t id = 0);

}  // namespace gtabot::proto
