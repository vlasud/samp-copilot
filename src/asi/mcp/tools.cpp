#include "mcp/tools.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

#include "bridge.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "mcp/server.hpp"
#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "game/paths.hpp"
#include "game/world_query.hpp"
#include "nav/planner.hpp"
#include "samp/chat.hpp"
#include "samp/world.hpp"
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

json Point(const game::Vec3& p) { return json{p.x, p.y, p.z}; }

json VerdictJson(const nav::Verdict& v) {
  json out{{"ok", v.ok}};
  if (!v.ok) out["why"] = v.why;
  out["ground_z"] = v.ground_z;
  out["at"] = Point(v.where);
  return out;
}

// A point from the arguments. Without a z the player's own height is used,
// which is right for anywhere on the same floor as him: the ground is then
// found from a little above that.
bool PointFrom(const json& args, const samp::LocalPed& self, game::Vec3* out) {
  if (!args.contains("x") || !args.contains("y")) return false;
  out->x = args["x"].get<float>();
  out->y = args["y"].get<float>();
  out->z = args.contains("z") ? args["z"].get<float>() : self.z;
  return true;
}

json CheckPoint(const json& args) {
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) throw std::runtime_error("the local player is not readable");
  if (!game::CallsTrusted())
    throw std::runtime_error("game calls are not verified on this build");
  game::Vec3 p;
  if (!PointFrom(args, self, &p)) throw std::runtime_error("x and y are required");
  const game::Vec3 here{self.x, self.y, self.z};
  json out;
  out["point"]    = Point(p);
  out["distance"] = std::sqrt((p.x - self.x) * (p.x - self.x) +
                              (p.y - self.y) * (p.y - self.y));
  out["standable"] = VerdictJson(nav::Standable(p));
  out["walkable_from_here"] = VerdictJson(nav::Walkable(here, p));
  return out;
}

json PlanTo(const json& args) {
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) throw std::runtime_error("the local player is not readable");
  if (!game::CallsTrusted())
    throw std::runtime_error("game calls are not verified on this build");
  game::Vec3 target;
  if (!PointFrom(args, self, &target))
    throw std::runtime_error("x and y are required");
  const game::Vec3 here{self.x, self.y, self.z};
  const nav::Plan plan = nav::PlanPath(here, target);
  nav::SetDebugPlan(target, plan);

  json legs = json::array();
  for (const nav::Leg& leg : plan.legs) {
    json entry{{"from", Point(leg.from)},
               {"to", Point(leg.to)},
               {"ok", leg.ok},
               {"verified", leg.verified},
               {"via_graph", leg.via_graph}};
    if (!leg.why.empty()) entry["why"] = leg.why;
    legs.push_back(std::move(entry));
  }
  json waypoints = json::array();
  for (const game::Vec3& p : plan.waypoints) waypoints.push_back(Point(p));
  return json{{"ok", plan.ok},
              {"note", plan.note},
              {"length_m", plan.length_m},
              {"graph_nodes", plan.graph_nodes},
              {"blocked_legs", plan.blocked_legs},
              {"game_calls", plan.game_calls},
              {"waypoints", std::move(waypoints)},
              {"legs", std::move(legs)}};
}

json WalkStatus() {
  const act::Status walk = act::Get();
  json out{{"walking", walk.walking},
           {"note", walk.note},
           {"leg", walk.leg},
           {"legs", walk.legs},
           {"to_next_m", walk.to_next_m},
           {"remaining_m", walk.remaining_m},
           {"sidesteps", walk.sidesteps}};
  if (walk.corrected) out["steering_corrected_deg"] = walk.error_deg;
  return out;
}

json TravelStatusJson() {
  const act::TravelStatus trip = act::TravelGet();
  json out{{"travelling", trip.travelling},
           {"note", trip.note},
           {"straight_m", trip.straight_m},
           {"legs_planned", trip.replans},
           {"heading_for_staging_point", trip.reaching},
           {"destination", Point(trip.destination)}};
  out["walk"] = WalkStatus();
  return out;
}

json TravelTo(const json& args) {
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) throw std::runtime_error("the local player is not readable");
  if (!game::CallsTrusted())
    throw std::runtime_error("game calls are not verified on this build");
  game::Vec3 target;
  if (!PointFrom(args, self, &target))
    throw std::runtime_error("x and y are required");
  act::TravelTo(target);
  return TravelStatusJson();
}

json MoveTo(const json& args) {
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) throw std::runtime_error("the local player is not readable");
  if (!game::CallsTrusted())
    throw std::runtime_error("game calls are not verified on this build");
  game::Vec3 target;
  if (!PointFrom(args, self, &target))
    throw std::runtime_error("x and y are required");

  const game::Vec3 here{self.x, self.y, self.z};
  const nav::Plan plan = nav::PlanPath(here, target);
  nav::SetDebugPlan(target, plan);
  if (!plan.ok || plan.waypoints.size() < 2)
    throw std::runtime_error("no walkable route: " + plan.note);

  // The first waypoint is where he already is.
  act::WalkTo(std::vector<game::Vec3>(plan.waypoints.begin() + 1,
                                      plan.waypoints.end()));
  json out = WalkStatus();
  out["route_note"]  = plan.note;
  out["route_length_m"] = plan.length_m;
  return out;
}

json NavNodes(const json& args) {
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) throw std::runtime_error("the local player is not readable");
  const game::PathLayout& graph = game::CachedPaths();
  json out{{"graph", graph.valid},
           {"note", graph.note},
           {"areas_loaded", graph.loaded_areas},
           {"ped_nodes_loaded", graph.ped_nodes_loaded}};
  json nodes = json::array();
  if (graph.valid) {
    const float radius = args.value("radius", 60.0f);
    const std::size_t limit = args.value("limit", std::size_t{100});
    for (const game::PathNode& node :
         game::PedNodesNear(game::Vec3{self.x, self.y, self.z}, radius, limit)) {
      game::PathLink links[16];
      const int count = game::ReadLinks(node, links, 16);
      json linked = json::array();
      for (int i = 0; i < count; ++i)
        linked.push_back({{"area", links[i].area}, {"index", links[i].index}});
      nodes.push_back({{"area", node.area},
                       {"index", node.index},
                       {"pos", Point(node.pos)},
                       {"links", std::move(linked)}});
    }
  }
  out["count"] = nodes.size();
  out["nodes"] = std::move(nodes);
  return out;
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
      "check_point",
      "Whether the character could stand at a point and walk there in a "
      "straight line from where he is. Both answers say why when they are "
      "no. 'No ground' far away means the game has not streamed that far, "
      "not that there is a hole. Positions are ped-origin height, like "
      "self.pos in get_world.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}},
         {"z", {{"type", "number"},
                {"description", "Optional; the ground is found without it."}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread([args] { return CheckPoint(args); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "plan_path",
      "A walkable route from the character to a point: straight when the "
      "straight line is clear, otherwise along the pavements the game's own "
      "pedestrians use, pulled tight. Every leg is checked against the world "
      "and says why when it is blocked. The plan is also drawn in the game "
      "for anyone watching.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}},
         {"z", {{"type", "number"}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread([args] { return PlanTo(args); },
                                    kScanTimeoutMs);
      },
  });

  server->AddTool({
      "get_nav_nodes",
      "The game's ped nodes near the character - the points its pedestrians "
      "walk between. Where a character can naturally go. Only loaded areas "
      "are known, which covers a few hundred metres around him.",
      {{"type", "object"},
       {"properties",
        {{"radius", {{"type", "number"}, {"description", "Metres, default 60."}}},
         {"limit", {{"type", "integer"}, {"description", "Default 100."}}}}}},
      [](const json& args) {
        return Rpc::RunOnGameThread([args] { return NavNodes(args); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "move_to",
      "Walk the character to a point. Plans a route the same way plan_path "
      "does, then works his controller so the game walks him along it - the "
      "same speed and animation as a person at the keyboard, and the same "
      "position updates to the server. Returns immediately; call walk_status "
      "to see how it is going.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}},
         {"z", {{"type", "number"}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread([args] { return MoveTo(args); },
                                    kScanTimeoutMs);
      },
  });

  server->AddTool({
      "travel_to",
      "Go to a point, however far. Unlike move_to, this keeps the destination "
      "rather than a route: it plans as far toward it as the loaded world "
      "allows, walks that, and plans again from wherever it ends up - because "
      "more of the city streams in as you go, and because a car that blocked a "
      "leg a minute ago may have driven off. Returns at once; poll "
      "travel_status.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}},
         {"z", {{"type", "number"}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread([args] { return TravelTo(args); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "travel_status",
      "How the journey is going: how far is left in a straight line, how many "
      "legs have been planned, and whether it is heading for the destination "
      "or for a staging point on the way to it.",
      NoArguments(),
      [](const json&) {
        // On the game thread: this reads SA-MP's structures, and resolving
        // those is game-thread work that keeps state of its own.
        return Rpc::RunOnGameThread([] { return TravelStatusJson(); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "walk_status",
      "How the current walk is going, or why the last one ended: arrived, "
      "stuck, out of time, or stopped.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread([] { return WalkStatus(); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "set_movement",
      "Arms the character's movement, or stands it down. Nothing that moves "
      "him works until this is on: it is off when the game starts, and the "
      "F11 menu's own switch is the same one. Arming runs the two checks that "
      "decide whether the world reads correctly here - the ground under him "
      "must be about a ped's height below him, and the space he stands in "
      "must read as clear - and reports them, so a test run can wait for "
      "'ready' rather than guess.",
      {{"type", "object"},
       {"properties", {{"on", {{"type", "boolean"},
                               {"description", "True to arm, false to stand down."}}}}},
       {"required", json::array({"on"})}},
      [](const json& args) {
        const bool on = args.value("on", false);
        return Rpc::RunOnGameThread(
            [on]() -> json {
              if (!on) {
                act::CancelTravel("stood down over the interface");
                act::Stop("stood down over the interface");
                game::SetEnabled(false);
                return json{{"movement", "off"}, {"ready", false}};
              }
              game::SetEnabled(true);
              const samp::LocalPed self = samp::ReadLocalPed();
              const char* why = "the local player is not readable yet";
              bool ground = false, sight = false;
              if (self.valid) {
                const game::Vec3 here{self.x, self.y, self.z};
                ground = game::SelfCheck(here, &why);
                if (ground) sight = game::SelfCheckLineOfSight(here, &why);
              }
              return json{{"movement", "on"},
                          {"ground_verified", ground},
                          {"line_of_sight_verified", sight},
                          {"ready", game::CallsTrusted() && game::LineOfSightAvailable()},
                          {"note", why}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "stop",
      "Let go of the controller. The character stops where he is and the "
      "player's own input passes through untouched again.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            [] {
              act::CancelTravel("stopped on request");
              act::Stop("stopped on request");
              return TravelStatusJson();
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
