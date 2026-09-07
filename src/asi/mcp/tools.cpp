#include "mcp/tools.hpp"

#include <stdexcept>
#include <string>

#include "bridge.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "mcp/server.hpp"
#include "samp/chat.hpp"
#include "samp/discovery.hpp"
#include "state/probe.hpp"
#include "types.hpp"

namespace gtabot::mcp {
namespace {

// A scan of samp.dll alone takes a frame or two; sweeping the whole process is
// a second of memcmp, and it runs inside the game's frame.
constexpr int kFastTimeoutMs = 5000;
constexpr int kScanTimeoutMs = 60000;

json NoArguments() {
  return json{{"type", "object"}, {"properties", json::object()}};
}

}  // namespace

void RegisterTools(Server* server) {
  server->AddTool({
      "bot_status",
      "Whether the module is hooked into the game, which SA-MP build it found, "
      "and - when the frame counter is not moving - the verdict explaining "
      "why. Call this first when anything looks wrong.",
      NoArguments(),
      [](const json&) {
        // Status is assembled off the game thread on purpose: it has to keep
        // answering precisely when the game thread has stopped running.
        json status = asi::BuildStatusSnapshot();
        std::int64_t world_age_ms = -1;
        status["world"]        = asi::Bridge::GetWorld(&world_age_ms);
        status["world_age_ms"] = world_age_ms;
        return status;
      },
  });

  server->AddTool({
      "get_world",
      "The most recent world state the game thread managed to build: the local "
      "player, nearby players and vehicles. Check world_age_ms - a large value "
      "means the game is not rendering and this is stale.",
      NoArguments(),
      [](const json&) {
        std::int64_t world_age_ms = -1;
        json world = asi::Bridge::GetWorld(&world_age_ms);
        return json{{"world", std::move(world)}, {"world_age_ms", world_age_ms}};
      },
  });

  server->AddTool({
      "get_chat",
      "The chat log as the player sees it: server messages, other players "
      "talking, and anything the server printed. Oldest first. 'order' says "
      "whether the module could establish the direction of the array from "
      "timestamps or is assuming it.",
      {{"type", "object"},
       {"properties",
        {{"limit",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 200},
           {"description", "How many of the most recent lines to return. "
                           "Defaults to 40."}}}}}},
      [](const json& args) {
        const int limit = args.value("limit", 40);
        return Rpc::RunOnGameThread([limit] { return samp::ReadChat(limit); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "dump_chat",
      "Writes bot.chat-dump.txt next to the module: the shape the chat log was "
      "recognised by, and the raw bytes of the last few entries. This is how a "
      "column the search labelled wrongly gets corrected.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            []() -> json {
              if (!samp::DumpChat())
                throw std::runtime_error("could not write the dump");
              return json{{"path", ModuleDirectory() + "bot.chat-dump.txt"}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
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
           {"enum", json::array({"samp", "process"})},
           {"description", "Defaults to 'samp'."}}},
         {"max_hits", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}}}},
      [](const json& args) {
        const bool whole_process =
            args.value("scope", std::string{"samp"}) == "process";
        return Rpc::RunOnGameThread(
            [args] { return asi::ProbeMemory(args); },
            whole_process ? kScanTimeoutMs : kFastTimeoutMs);
      },
  });

  server->AddTool({
      "dump_samp_structures",
      "Writes bot.samp-report.txt next to the module: every live copy of the "
      "player nickname in memory, annotated with what points at it and what "
      "surrounds it. This is how the player pool's layout gets established for "
      "a build instead of guessed. Needs the client to be in a server.",
      {{"type", "object"},
       {"properties",
        {{"needle",
          {{"type", "string"},
           {"description",
            "Search for this instead of the launcher's nickname."}}}}}},
      [](const json& args) {
        const std::string needle = args.value("needle", std::string{});
        return Rpc::RunOnGameThread(
            [needle]() -> json {
              const samp::ReportOutcome outcome =
                  samp::WriteStructureReport(needle);
              if (!outcome.written) throw std::runtime_error(outcome.error);
              return json{{"path", outcome.path},
                          {"live_occurrences", outcome.heap_hits},
                          {"image_occurrences", outcome.module_hits}};
            },
            kScanTimeoutMs);
      },
  });

  server->AddTool({
      "send_chat",
      "Send a line of text or a slash command to the server chat as the "
      "player.",
      {{"type", "object"},
       {"properties",
        {{"text",
          {{"type", "string"},
           {"maxLength", 128},
           {"description", "The line to send, including any leading slash."}}}}},
       {"required", json::array({"text"})}},
      [](const json& args) -> json {
        const std::string text = args.value("text", std::string{});
        if (text.empty()) throw std::runtime_error("text must not be empty");
        // Deliberately explicit: reporting success for something that did not
        // happen would have the agent build on a lie.
        throw std::runtime_error(
            std::string("action not implemented yet: ") + action::kChatSend);
      },
  });
}

}  // namespace gtabot::mcp
