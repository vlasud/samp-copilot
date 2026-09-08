#include "mcp/tools.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "mcp/server.hpp"
#include "actions/driver.hpp"
#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "game/bindings.hpp"
#include "game/paths.hpp"
#include "game/world_query.hpp"
#include "nav/indoors.hpp"
#include "nav/planner.hpp"
#include "samp/chat.hpp"
#include "samp/dialog.hpp"
#include "samp/input_state.hpp"
#include "samp/keys.hpp"
#include "samp/labels.hpp"
#include "samp/objects.hpp"
#include "samp/textdraws.hpp"
#include "samp/login.hpp"
#include "samp/world.hpp"
#include "samp/discovery.hpp"
#include "samp/version.hpp"
#include "state/events.hpp"
#include "state/memory.hpp"
#include "state/probe.hpp"
#include "types.hpp"

namespace gtabot::mcp {
namespace {

// A scan of samp.dll alone takes a frame or two; sweeping the whole process is
// a second of memcmp, and it runs inside the game's frame.
constexpr int kFastTimeoutMs = 5000;
// The first look for the three-dimensional text walks the client's memory.
constexpr int kSlowTimeoutMs = 30000;
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

json DriveStatusJson() {
  const act::DriveStatus drive = act::DriveGet();
  json out{{"driving", drive.driving},
              {"note", drive.note},
              {"leg", drive.leg},
              {"legs", drive.legs},
              {"to_next_m", drive.to_next_m},
              {"remaining_m", drive.remaining_m},
              {"speed_kmh", drive.speed_kmh},
              {"heading_error_deg", drive.heading_error_deg},
              {"times_stuck", drive.times_stuck}};
  // A car that will not go on is usually being told why, in writing, by
  // something it is parked in front of.
  if (drive.times_stuck > 0) {
    const samp::LocalPed self = samp::ReadLocalPed();
    if (self.valid) {
      const game::Vec3 here{self.x, self.y, self.z};
      json writing = json::array();
      for (const samp::ObjectText& painted : samp::ObjectTextsNear(here, 60.0f, 6))
        writing.push_back(json{{"away_m", painted.away_m}, {"text", painted.text}});
      for (const samp::Label& label : samp::LabelsNear(here, 60.0f, 4))
        writing.push_back(json{{"away_m", label.away_m}, {"text", label.text}});
      if (!writing.empty()) out["written_round_about"] = std::move(writing);
    }
  }
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
  float stop_within = 2.5f;
  if (args.contains("stop_within") && args["stop_within"].is_number())
    stop_within = args["stop_within"].get<float>();
  act::TravelTo(target, /*height_unknown=*/false, stop_within);
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
      "travel_status. stop_within is how near counts as arrived, in metres: "
      "2.5 by default, which is a pavement's width - pass 0.8 to step onto a "
      "pickup or up to a counter.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}},
         {"z", {{"type", "number"}}},
         {"stop_within", {{"type", "number"}}}}},
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
      "ready",
      "One call that says where the session stands and what to do next: "
      "whether the client is up, whether the character has spawned, what "
      "dialog the server is showing, whether movement is armed and the world "
      "reads. Poll this rather than guessing - 'next' names the call to make.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            []() -> json {
              const samp::Client client = samp::Detect();
              const samp::Dialog dialog = samp::CurrentDialog();
              const samp::InputSwitch state = samp::ReadInputSwitch();
              const samp::LocalPed self = samp::ReadLocalPed();
              const bool spawned = state.player_known && state.active != 0;
              const bool armed = game::Enabled();
              const bool readable = game::CallsTrusted() && game::LineOfSightAvailable();

              json out;
              out["client_found"] = client.base != 0;
              out["connection"] = samp::ConnectionState();
              out["spawned"]      = spawned;
              out["movement_armed"] = armed;
              out["world_readable"] = readable;
              out["login"] = samp::LoginSent() ? "sent"
                             : samp::LoginConfigured() ? "ready" : "none";
              if (self.valid)
                out["position"] = json{{"x", self.x}, {"y", self.y}, {"z", self.z}};
              json shown{{"shown", dialog.shown}};
              if (dialog.shown) {
                shown["style"]   = samp::DialogStyleName(dialog.style);
                shown["caption"] = dialog.caption;
                shown["id"]      = dialog.id;
              }
              out["dialog"] = shown;
              // The ped's own state: fifty is at the wheel or in a seat.
              int ped_state = -1;
              if (self.valid && self.game_ped != 0)
                asi::mem::Read<int>(self.game_ped + 0x530, &ped_state);
              out["in_vehicle"] = ped_state == 50;
              out["ready_to_travel"] = spawned && armed && readable && !dialog.shown;

              const char* next = "ready: call travel_to with x and y";
              if (client.base == 0) {
                next = "the SA-MP client is not loaded yet - wait";
              } else if (out["connection"] == "not connected") {
                next = "the client is not connected - it was kicked or the "
                       "server dropped it; the game has to be started again";
              } else if (dialog.shown && dialog.style == 3) {
                next = samp::LoginConfigured()
                           ? "the server is asking for a password - it is being "
                             "answered from bot.login; wait, or call login"
                           : "the server is asking for a password and there is no "
                             "bot.login - a person has to type it";
              } else if (dialog.shown) {
                next = "a dialog is on screen and he cannot move until it is "
                       "answered - read it in 'dialog'";
              } else if (!spawned) {
                next = "the character has not spawned yet - wait";
              } else if (!armed || !readable) {
                next = "call set_movement with on true";
              }
              out["next"] = next;
              return out;
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "events",
      "What has happened since you last looked: lines of chat, a dialog "
      "opening or closing, spawning, dying, a journey ending. Pass the 'next' "
      "from the previous call as 'since' and this returns only what came "
      "after it, oldest first. This is the call to poll - everything else "
      "describes how things are now, and a dialog that opened and closed "
      "between two of those is a dialog you never saw.",
      {{"type", "object"},
       {"properties",
        {{"since",
          {{"type", "integer"},
           {"description", "The 'next' from the previous call. Zero, or "
                           "absent, means everything still remembered."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 200},
           {"description", "At most this many. Defaults to fifty."}}},
         {"kinds",
          {{"type", "array"},
           {"items", {{"type", "string"}}},
           {"description", "Only these kinds: chat, dialog, spawn, unspawn, "
                           "death, travel, login. A busy server's chat drowns "
                           "everything else, so name what you care about."}}}}}},
      [](const json& args) {
        const long long since = args.value("since", 0LL);
        const int limit = args.value("limit", 50);
        std::vector<std::string> kinds;
        if (args.contains("kinds") && args["kinds"].is_array())
          for (const json& kind : args["kinds"])
            if (kind.is_string()) kinds.push_back(kind.get<std::string>());
        return state::EventsSince(since, limit, kinds);
      },
  });

  server->AddTool({
      "get_dialog",
      "What the server is showing, if anything: the style, the caption and "
      "the text. A character at a dialog cannot move, so this is the first "
      "thing to look at when he will not.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            []() -> json {
              const samp::Dialog dialog = samp::CurrentDialog();
              json out{{"readable", dialog.valid}, {"shown", dialog.shown}};
              if (dialog.shown) {
                out["id"]      = dialog.id;
                out["style"]   = samp::DialogStyleName(dialog.style);
                out["style_number"] = dialog.style;
                out["caption"] = dialog.caption;
                out["text"]    = dialog.text;
              }
              return out;
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "get_textdraws",
      "What the server has written on the screen: the money in the corner, "
      "the hunger bar, the prompt saying which key opens the thing in front "
      "of him. Many servers put their whole interface there and none of it "
      "reaches the chat. 'for_me' marks the ones addressed to this player "
      "rather than shown to everybody.",
      {{"type", "object"},
       {"properties",
        {{"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 300},
           {"description", "At most this many. Defaults to eighty."}}}}}},
      [](const json& args) -> json {
        const std::size_t limit = args.value("limit", 80);
        json out = json::array();
        for (const samp::TextDraw& draw : samp::TextDraws(limit)) {
          json one{{"id", draw.id},
                   {"for_me", draw.for_me},
                   {"text", draw.text},
                   {"x", draw.x},
                   {"y", draw.y}};
          if (draw.model != 0) one["model"] = draw.model;
          out.push_back(std::move(one));
        }
        return json{{"textdraws", std::move(out)}, {"note", samp::TextDrawsNote()}};
      },
  });

  server->AddTool({
      "get_pickups",
      "The things the server has put on the floor to be walked into: the way "
      "out of an interior, a shop counter, a job point, an entrance. Walking "
      "onto one is how a player uses it, so travel_to its position is how "
      "this character does.",
      {{"type", "object"},
       {"properties",
        {{"radius",
          {{"type", "number"}, {"minimum", 1}, {"maximum", 300},
           {"description", "How far to look. Defaults to forty metres."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 100},
           {"description", "At most this many. Defaults to twenty."}}}}}},
      [](const json& args) -> json {
        const float radius = args.value("radius", 40.0f);
        const std::size_t limit = args.value("limit", 20);
        std::int64_t age_ms = -1;
        const json world = asi::Bridge::GetWorld(&age_ms);
        const json pos = world.value("self", json::object()).value("pos", json::array());
        if (pos.size() < 3)
          throw std::runtime_error("where the character is is not known yet");
        const game::Vec3 here{pos[0].get<float>(), pos[1].get<float>(),
                              pos[2].get<float>()};
        json out = json::array();
        for (const samp::Pickup& one : samp::PickupsNear(here, radius, limit))
          out.push_back(json{{"id", one.id},
                             {"model", one.model},
                             {"type", one.type},
                             {"away_m", one.away_m},
                             {"at", json{{"x", one.at.x},
                                         {"y", one.at.y},
                                         {"z", one.at.z}}}});
        return json{{"pickups", std::move(out)}};
      },
  });

  server->AddTool({
      "map_room",
      "Feels out the room he is standing in and draws it. Indoors nothing "
      "else works: the game's pedestrian graph stops at the door of every "
      "building, and asking whether a place can be stood in wants a clear "
      "line from the floor to head height, which a low ceiling refuses "
      "everywhere. This asks a different question - can a knee and a chest "
      "pass from here to there - which a wall answers no and a doorway "
      "answers yes. Returns the room as a picture, the way through it, and "
      "where it ends nearest wherever you said you were going, which is the "
      "door whether it is open or shut.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}, {"description", "Where you are trying to get to."}}},
         {"y", {{"type", "number"}}},
         {"radius",
          {{"type", "number"}, {"minimum", 4}, {"maximum", 40},
           {"description", "How far around him to feel. Defaults to twenty metres."}}},
         {"picture",
          {{"type", "boolean"},
           {"description", "Include the drawing. On by default."}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              if (!self.valid)
                throw std::runtime_error("the local player is not readable");
              const game::Vec3 here{self.x, self.y, self.z};
              const game::Vec3 towards{args.value("x", self.x),
                                       args.value("y", self.y), self.z};
              const nav::Room room =
                  nav::MapRoom(here, towards, args.value("radius", 20.0f));
              json out{{"note", room.note},
                       {"cells_reached", room.cells_reached},
                       {"cell_m", room.cell_m},
                       {"reaches_target", room.reaches_target}};
              if (room.way_out_found)
                out["way_out"] = json{{"x", room.way_out.x},
                                      {"y", room.way_out.y},
                                      {"z", room.way_out.z},
                                      {"away_from_target_m", room.way_out_away_m}};
              json path = json::array();
              for (const game::Vec3& point : room.points)
                path.push_back(json{{"x", point.x}, {"y", point.y}});
              out["way_there"] = std::move(path);
              json doors = json::array();
              for (const game::Vec3& door : room.doors)
                doors.push_back(
                    json{{"x", door.x}, {"y", door.y}, {"z", door.z}});
              out["doors"] = std::move(doors);
              if (args.value("picture", true)) {
                json picture = json::array();
                for (const std::string& row : room.picture) picture.push_back(row);
                out["picture"] = std::move(picture);
              }
              return out;
            },
            kSlowTimeoutMs);
      },
  });

  server->AddTool({
      "get_objects",
      "The server's own objects near the character, nearest first, with the "
      "model each is made of. A room a server has built is these; so is a "
      "door that opens when it is pushed, a gate, a barrier. When the "
      "planner says there is no way out of somewhere, this is what is in "
      "the way.",
      {{"type", "object"},
       {"properties",
        {{"radius",
          {{"type", "number"}, {"minimum", 1}, {"maximum", 200},
           {"description", "How far to look. Defaults to fifteen metres."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 100},
           {"description", "At most this many. Defaults to twenty."}}},
         {"models",
          {{"type", "array"}, {"items", {{"type", "integer"}}},
           {"description", "Only these models. A room is hundreds of objects "
                           "and the interesting ones are a handful."}}},
         {"doors_only",
          {{"type", "boolean"},
           {"description", "Only the models that are doors."}}}}}},
      [](const json& args) -> json {
        const float radius = args.value("radius", 15.0f);
        const std::size_t limit = args.value("limit", 20);
        std::vector<int> wanted;
        if (args.contains("models") && args["models"].is_array())
          for (const json& model : args["models"])
            if (model.is_number_integer()) wanted.push_back(model.get<int>());
        const bool doors_only = args.value("doors_only", false);
        std::int64_t age_ms = -1;
        const json world = asi::Bridge::GetWorld(&age_ms);
        const json pos = world.value("self", json::object()).value("pos", json::array());
        if (pos.size() < 3)
          throw std::runtime_error("where the character is is not known yet");
        const game::Vec3 here{pos[0].get<float>(), pos[1].get<float>(),
                              pos[2].get<float>()};
        // Filtering has to happen before the count is cut, or asking for the
        // doors among a room's four hundred objects returns the twenty
        // nearest things that are not doors.
        const std::size_t sweep =
            (wanted.empty() && !doors_only) ? limit : std::size_t{1000};
        json out = json::array();
        for (const samp::NearObject& one : samp::ObjectsNear(here, radius, sweep)) {
          if (out.size() >= limit) break;
          if (doors_only && !samp::IsDoorModel(one.model)) continue;
          if (!wanted.empty() &&
              std::find(wanted.begin(), wanted.end(), one.model) == wanted.end())
            continue;
          out.push_back(json{{"id", one.id},
                             {"model", one.model},
                             {"away_m", one.away_m},
                             {"at", json{{"x", one.at.x},
                                         {"y", one.at.y},
                                         {"z", one.at.z}}}});
        }
        return json{{"objects", std::move(out)}};
      },
  });

  server->AddTool({
      "get_object_texts",
      "The words the server has painted onto things: the sign over a shop, "
      "the number on a house, the notice on a barrier saying which key opens "
      "it. Set on an object's material rather than hung in the air, so "
      "get_labels does not see them.",
      {{"type", "object"},
       {"properties",
        {{"radius",
          {{"type", "number"}, {"minimum", 1}, {"maximum", 1000},
           {"description", "How far to look. Defaults to eighty metres."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 100},
           {"description", "At most this many. Defaults to twenty."}}}}}},
      [](const json& args) -> json {
        const float radius = args.value("radius", 80.0f);
        const std::size_t limit = args.value("limit", 20);
        std::int64_t age_ms = -1;
        const json world = asi::Bridge::GetWorld(&age_ms);
        const json pos = world.value("self", json::object()).value("pos", json::array());
        if (pos.size() < 3)
          throw std::runtime_error("where the character is is not known yet");
        const game::Vec3 here{pos[0].get<float>(), pos[1].get<float>(),
                              pos[2].get<float>()};
        json out = json::array();
        for (const samp::ObjectText& painted :
             samp::ObjectTextsNear(here, radius, limit))
          out.push_back(json{{"object_id", painted.object_id},
                             {"material", painted.material},
                             {"model", painted.model},
                             {"text", painted.text},
                             {"font", painted.font},
                             {"away_m", painted.away_m},
                             {"at", json{{"x", painted.at.x},
                                         {"y", painted.at.y},
                                         {"z", painted.at.z}}}});
        return json{{"object_texts", std::move(out)},
                    {"note", samp::ObjectTextsNote()}};
      },
  });

  server->AddTool({
      "get_labels",
      "The three-dimensional text the server has hung in the air near the "
      "character, nearest first. A role-play server says a great deal this "
      "way and none of it reaches the chat: what a building is, whose house "
      "this is, what to press at the barrier ahead. Read this when something "
      "will not let him past and the chat says nothing.",
      {{"type", "object"},
       {"properties",
        {{"radius",
          {{"type", "number"}, {"minimum", 1}, {"maximum", 500},
           {"description", "How far to look. Defaults to sixty metres."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 100},
           {"description", "At most this many. Defaults to twenty."}}}}}},
      [](const json& args) -> json {
        {
              const float radius = args.value("radius", 60.0f);
              const std::size_t limit = args.value("limit", 20);
              // Off the game thread on purpose: this is reading memory, not
              // touching the client's live structures, and the first look
              // walks a lot of it. The position comes from the snapshot the
              // game thread keeps up to date.
              std::int64_t age_ms = -1;
              const json world = asi::Bridge::GetWorld(&age_ms);
              const json me = world.value("self", json::object());
              const json pos = me.value("pos", json::array());
              if (pos.size() < 3)
                throw std::runtime_error("where the character is is not known yet");
              const game::Vec3 here{pos[0].get<float>(), pos[1].get<float>(),
                                    pos[2].get<float>()};
              json out = json::array();
              for (const samp::Label& label : samp::LabelsNear(here, radius, limit))
                out.push_back(json{{"id", label.id},
                                   {"text", label.text},
                                   {"away_m", label.away_m},
                                   {"at", json{{"x", label.at.x},
                                               {"y", label.at.y},
                                               {"z", label.at.z}}},
                                   {"draw_distance", label.draw_distance},
                                   {"attached_to_player", label.attached_to_player},
                                   {"attached_to_vehicle", label.attached_to_vehicle}});
              json result{{"labels", out}, {"note", samp::LabelsNote()}};
              if (out.empty()) {
                // Nothing near: say what the table does hold, so the next
                // question is about the right thing.
                json sample = json::array();
                for (const samp::Label& label : samp::LabelsAny(here, 6))
                  sample.push_back(json{{"text", label.text},
                                        {"away_m", label.away_m},
                                        {"at", json{{"x", label.at.x},
                                                    {"y", label.at.y},
                                                    {"z", label.at.z}}}});
                result["nothing_near_but_the_table_holds"] = std::move(sample);
              }
              return result;
        }
      },
  });

  server->AddTool({
      "get_bindings",
      "The player's own controller table: which key each of the game's "
      "actions is on. A server's prompt names a key - \"press Alt\" - and "
      "this says what that key is on this installation, rather than assuming "
      "the defaults.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            [] {
              json rows = json::array();
              for (const game::Binding& b : game::AllBindings()) {
                json row{{"action", b.action}};
                if (!b.primary.empty()) row["primary"] = b.primary;
                if (!b.alternative.empty()) row["alternative"] = b.alternative;
                rows.push_back(std::move(row));
              }
              return json{{"bindings", rows}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "press_key",
      "Holds one key down for a moment, the way a hand does. Names the "
      "player's own bindings rather than keys: 'horn' is what opens a barrier "
      "that listens for one, 'accelerate', 'brake', 'left', 'right', "
      "'enter_exit', 'jump', 'sprint', 'handbrake'. A single character (\"y\", "
      "\"2\") presses that key instead, which is how a server's own prompts "
      "are answered. 'walk' (also called 'use') is the key servers watch as "
      "KEY_WALK - Left Alt by default - and is what a prompt saying \"press "
      "Alt\" means. Key names work too: alt, enter, space, tab, esc, f1-f12, "
      "up, down, left, right.",
      {{"type", "object"},
       {"properties",
        {{"key",
          {{"type", "string"},
           {"description", "A binding name, or one character to press."}}},
         {"ms",
          {{"type", "integer"}, {"minimum", 30}, {"maximum", 5000},
           {"description", "How long to hold it. Defaults to 200."}}}}},
       {"required", json::array({"key"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const std::string name = args.value("key", std::string{});
              const int ms = args.value("ms", 200);
              int vk = 0;
              if (name == "horn")        vk = game::KeyForAction(game::kVehicleHorn, 'H');
              else if (name == "accelerate") vk = game::KeyForAction(game::kVehicleAccelerate, 'W');
              else if (name == "brake")  vk = game::KeyForAction(game::kVehicleBrake, 'S');
              else if (name == "left")   vk = game::KeyForAction(game::kVehicleSteerLeft, VK_LEFT);
              else if (name == "right")  vk = game::KeyForAction(game::kVehicleSteerRight, VK_RIGHT);
              else if (name == "enter_exit") vk = game::KeyForAction(game::kVehicleEnterExit, VK_RETURN);
              else if (name == "jump")   vk = game::KeyForAction(game::kJumping, VK_LSHIFT);
              else if (name == "sprint") vk = game::KeyForAction(game::kSprint, VK_SPACE);
              else if (name == "handbrake") vk = game::KeyForAction(game::kVehicleHandbrake, VK_SPACE);
              else if (name == "walk" || name == "use")
                vk = game::KeyForAction(game::kPedWalk, VK_LMENU);
              else if (name == "duck") vk = game::KeyForAction(game::kPedDuck, 'C');
              else vk = game::KeyFromName(name);
              if (vk == 0) throw std::runtime_error("no key called \"" + name + "\"");
              if (samp::KeysBusy())
                throw std::runtime_error("keys are still being played - try again");
              samp::KeysPressFor(vk, ms / 16);
              return json{{"pressed", game::KeyName(vk)}, {"held_ms", ms}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "drive_to",
      "Drives the car he is sitting in to a point, along the roads the game's "
      "own traffic uses. Nothing is asked of the world for this: a link "
      "between two road nodes is a road. He must already be in a vehicle - "
      "travel_to the car, use_vehicle, then this. Returns at once; poll "
      "drive_status.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              if (!self.valid)
                throw std::runtime_error("the local player is not readable");
              game::Vec3 target;
              if (!PointFrom(args, self, &target))
                throw std::runtime_error("x and y are required");
              std::string note;
              if (!act::DriveTo(target, &note)) throw std::runtime_error(note);
              json out = DriveStatusJson();
              out["route"] = note;
              return out;
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "drive_status",
      "How the drive is going: which point of the road route he is heading "
      "for, how far is left, how fast he is going, how far off the line he is "
      "pointed, and how many times he has had to back out of something.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread([] { return DriveStatusJson(); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "use_vehicle",
      "Presses the key the player has bound to getting in and out of a "
      "vehicle. Standing beside one he gets in; sitting in one he gets out - "
      "the game decides which, exactly as it does for a person, so walk him "
      "next to the car first with travel_to and read 'in_vehicle' afterwards "
      "to see what came of it. Nothing is forced: a locked car stays locked.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            []() -> json {
              if (samp::KeysBusy())
                throw std::runtime_error("keys are still being played - try again");
              const samp::LocalPed self = samp::ReadLocalPed();
              if (!self.valid)
                throw std::runtime_error("the local player is not readable");
              const int key = game::KeyForAction(game::kVehicleEnterExit, VK_RETURN);
              samp::KeysPress(key);
              return json{{"pressed", game::KeyName(key)},
                          {"note", "read ready or get_world in a second or two - "
                                   "getting in takes an animation"}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "answer_dialog",
      "Answers the dialog the server is showing, the way a player answers it: "
      "arrow keys to move a list selection, characters into an input, then "
      "Enter for the first button or Escape for the second. Almost everything "
      "a role-play server offers is behind one of these, so this is how the "
      "character uses a menu, a bank, a job or a phone. Read it with "
      "get_dialog first; the rows of a list are the lines of its text, counted "
      "from zero. The server's own login dialog is answered by 'login' "
      "instead, which is where the password lives.",
      {{"type", "object"},
       {"properties",
        {{"item",
          {{"type", "integer"},
           {"minimum", 0},
           {"description", "Row of a list dialog to choose, counted from zero."}}},
         {"text",
          {{"type", "string"},
           {"description", "What to type into an input dialog."}}},
         {"button",
          {{"type", "integer"},
           {"enum", json::array({1, 2})},
           {"description", "1 presses the first button (Enter), 2 the second "
                           "(Escape). Defaults to 1."}}}}}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              if (samp::KeysBusy())
                throw std::runtime_error("keys are still being played - try again");
              const samp::Dialog dialog = samp::CurrentDialog();
              if (!dialog.valid)
                throw std::runtime_error("the client's dialog could not be read");
              if (!dialog.shown)
                throw std::runtime_error("no dialog is on screen");
              json did = json::array();

              if (args.contains("item") && args["item"].is_number_integer()) {
                const int item = args["item"].get<int>();
                // The rows are the lines of the text. Where the selection
                // sits now is the client's business, so it is walked to the
                // top first and counted down from there.
                int rows = 1;
                for (char c : dialog.text) if (c == 0x0A) ++rows;
                if (item < 0 || item >= rows)
                  throw std::runtime_error("that row is not in the list, which has " +
                                           std::to_string(rows));
                samp::KeysPress(VK_UP, rows);
                if (item > 0) samp::KeysPress(VK_DOWN, item);
                did.push_back("chose row " + std::to_string(item) + " of " +
                              std::to_string(rows));
              }
              if (args.contains("text") && args["text"].is_string()) {
                samp::KeysType(args["text"].get<std::string>());
                did.push_back("typed the text");
              }
              const int button = args.value("button", 1);
              samp::KeysPress(button == 2 ? VK_ESCAPE : VK_RETURN);
              did.push_back(button == 2 ? "pressed the second button"
                                        : "pressed the first button");

              return json{{"answered", did},
                          {"dialog", json{{"id", dialog.id},
                                          {"caption", dialog.caption},
                                          {"style", samp::DialogStyleName(dialog.style)}}},
                          {"note", "the keys are played one a frame; read "
                                   "get_dialog or events to see what came of it"}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "login",
      "Answers the server's password dialog with the password the player "
      "keeps in bot.login next to the module, typed the way a hand would. "
      "Only a dialog the server marked as a password input is answered, and "
      "only once in a session. The password is never returned, logged or "
      "sent anywhere; without bot.login this does nothing and says so.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            []() -> json {
              return json{{"note", samp::LoginNow()},
                          {"sent", samp::LoginSent()}};
            },
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
              act::DriveStop("stopped on request");
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
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const std::string text = args.value("text", std::string{});
              if (text.empty()) throw std::runtime_error("text must not be empty");
              if (samp::KeysBusy())
                throw std::runtime_error("keys are still being played - try again");
              const samp::Dialog dialog = samp::CurrentDialog();
              if (dialog.shown)
                throw std::runtime_error(
                    "a dialog is on screen and takes the keys - answer it first");
              // The way a player says something: the chat key, the words, and
              // Enter. Nothing is written into the client.
              samp::KeysPress('T');
              samp::KeysType(text);
              samp::KeysPress(VK_RETURN);
              return json{{"sent", text},
                          {"note", "typed into the chat and entered; read the "
                                   "chat or events for the server's answer"}};
            },
            kFastTimeoutMs);
      },
  });
}

}  // namespace gtabot::mcp
