//
// gta-mcp.exe - owns the named pipe, keeps the collected state, and exposes it
// to an agent over MCP on stdio.
//
#include <cstdio>
#include <stdexcept>
#include <string>

#include "common/pipe.hpp"
#include "common/protocol.hpp"
#include "rpc.hpp"
#include "server.hpp"
#include "state.hpp"

namespace {

// A scan of samp.dll alone is quick; sweeping the whole process is a second or
// two of memcmp, and it runs inside the game's frame.
constexpr int kActionTimeoutMs      = 5000;
constexpr int kProcessScanTimeoutMs = 60000;

void RequireConnected(const gtabot::ipc::Endpoint& link) {
  if (!link.connected())
    throw std::runtime_error(
        "bot.asi is not connected - start the game with bot.asi in the game "
        "folder");
}

}  // namespace

int main() {
  using namespace gtabot;

  mcp::State state;
  mcp::Rpc   rpc;
  ipc::PipeServer link;

  link.Start(
      [&state, &rpc](const proto::Envelope& env) {
        state.OnMessage(env);
        if (env.type == proto::msg::kResult) rpc.Complete(env.id, env.payload);
      },
      [&state, &rpc](bool up) {
        state.OnLinkState(up);
        if (!up)
          rpc.FailAll("the game disconnected while the action was in flight");
        std::fprintf(stderr, "[ipc] link %s\n", up ? "up" : "down");
      });

  // Sends an action and blocks until the game answers inside its frame.
  auto invoke = [&link, &rpc](const std::string& kind, mcp::json payload,
                              int timeout_ms) -> mcp::json {
    RequireConnected(link);
    payload["kind"] = kind;
    const std::uint64_t id = rpc.NextId();
    if (!link.Send(proto::Make(proto::msg::kAction, std::move(payload), id)))
      throw std::runtime_error("failed to write to the game");

    mcp::json result = rpc.Await(id, timeout_ms);
    if (!result.value("ok", false))
      throw std::runtime_error(
          result.value("error", std::string{"the action failed"}));
    return result.value("data", mcp::json::object());
  };

  mcp::Server server("gta-mcp", GTABOT_VERSION);

  server.AddTool({
      "bot_status",
      "Whether the in-game module is attached, what SA-MP build it found, and "
      "how much data has arrived. Call this first when anything looks wrong.",
      {{"type", "object"}, {"properties", mcp::json::object()}},
      [&state](const mcp::json&) { return state.StatusJson(); },
  });

  server.AddTool({
      "get_snapshot",
      "The most recent world snapshot published by the in-game module, "
      "including the frame counter and measured FPS.",
      {{"type", "object"}, {"properties", mcp::json::object()}},
      [&state](const mcp::json&) { return state.LatestSnapshot(); },
  });

  server.AddTool({
      "get_events",
      "Recent in-game events (chat lines, dialogs, connect/disconnect), oldest "
      "first.",
      {{"type", "object"},
       {"properties",
        {{"limit",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 500},
           {"description", "How many events to return. Defaults to 50."}}}}}},
      [&state](const mcp::json& args) {
        return state.RecentEvents(args.value("limit", std::size_t{50}));
      },
  });

  server.AddTool({
      "probe_memory",
      "Proves the module really reads the SA-MP client's live memory. Searches "
      "the running process for a literal string and reports where it was "
      "found, with a hexdump around each hit. With no needle it looks for the "
      "nickname the launcher passed on the command line. Scope 'samp' searches "
      "samp.dll only and is fast; 'process' sweeps everything and briefly "
      "stutters the game.",
      {{"type", "object"},
       {"properties",
        {{"needle",
          {{"type", "string"},
           {"description",
            "Literal ASCII text to look for - a nickname, or something "
            "visible on screen right now."}}},
         {"scope",
          {{"type", "string"},
           {"enum", mcp::json::array({"samp", "process"})},
           {"description", "Defaults to 'samp'."}}},
         {"max_hits",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}}}},
      [&invoke](const mcp::json& args) {
        const bool whole_process =
            args.value("scope", std::string{"samp"}) == "process";
        return invoke(proto::action::kProbeMemory, args,
                      whole_process ? kProcessScanTimeoutMs : kActionTimeoutMs);
      },
  });

  server.AddTool({
      "send_chat",
      "Send a line of text or a slash command to the server chat as the "
      "player.",
      {{"type", "object"},
       {"properties",
        {{"text",
          {{"type", "string"},
           {"maxLength", 128},
           {"description", "The line to send, including any leading slash."}}}}},
       {"required", mcp::json::array({"text"})}},
      [&invoke](const mcp::json& args) {
        const std::string text = args.value("text", std::string{});
        if (text.empty()) throw std::runtime_error("text must not be empty");
        return invoke(proto::action::kChatSend, {{"text", text}},
                      kActionTimeoutMs);
      },
  });

  std::fprintf(stderr, "[mcp] gta-mcp %s ready, pipe %s\n", GTABOT_VERSION,
               proto::kPipeName);
  server.Run();

  link.Stop();
  return 0;
}
