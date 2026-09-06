//
// gta-mcp.exe - owns the named pipe, keeps the collected state, and exposes it
// to an agent over MCP on stdio.
//
#include <atomic>
#include <cstdio>
#include <stdexcept>

#include "common/pipe.hpp"
#include "common/protocol.hpp"
#include "server.hpp"
#include "state.hpp"

namespace {

std::atomic<std::uint64_t> g_next_action_id{1};

gtabot::proto::json RequireConnected(const gtabot::ipc::Endpoint& link) {
  if (!link.connected())
    throw std::runtime_error(
        "bot.asi is not connected - start the game with bot.asi in the game "
        "folder");
  return {};
}

}  // namespace

int main() {
  using namespace gtabot;

  mcp::State state;
  ipc::PipeServer link;
  link.Start([&state](const proto::Envelope& e) { state.OnMessage(e); },
             [&state](bool up) {
               state.OnLinkState(up);
               std::fprintf(stderr, "[ipc] link %s\n", up ? "up" : "down");
             });

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
      "The most recent world snapshot published by the in-game module.",
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
      "send_chat",
      "Send a line of text or a slash command to the server chat as the "
      "player.",
      {{"type", "object"},
       {"properties",
        {{"text",
          {{"type", "string"},
           {"maxLength", 128},
           {"description", "The line to send, including any leading '/'."}}}}},
       {"required", mcp::json::array({"text"})}},
      [&link](const mcp::json& args) {
        RequireConnected(link);
        const std::string text = args.value("text", std::string{});
        if (text.empty()) throw std::runtime_error("text must not be empty");

        const std::uint64_t id = g_next_action_id++;
        const bool sent = link.Send(proto::Make(
            proto::msg::kAction,
            {{"kind", proto::action::kChatSend}, {"text", text}}, id));
        if (!sent) throw std::runtime_error("failed to write to the game");
        return mcp::json{{"queued", true}, {"action_id", id}};
      },
  });

  std::fprintf(stderr, "[mcp] gta-mcp %s ready, pipe %s\n", GTABOT_VERSION,
               proto::kPipeName);
  server.Run();

  link.Stop();
  return 0;
}
