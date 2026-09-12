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
#include "actions/chain.hpp"
#include "actions/driver.hpp"
#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "game/bindings.hpp"
#include "game/blips.hpp"
#include "game/collision.hpp"
#include "nav/field.hpp"
#include "game/paths.hpp"
#include "game/peds.hpp"
#include "game/streaming.hpp"
#include "game/world_query.hpp"
#include "nav/indoors.hpp"
#include "nav/planner.hpp"
#include "nav/trail.hpp"
#include "samp/bubbles.hpp"
#include "samp/chat.hpp"
#include "samp/checkpoints.hpp"
#include "samp/dialog.hpp"
#include "samp/dialog_path.hpp"
#include "samp/input_state.hpp"
#include "samp/keys.hpp"
#include "samp/labels.hpp"
#include "samp/objects.hpp"
#include "samp/reconnect.hpp"
#include "samp/textdraws.hpp"
#include "samp/login.hpp"
#include "samp/world.hpp"
#include "samp/discovery.hpp"
#include "samp/talk.hpp"
#include "samp/version.hpp"
#include "state/events.hpp"
#include "state/memory.hpp"
#include "state/people.hpp"
#include "state/plan.hpp"
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
  // Who has the pad, when it is not the walk. Read this before concluding
  // anything from a character who did not move.
  if (!walk.held_by.empty()) out["held_by"] = walk.held_by;
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

// Whether a row says what was asked for: a case-insensitive containment,
// which is what naming a menu row amounts to when the server writes it with
// a number, a colour and a trailing space.
bool Mentions(const std::string& row, const std::string& want) {
  if (want.empty()) return false;
  const auto fold = [](std::string text) {
    for (char& c : text)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
  };
  return fold(row).find(fold(want)) != std::string::npos;
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
      "timestamps or is assuming it. Every line also carries 'kind' - say, "
      "shout, action, advert, admin, news, ooc, server - the speaker where "
      "the line names one, 'plain' with the colour codes taken out, and "
      "'to_me' when somebody else used this character's name. Adverts look "
      "like speech and are not, which is what the kinds are for.",
      {{"type", "object"},
       {"properties",
        {{"limit",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 200},
           {"description", "How many of the most recent lines to return. "
                           "Defaults to 40."}}},
         {"only_to_me",
          {{"type", "boolean"},
           {"description", "Only the lines where somebody used this "
                           "character's name - what actually wants an "
                           "answer."}}}}}},
      [](const json& args) {
        const int limit = args.value("limit", 40);
        const bool only_to_me = args.value("only_to_me", false);
        return Rpc::RunOnGameThread(
            [limit, only_to_me]() -> json {
              // Read more than was asked for when only some of it will be
              // kept, or "the last ten lines addressed to me" turns into
              // "whichever of the last ten lines were".
              json out = samp::ReadChat(only_to_me ? 200 : limit);
              const samp::LocalPed self = samp::ReadLocalPed();
              std::string me;
              const json world = asi::Bridge::GetWorld(nullptr);
              if (world.contains("self") && world["self"].contains("name"))
                me = world["self"].value("name", std::string{});
              json kept = json::array();
              for (json& line : out["lines"]) {
                const samp::TalkLine said =
                    samp::Classify(line.value("text", std::string{}),
                                   line.value("from", std::string{}), me);
                line["kind"] = said.kind;
                line["plain"] = said.plain;
                if (!said.speaker.empty()) line["speaker"] = said.speaker;
                if (said.to_me) line["to_me"] = true;
                if (said.from_me) line["from_me"] = true;
                if (!only_to_me || said.to_me) kept.push_back(line);
              }
              if (only_to_me && kept.size() > static_cast<std::size_t>(limit))
                kept.erase(kept.begin(),
                           kept.end() - static_cast<std::ptrdiff_t>(limit));
              if (only_to_me) out["lines"] = std::move(kept);
              if (!me.empty()) out["me"] = me;
              (void)self;
              return out;
            },
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
              // The red cylinder a server puts on the ground to say "go
              // here". It is in none of the pools; SA-MP keeps one of each
              // in its own CGame, because a player is only ever shown one.
              const samp::Checkpoint mark = samp::CheckpointNow(here);
              if (mark.shown)
                out["checkpoint"] = json{{"at", json{{"x", mark.at.x},
                                                     {"y", mark.at.y},
                                                     {"z", mark.at.z}}},
                                         {"size", mark.size},
                                         {"away_m", mark.away_m}};
              const samp::RaceCheckpoint race = samp::RaceCheckpointNow(here);
              if (race.shown)
                out["race_checkpoint"] =
                    json{{"at", json{{"x", race.at.x},
                                     {"y", race.at.y},
                                     {"z", race.at.z}}},
                         {"next", json{{"x", race.next.x},
                                       {"y", race.next.y},
                                       {"z", race.next.z}}},
                         {"size", race.size},
                         {"type", race.type},
                         {"away_m", race.away_m}};

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
      "follow_dialog",
      "Walks a chain of the server's menus in one call: each step is text one "
      "row of the dialog then on screen must say. Answering a menu one step "
      "at a time means pressing, then polling until the server has sent the "
      "next dialog - a round trip each time - and getting that wait wrong is "
      "how a step lands in the wrong menu. Steps are named rather than "
      "numbered because a server renumbers its menus. Optionally opens the "
      "chain first by typing a command. Returns at once; poll "
      "dialog_path_status, which lists what was on screen if a step found "
      "nothing.",
      {{"type", "object"},
       {"properties",
        {{"path",
          {{"type", "array"},
           {"items", {{"type", "string"}}},
           {"description", "What each step's row says, in order."}}},
         {"open_with",
          {{"type", "string"},
           {"description", "A chat line to send first, e.g. \"/menu\"."}}}}},
       {"required", json::array({"path"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              std::vector<std::string> steps;
              for (const json& step : args["path"])
                if (step.is_string()) steps.push_back(step.get<std::string>());
              if (steps.empty())
                throw std::runtime_error("the path is empty");
              const std::string open = args.value("open_with", std::string{});
              if (!open.empty()) {
                samp::KeysPress('T');   // the same key send_chat uses
                samp::KeysType(open);
                samp::KeysPress(VK_RETURN);
              }
              samp::WalkDialogs(steps);
              return json{{"walking", true},
                          {"steps", steps.size()},
                          {"opened_with", open},
                          {"note", "poll dialog_path_status"}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "dialog_path_status",
      "How the menu walk is going: how many steps have been answered, and "
      "why it stopped. When a step named something that was not there, the "
      "rows that were there are listed.",
      NoArguments(),
      [](const json&) -> json {
        const samp::PathStatus status = samp::DialogPathGet();
        json out{{"walking", status.walking},
                 {"step", status.step},
                 {"steps", status.steps},
                 {"note", status.note}};
        if (!status.rows.empty()) out["rows_on_screen"] = status.rows;
        return out;
      },
  });

  server->AddTool({
      "probe_heights",
      "Diagnostic. From where the character stands, a one-metre horizontal "
      "line in each of four directions at a ladder of heights above the "
      "floor, and whether each is clear. Says at what height the world "
      "starts to be seen - which is how the room map's own lines are chosen, "
      "rather than by guessing where a server's floor slab ends.",
      {{"type", "object"},
       {"properties",
        {{"length",
          {{"type", "number"}, {"minimum", 0.05}, {"maximum", 5.0},
           {"description", "How long each line is. Defaults to one metre."}}}}}},
      [](const json& args) {
        const float length = args.value("length", 1.0f);
        return Rpc::RunOnGameThread(
            [length]() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              if (!self.valid)
                throw std::runtime_error("the local player is not readable");
              json out{{"z", self.z}, {"length", length}};
              float with = 0, without = 0;
              const bool got_with = game::GroundBelow(
                  game::Vec3{self.x, self.y, self.z + 1.2f}, &with, true);
              const bool got_without = game::GroundBelow(
                  game::Vec3{self.x, self.y, self.z + 1.2f}, &without, false);
              if (got_with) out["floor_with_objects"] = with;
              if (got_without) out["floor_without_objects"] = without;
              const float base = got_with ? with : self.z - 1.0f;
              out["base"] = base;
              const float heights[] = {0.1f, 0.2f, 0.35f, 0.5f, 0.7f, 0.9f,
                                       1.1f, 1.35f, 1.6f, 2.0f, 2.4f};
              const float dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
              json ladder = json::array();
              for (const float h : heights) {
                int clear = 0;
                for (const float* d : dirs) {
                  const game::Vec3 a{self.x, self.y, base + h};
                  const game::Vec3 b{self.x + d[0] * length,
                                     self.y + d[1] * length, base + h};
                  if (game::LineClear(a, b, false)) ++clear;
                }
                ladder.push_back(json{{"above_floor", h}, {"clear_of_4", clear}});
              }
              out["ladder"] = ladder;
              return out;
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "get_trail",
      "What he knows because he has walked it: the squares he has physically "
      "stood in and the steps between them, which is the one map of an "
      "interior that cannot be wrong. Routing indoors prefers it over "
      "anything worked out from the geometry, and it is written to bot.trail "
      "so a building is learned once.",
      NoArguments(),
      [](const json&) -> json {
        const nav::TrailFacts facts = nav::TrailGet();
        return json{{"squares", facts.squares},
                    {"steps", facts.steps},
                    {"routes_found", facts.routes_found},
                    {"routes_missed", facts.routes_missed},
                    {"loaded", facts.loaded},
                    {"note", facts.note}};
      },
  });

  server->AddTool({
      "act",
      "Hands the module a chain of actions and lets it walk them, a step at a "
      "time, checking the world every tick. This is how to drive the "
      "character: a brain that thinks in language cannot decide once a "
      "second, and the character has to act every second. The chain stops by "
      "itself the moment something happens worth knowing about - a dialog "
      "appeared, somebody used his name, he lost blood, a step got stuck - "
      "and says which it was, so the next thought is about a world that has "
      "changed. Returns at once; poll act_status. "
      "Steps: {go:{x,y,stop_within}} walk somewhere; {press:\"walk\"} hold a "
      "key (walk is the interaction key); {say:\"...\"} type a line of chat; "
      "{answer:{choose|item,text,button}} answer the dialog on screen; "
      "{wait:1500} do nothing for that many milliseconds.",
      {{"type", "object"},
       {"properties",
        {{"steps",
          {{"type", "array"}, {"items", {{"type", "object"}}},
           {"description", "The chain, in order."}}},
         {"stop_on_dialog", {{"type", "boolean"}}},
         {"stop_on_spoken_to", {{"type", "boolean"}}},
         {"stop_on_hurt", {{"type", "boolean"}}}}},
       {"required", json::array({"steps"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              std::vector<act::Step> steps;
              for (const json& one : args["steps"]) {
                if (!one.is_object()) continue;
                act::Step step;
                if (one.contains("go")) {
                  const json& go = one["go"];
                  step.kind = "go";
                  step.x = go.value("x", 0.0f);
                  step.y = go.value("y", 0.0f);
                  step.stop_within = go.value("stop_within", 2.0f);
                } else if (one.contains("press")) {
                  step.kind = "press";
                  step.key = one["press"].is_string()
                                 ? one["press"].get<std::string>()
                                 : std::string{"walk"};
                  step.ms = one.value("ms", 0);
                } else if (one.contains("say")) {
                  step.kind = "say";
                  step.text = one["say"].get<std::string>();
                } else if (one.contains("answer")) {
                  const json& answer = one["answer"];
                  step.kind = "answer";
                  if (answer.is_object()) {
                    step.item = answer.value("item", -1);
                    step.choose = answer.value("choose", std::string{});
                    step.text = answer.value("text", std::string{});
                    step.button = answer.value("button", 1);
                  }
                } else if (one.contains("wait")) {
                  step.kind = "wait";
                  step.ms = one["wait"].get<int>();
                } else {
                  continue;
                }
                steps.push_back(std::move(step));
              }
              if (steps.empty()) throw std::runtime_error("no steps");
              act::StopWhen when;
              when.on_dialog = args.value("stop_on_dialog", true);
              when.on_spoken_to = args.value("stop_on_spoken_to", true);
              when.on_hurt = args.value("stop_on_hurt", true);
              act::RunChain(std::move(steps), when);
              const act::ChainStatus status = act::ChainGet();
              return json{{"running", status.running},
                          {"steps", status.steps},
                          {"note", "poll act_status"}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "act_status",
      "How the chain is going: how many steps are done, what is under way, "
      "and - when it has stopped - which of the things worth knowing about "
      "stopped it: done, dialog, spoken_to, hurt, blocked, cancelled. "
      "'typing' is true while letters are still being played into the game: "
      "a line of chat is not said the moment it is asked for, and deciding "
      "again before it is finished is how the same sentence gets said twice.",
      NoArguments(),
      [](const json&) -> json {
        const act::ChainStatus status = act::ChainGet();
        return json{{"running", status.running},
                    {"at", status.at},
                    {"steps", status.steps},
                    {"doing", status.doing},
                    {"stopped_by", status.stopped_by},
                    {"note", status.note},
                    {"ran_ms", status.ran_ms},
                    {"typing", samp::KeysBusy()}};
      },
  });

  server->AddTool({
      "act_stop",
      "Drops whatever chain is running.",
      NoArguments(),
      [](const json&) -> json {
        act::StopChain("asked to stop");
        return json{{"running", false}};
      },
  });

  server->AddTool({
      "set_plan",
      "Says what you are trying to do and how, so it appears on screen. The "
      "module is the hands; watching it from outside shows where the "
      "character went and nothing about why, which makes a sensible plan "
      "look like a man wandering about at random. Post one line of intent "
      "and the steps you mean to take, and post it again whenever the plan "
      "changes or you move on to the next step.",
      {{"type", "object"},
       {"properties",
        {{"summary",
          {{"type", "string"},
           {"description", "What he is trying to do, in a line."}}},
         {"steps",
          {{"type", "array"}, {"items", {{"type", "string"}}},
           {"description", "The steps, in order. At most eight are shown."}}},
         {"doing",
          {{"type", "integer"},
           {"description", "Which step is under way, counted from zero; "
                           "-1 for none."}}}}},
       {"required", json::array({"summary"})}},
      [](const json& args) -> json {
        std::vector<std::string> steps;
        if (args.contains("steps") && args["steps"].is_array())
          for (const json& step : args["steps"])
            if (step.is_string()) steps.push_back(step.get<std::string>());
        const int doing = args.value("doing", -1);
        state::SetPlan(args.value("summary", std::string{}), steps, doing);
        return json{{"shown", true},
                    {"summary", args.value("summary", std::string{})},
                    {"steps", steps.size()},
                    {"doing", doing}};
      },
  });

  server->AddTool({
      "get_plan",
      "What was last posted with set_plan, and how long ago.",
      NoArguments(),
      [](const json&) -> json {
        const state::Plan plan = state::GetPlan();
        return json{{"summary", plan.summary},
                    {"steps", plan.steps},
                    {"doing", plan.doing},
                    {"age_ms", plan.age_ms},
                    {"thought_ms", plan.thought_ms},
                    {"looked_ago_ms", plan.looked_ago_ms}};
      },
  });

  server->AddTool({
      "look",
      "Everything worth knowing about where he is, in one answer: himself, "
      "the dialog on screen, what has just been said to him, the people and "
      "the things around, and what he is in the middle of doing. This is the "
      "call to make on a loop - one snapshot a second is plenty - so that "
      "deciding what to do next is done against a whole picture rather than "
      "a dozen separate questions. Nothing here is interpreted: what a skin "
      "or a label means belongs in the server's own notes.",
      {{"type", "object"},
       {"properties",
        {{"radius",
          {{"type", "number"}, {"minimum", 2}, {"maximum", 200},
           {"description", "How far to look. Defaults to forty metres."}}},
         {"chat",
          {{"type", "integer"}, {"minimum", 0}, {"maximum", 40},
           {"description", "How many recent chat lines. Defaults to eight."}}}}}},
      [](const json& args) {
        // The clock starts when the brain asks, not when the game thread gets
        // round to answering: what is being measured is the brain's thinking,
        // and a stalled frame is not that.
        state::NotedLook();
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const float radius = args.value("radius", 40.0f);
              const int lines = args.value("chat", 8);
              json out;

              const samp::LocalPed self = samp::ReadLocalPed();
              std::int64_t age_ms = -1;
              const json world = asi::Bridge::GetWorld(&age_ms);
              out["self"] = world.value("self", json::object());
              out["connection"] = world.value("connection", "unknown");

              // Always present, and always says whether one is up. A field
              // that appears only when there is a dialog makes its absence
              // the signal, and absence is indistinguishable from a reader
              // that failed - which is how the character came to stand in
              // front of an open dialog pressing keys the game never saw.
              const samp::Dialog dialog = samp::CurrentDialog();
              json on_screen{{"shown", dialog.valid && dialog.shown},
                             {"readable", dialog.valid}};
              if (dialog.valid && dialog.shown) {
                on_screen["id"] = dialog.id;
                on_screen["style"] = samp::DialogStyleName(dialog.style);
                on_screen["style_number"] = dialog.style;
                on_screen["caption"] = dialog.caption;
                on_screen["text"] = dialog.text;
                // The rows of a list, ready to be named back in an answer.
                if (dialog.style == 2 || dialog.style == 4 || dialog.style == 5) {
                  json rows = json::array();
                  for (const std::string& row : samp::Rows(dialog.text))
                    rows.push_back(row);
                  on_screen["rows"] = std::move(rows);
                }
                on_screen["blocks_everything"] = true;
              }
              out["dialog"] = std::move(on_screen);

              // What has been said, already sorted into kinds, with the lines
              // that used his name marked.
              json chat = samp::ReadChat(lines);
              std::string me;
              if (world.contains("self"))
                me = world["self"].value("name", std::string{});
              json said = json::array();
              for (json& line : chat["lines"]) {
                const samp::TalkLine talk =
                    samp::Classify(line.value("text", std::string{}),
                                   line.value("from", std::string{}), me);
                json one{{"kind", talk.kind}, {"text", talk.plain}};
                if (!talk.speaker.empty()) one["speaker"] = talk.speaker;
                if (talk.to_me) one["to_me"] = true;
                said.push_back(std::move(one));
              }
              out["chat"] = std::move(said);

              if (!self.valid) {
                out["note"] = "the local player is not readable yet";
                return out;
              }
              const game::Vec3 here{self.x, self.y, self.z};

              // Other players, by name, and never mixed in with the NPCs.
              //
              // They used to be dropped here entirely - the loop below skipped
              // every ped the pool called a player - so a brain looking at the
              // world saw shopkeepers and medics and not one living soul. It
              // could neither speak to anybody nor keep away from anybody,
              // which is half of what it is for.
              json players = json::array();
              {
                std::int64_t age_ms = -1;
                const json world = asi::Bridge::GetWorld(&age_ms);
                for (const json& one : world.value("players", json::array())) {
                  if (!one.value("streamed", false)) continue;
                  const auto at = one.value("pos", std::vector<float>{});
                  if (at.size() != 3) continue;
                  const float dx = at[0] - here.x, dy = at[1] - here.y;
                  const float away = std::sqrt(dx * dx + dy * dy);
                  if (away > radius) continue;
                  const std::string name = one.value("name", std::string());
                  json who{{"name", name},
                           {"id", one.value("id", -1)},
                           {"away_m", away},
                           {"at", json{{"x", at[0]}, {"y", at[1]}, {"z", at[2]}}},
                           {"in_vehicle", one.value("in_vehicle", false)}};
                  // What is thought of him, kept beside him rather than in a
                  // list somebody has to go and ask for. A record nobody
                  // reads at the moment of meeting is a record for nothing.
                  for (const people::Person& known : people::Everyone()) {
                    if (known.name != name) continue;
                    if (!known.standing.empty() && known.standing != "neutral") {
                      who["standing"] = known.standing;
                      if (!known.why.empty()) who["why"] = known.why;
                    }
                    if (known.spoke_to_me > 0) who["has_spoken_to_me"] = true;
                    break;
                  }
                  if (one.contains("health")) who["health"] = one["health"];
                  if (one.contains("weapon_name"))
                    who["weapon_name"] = one["weapon_name"];
                  players.push_back(std::move(who));
                }
              }
              out["players"] = std::move(players);

              json npcs = json::array();
              for (const game::Ped& who : game::PedsNear(here, radius, 12, self.game_ped)) {
                if (who.is_player) continue;
                const game::Vec3 stand = game::InFrontOf(who, 1.2f);
                npcs.push_back(json{{"skin", who.skin},
                                    {"away_m", who.away_m},
                                    {"heading_deg", who.heading * 57.2957795f},
                                    {"at", json{{"x", who.position.x},
                                                {"y", who.position.y},
                                                {"z", who.position.z}}},
                                    {"stand_at", json{{"x", stand.x}, {"y", stand.y}}}});
              }
              out["npcs"] = std::move(npcs);

              json labels = json::array();
              for (const samp::Label& one : samp::LabelsNear(here, radius, 20))
                labels.push_back(json{{"text", one.text},
                                      {"away_m", one.away_m},
                                      {"at", json{{"x", one.at.x},
                                                  {"y", one.at.y},
                                                  {"z", one.at.z}}}});
              out["labels"] = std::move(labels);

              json pickups = json::array();
              for (const samp::Pickup& one : samp::PickupsNear(here, radius, 16))
                pickups.push_back(json{{"model", one.model},
                                       {"type", one.type},
                                       {"away_m", one.away_m},
                                       {"at", json{{"x", one.at.x},
                                                   {"y", one.at.y},
                                                   {"z", one.at.z}}}});
              out["pickups"] = std::move(pickups);

              json doors = json::array();
              for (const samp::NearObject& one : samp::DoorsNear(here, radius, 10))
                doors.push_back(json{{"model", one.model},
                                     {"away_m", one.away_m},
                                     {"at", json{{"x", one.at.x},
                                                 {"y", one.at.y},
                                                 {"z", one.at.z}}}});
              out["doors"] = std::move(doors);

              out["travel"] = TravelStatusJson();
              const nav::TrailFacts trail = nav::TrailGet();
              out["learned"] = json{{"squares", trail.squares},
                                    {"steps", trail.steps}};
              out["movement_armed"] = game::Enabled();
              return out;
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "find_text",
      "Diagnostic. Says where in the client's memory a piece of text lives. "
      "For finding what nothing reads yet: put something on screen - a chat "
      "bubble over somebody's head, a label, a message - and search for a few "
      "words of it. The answer gives the address, which module it belongs to "
      "and the bytes on either side, and the same offset from the same base "
      "twice running stops being a guess.",
      {{"type", "object"},
       {"properties",
        {{"text",
          {{"type", "string"},
           {"description", "At least three characters that are on screen now."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 40},
           {"description", "At most this many places. Defaults to ten."}}}}},
       {"required", json::array({"text"})}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const std::string text = args.value("text", std::string{});
              const std::size_t limit = args.value("limit", 10);
              json out = json::array();
              for (const samp::Found& one : samp::FindText(text, limit)) {
                char at[24];
                std::snprintf(at, sizeof(at), "0x%08X",
                              static_cast<unsigned>(one.at));
                out.push_back(json{{"at", at},
                                   {"where", one.where},
                                   {"encoding", one.encoding},
                                   {"around", one.around}});
              }
              return json{{"found", std::move(out)},
                          {"note", samp::FindTextNote()}};
            },
            kSlowTimeoutMs);
      },
  });

  server->AddTool({
      "local_picture",
      "What the walker sees round the character this instant: the local "
      "picture it steers on, painted from the world's collision - a "
      "quarter-metre cell each character, y upward. S is him, # something "
      "solid from the waist up (or a ledge, or no ground), ~ something low "
      "enough to hop, . open ground. Diagnostic.",
      {{"type", "object"},
       {"properties",
        {{"reach", {{"type", "number"}, {"minimum", 1}, {"maximum", 6},
                    {"description", "Metres each way; six at most."}}}}}},
      [](const json& args) -> json {
        const float reach = args.value("reach", 4.0f);
        return Rpc::RunOnGameThread(
            [reach]() -> json {
              return json{{"picture", act::LocalPictureText(reach)}};
            },
            5000);
      },
  });

  server->AddTool({
      "plan_field",
      "Plans a route over a walkability field painted from the world's own "
      "collision - the same way the room is mapped indoors, taken outside. "
      "Every cell knows how far the nearest wall is and walking near one "
      "costs more, so the route keeps to the middle of the pavement by "
      "itself. Diagnostic: returns the route, the numbers and a picture. "
      "Game thread; bounded by deadline_ms.",
      {{"type", "object"},
       {"properties",
        {{"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}},
         {"deadline_ms", {{"type", "integer"}, {"minimum", 200}, {"maximum", 20000}}},
         {"picture", {{"type", "boolean"}}},
         {"probe", {{"type", "array"},
                    {"description", "Points to classify on the finished field: "
                                    "[{x,y}, ...]. A blocked one is explained "
                                    "in the log - which entity is under it."}}}}},
       {"required", json::array({"x", "y"})}},
      [](const json& args) -> json {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              const game::Vec3 from{self.x, self.y, self.z};
              float ground = self.z - 1.0f;
              game::GroundBelow(game::Vec3{args["x"], args["y"], self.z + 20.0f}, &ground);
              const game::Vec3 to{args["x"], args["y"], ground + 1.0f};
              nav::Field field;
              field.Start(from, to);
              const unsigned long long until =
                  GetTickCount64() + args.value("deadline_ms", 8000);
              while (!field.Step())
                if (GetTickCount64() > until) break;
              const nav::FieldResult r = field.result();
              json probes = json::array();
              if (args.contains("probe") && args["probe"].is_array()) {
                for (const json& pt : args["probe"]) {
                  if (!pt.is_object()) continue;
                  const game::Vec3 at{pt.value("x", 0.0f), pt.value("y", 0.0f), self.z};
                  nav::Field::CellInfo info;
                  json one{{"x", at.x}, {"y", at.y}};
                  if (field.At(at, &info)) {
                    one["passable"] = info.passable;
                    one["blocked"] = info.blocked;
                    one["known"] = info.known;
                    one["ground"] = info.ground;
                    one["clear"] = info.clear;
                    if (info.blocked) {
                      float gz = 0;
                      game::col::GroundBelow(at.x, at.y, self.z + 3.0f, &gz, false);
                      game::col::Explain(at.x, at.y, gz + 1.2f);
                      one["explained_in_log"] = true;
                    }
                  } else {
                    one["outside"] = true;
                  }
                  probes.push_back(one);
                }
              }
              json points = json::array();
              for (const game::Vec3& p : r.points)
                points.push_back(json{{"x", p.x}, {"y", p.y}, {"z", p.z}});
              json out{{"ok", r.ok}, {"reaches_target", r.reaches_target},
                       {"short_by_m", r.short_by_m}, {"length_m", r.length_m},
                       {"legs", static_cast<int>(r.points.size()) - 1},
                       {"points", points}, {"note", r.note},
                       {"cells", r.cells}, {"blocked", r.blocked},
                       {"tiles", r.tiles}, {"ground_reads", r.ground_reads},
                       {"expanded", r.expanded}, {"took_ms", r.took_ms}};
              if (args.value("picture", false)) out["picture"] = r.picture;
              out["ref_z"] = r.ref_z;
              out["tile_floors"] = r.tile_floors;
              out["ground_line"] = r.ground_line;
              out["probes"] = probes;
              out["box"] = json{{"x0", r.box_x0}, {"y0", r.box_y0}, {"x1", r.box_x1}, {"y1", r.box_y1}};
              out["from"] = json{{"x", from.x}, {"y", from.y}, {"z", from.z}};
              out["to"] = json{{"x", to.x}, {"y", to.y}, {"z", to.z}};
              out["end_neighbours"] = r.end_neighbours;
              out["refused"] = json{{"shut", r.refused_shut}, {"step", r.refused_step},
                                    {"corner", r.refused_corner},
                                    {"tallest_step", r.tallest_step}};
              return out;
            },
            25000);
      },
  });

  server->AddTool({
      "ground_at",
      "Whether the ground can be read at a point, and at what height. The "
      "one question the planner asks before it will plan anything, and the "
      "one that had no way of being asked from outside. Takes a list of "
      "points and probes each from `from_above` metres over the player's own "
      "height, so the answer is the same one the journey gets.",
      {{"type", "object"},
       {"properties",
        {{"points", {{"type", "array"},
                     {"description", "[{x, y}, ...]"}}},
         {"from_above", {{"type", "number"}}}}},
       {"required", json::array({"points"})}},
      [](const json& args) -> json {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              const float above = args.value("from_above", 80.0f);
              json rows = json::array();
              for (const json& one : args["points"]) {
                const float x = one.value("x", 0.0f), y = one.value("y", 0.0f);
                const float dx = x - self.x, dy = y - self.y;
                float ground = 0;
                const bool got = game::GroundBelow(
                    game::Vec3{x, y, self.z + above}, &ground);
                float from_sky = 0;
                const bool sky =
                    game::GroundBelow(game::Vec3{x, y, 1000.0f}, &from_sky);
                const int entities = game::col::LastLookEntities();
                const int primitives = game::col::LastLookPrimitives();
                rows.push_back(json{{"x", x}, {"y", y},
                                    {"entities_looked_at", entities},
                                    {"primitives_looked_at", primitives},
                                    {"away_m", std::sqrt(dx * dx + dy * dy)},
                                    {"read", got},
                                    {"ground", got ? ground : 0.0f},
                                    {"read_from_the_sky", sky},
                                    {"ground_from_the_sky", sky ? from_sky : 0.0f}});
              }
              return json{{"from", {{"x", self.x}, {"y", self.y}, {"z", self.z}}},
                          {"probed_from_above_m", above},
                          {"points", rows}};
            },
            30000);
      },
  });

  server->AddTool({
      "pin_world",
      "Asks the game to load the collision of the whole map and keep it "
      "there. The game streams collision only for the few hundred metres "
      "round the player: further out a building is in the world's lists but "
      "has no collision, a ray cast at it passes through, and the planner "
      "writes down 'no floor here'. Pinned, the planner can look as far as "
      "it likes. Costs one long load and a few tens of megabytes. Without "
      "arguments it pins everything; with a box it pins that box only.",
      {{"type", "object"},
       {"properties",
        {{"x0", {{"type", "number"}}}, {"y0", {{"type", "number"}}},
         {"x1", {{"type", "number"}}}, {"y1", {{"type", "number"}}}}}},
      [](const json& args) -> json {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              if (!game::streaming::Ready())
                return json{{"ok", false},
                            {"why", "this build of the game does not keep its "
                                    "streamer where we look for it"}};
              const unsigned long long began = GetTickCount64();
              const int before = game::streaming::Pinned();
              int asked = 0;
              if (args.contains("x0") && args.contains("y0") &&
                  args.contains("x1") && args.contains("y1"))
                asked = game::streaming::PinCollisionOver(args["x0"], args["y0"],
                                                          args["x1"], args["y1"]);
              else
                asked = game::streaming::PinWholeMap();
              const int nodes = game::streaming::PinPathNodes();
              int sections = 0;
              if (args.contains("x0"))
                sections = game::streaming::PinMapOver(args["x0"], args["y0"],
                                                       args["x1"], args["y1"]);
              else
                sections = game::streaming::PinMapOver(-4000, -4000, 4000, 4000);
              return json{{"ok", true},
                          {"asked", asked},
                          {"node_areas_asked", nodes},
                          {"map_sections_asked", sections},
                          {"map_sections_pinned", game::streaming::MapSectionsPinned()},
                          {"pinned_before", before},
                          {"pinned", game::streaming::Pinned()},
                          {"took_ms", static_cast<int>(GetTickCount64() - began)},
                          {"held_mb", game::streaming::MemoryUsed() / (1024 * 1024)},
                          {"allowed_mb", game::streaming::MemoryBudget() / (1024 * 1024)}};
            },
            120000);
      },
  });

  server->AddTool({
      "get_blips",
      "Every mark on the game's own radar, with its place. A server marks "
      "where it wants you to go with one of these - what /gps answers with is "
      "a blip, not a SA-MP checkpoint, which is why get_checkpoint reads "
      "empty while the radar plainly has a new mark on it. The player's own "
      "map waypoint is in here too. Nothing is interpreted: position, colour, "
      "whether it follows an entity, and the raw bytes of the trace go out as "
      "found.",
      NoArguments(),
      [](const json&) -> json {
        return Rpc::RunOnGameThread(
            []() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              const game::Vec3 here{self.x, self.y, self.z};
              const std::vector<game::Blip> blips = game::BlipsNow(here);
              json rows = json::array();
              for (const game::Blip& one : blips)
                rows.push_back(json{{"index", one.index},
                                    {"at", {{"x", one.at.x}, {"y", one.at.y},
                                            {"z", one.at.z}}},
                                    {"away_m", one.away_m},
                                    {"colour", one.colour},
                                    {"entity", one.entity},
                                    {"tracking", one.tracking},
                                    {"kind", one.kind},
                                    {"fresh", one.fresh},
                                    {"flags", one.flags},
                                    {"bytes", one.bytes}});
              json newest = nullptr;
              game::Blip last;
              if (game::AppearedLast(&last))
                newest = json{{"index", last.index},
                              {"at", {{"x", last.at.x}, {"y", last.at.y},
                                      {"z", last.at.z}}},
                              {"away_m", last.away_m}};
              return json{{"blips", rows}, {"count", rows.size()},
                          {"waypoint_index", game::WaypointIndex()},
                          {"appeared_last", newest}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "get_checkpoint",
      "The red cylinder the server is showing, if any, and the racing marker "
      "with the position of the one after it. A server marks where it wants "
      "somebody to go with a checkpoint rather than a pickup - the delivery "
      "point of a job, the next corner of a route, the spot to park - and "
      "they are in none of the pools, so nothing else reports them. Walk to "
      "'at' with travel_to.",
      NoArguments(),
      [](const json&) {
        return Rpc::RunOnGameThread(
            []() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              const game::Vec3 here = self.valid
                                          ? game::Vec3{self.x, self.y, self.z}
                                          : game::Vec3{};
              json out{{"note", samp::CheckpointsNote()}};
              const samp::Checkpoint mark = samp::CheckpointNow(here);
              out["checkpoint"] =
                  mark.shown ? json{{"at", json{{"x", mark.at.x},
                                                {"y", mark.at.y},
                                                {"z", mark.at.z}}},
                                    {"size", mark.size},
                                    {"away_m", mark.away_m}}
                             : json(nullptr);
              const samp::RaceCheckpoint race = samp::RaceCheckpointNow(here);
              out["race_checkpoint"] =
                  race.shown ? json{{"at", json{{"x", race.at.x},
                                                {"y", race.at.y},
                                                {"z", race.at.z}}},
                                    {"next", json{{"x", race.next.x},
                                                  {"y", race.next.y},
                                                  {"z", race.next.z}}},
                                    {"size", race.size},
                                    {"type", race.type},
                                    {"away_m", race.away_m}}
                             : json(nullptr);
              out["note"] = samp::CheckpointsNote();
              return out;
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "get_npcs",
      "The people the game itself is holding, nearest first, with the "
      "direction each is facing and the spot to stand on to be in front of "
      "his face. Shopkeepers, clerks and a hospital's duty doctor are not "
      "players and are in none of SA-MP's pools; they are peds the server "
      "made. A server checks that somebody is in front of its clerk before "
      "it will talk to him, so 'stand_at' is the point to walk to and "
      "'look_at' the point to face while doing it. Each one's 'skin' is "
      "reported and never interpreted: which skin is a medic or a clerk is "
      "written in the server's own notes.",
      {{"type", "object"},
       {"properties",
        {{"radius",
          {{"type", "number"}, {"minimum", 1}, {"maximum", 200},
           {"description", "How far to look. Defaults to thirty metres."}}},
         {"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 60},
           {"description", "At most this many. Defaults to ten."}}},
         {"stand_off",
          {{"type", "number"}, {"minimum", 0.4}, {"maximum", 3.0},
           {"description", "How far in front of the face to stand. Defaults "
                           "to one metre."}}},
         {"players_too",
          {{"type", "boolean"},
           {"description", "Include the peds that are players. Off by "
                           "default: those are in get_world already."}}},
         {"skins",
          {{"type", "array"}, {"items", {{"type", "integer"}}},
           {"description", "Only these skins. Which skin means what is a "
                           "fact about a server - its medics, its police - "
                           "and lives in the server's own notes, not here."}}}}}},
      [](const json& args) {
        return Rpc::RunOnGameThread(
            [args]() -> json {
              const samp::LocalPed self = samp::ReadLocalPed();
              if (!self.valid)
                throw std::runtime_error("the local player is not readable");
              const float radius = args.value("radius", 30.0f);
              const std::size_t limit = args.value("limit", 10);
              const float off = args.value("stand_off", 1.0f);
              const bool players_too = args.value("players_too", false);
              std::vector<int> wanted_skins;
              if (args.contains("skins") && args["skins"].is_array())
                for (const json& skin : args["skins"])
                  if (skin.is_number_integer()) wanted_skins.push_back(skin.get<int>());
              json out = json::array();
              for (const game::Ped& who :
                   game::PedsNear(game::Vec3{self.x, self.y, self.z}, radius,
                                  limit * 4, self.game_ped)) {
                if (!players_too && who.is_player) continue;
                if (out.size() >= limit) break;
                const game::Vec3 stand = game::InFrontOf(who, off);
                if (!wanted_skins.empty() &&
                    std::find(wanted_skins.begin(), wanted_skins.end(),
                              who.skin) == wanted_skins.end())
                  continue;
                out.push_back(json{
                    {"away_m", who.away_m},
                    {"is_player", who.is_player},
                    {"skin", who.skin},
                    {"heading_deg", who.heading * 57.2957795f},
                    {"at", json{{"x", who.position.x},
                                {"y", who.position.y},
                                {"z", who.position.z}}},
                    {"stand_at", json{{"x", stand.x}, {"y", stand.y}}},
                    {"look_at", json{{"x", who.position.x},
                                     {"y", who.position.y}}}});
              }
              return json{{"npcs", std::move(out)},
                          {"note", game::PedsNote()}};
            },
            kFastTimeoutMs);
      },
  });

  server->AddTool({
      "get_people",
      "Who is who: everyone this session has seen or heard, the most recently "
      "seen first, with a standing - friend, neutral, wary, enemy - and the "
      "reason for it. The reasons are what a client can honestly know: how "
      "near somebody has been, whether he was carrying anything, whether he "
      "has addressed this character by name, and whether he happened to be "
      "armed and close when this character lost health. That last one is a "
      "coincidence the module counts, not an accusation it can prove.",
      {{"type", "object"},
       {"properties",
        {{"limit",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 200},
           {"description", "At most this many. Defaults to twenty."}}},
         {"here_now",
          {{"type", "boolean"},
           {"description", "Only the ones in the world right now."}}},
         {"standing",
          {{"type", "string"},
           {"description", "Only this standing: friend, neutral, wary, "
                           "enemy."}}}}}},
      [](const json& args) -> json {
        const std::size_t limit = args.value("limit", 20);
        const bool here_now = args.value("here_now", false);
        const std::string want = args.value("standing", std::string{});
        json out = json::array();
        for (const people::Person& who : people::Everyone()) {
          if (out.size() >= limit) break;
          if (here_now && !who.streamed) continue;
          if (!want.empty() && who.standing != want) continue;
          json one{{"name", who.name},
                   {"standing", who.standing},
                   {"why", who.why},
                   {"set_by_hand", who.set_by_hand},
                   {"here_now", who.streamed},
                   {"times_seen", who.times_seen},
                   {"spoke_to_me", who.spoke_to_me},
                   {"seen_armed_near", who.seen_armed_near},
                   {"near_when_hurt", who.near_when_hurt}};
          if (who.last_id >= 0) one["id"] = who.last_id;
          if (who.streamed) one["away_m"] = who.last_away_m;
          if (who.closest_m > 0) one["closest_m"] = who.closest_m;
          out.push_back(std::move(one));
        }
        return json{{"people", std::move(out)}, {"note", people::Note()}};
      },
  });

  server->AddTool({
      "set_standing",
      "Decides what somebody is, overriding what the record adds up to. Pass "
      "an empty standing to hand the judgement back to the module.",
      {{"type", "object"},
       {"properties",
        {{"name", {{"type", "string"}}},
         {"standing",
          {{"type", "string"},
           {"enum", json::array({"friend", "neutral", "wary", "enemy", ""})}}},
         {"why", {{"type", "string"}}}}},
       {"required", json::array({"name"})}},
      [](const json& args) -> json {
        const std::string name = args.value("name", std::string{});
        if (name.empty()) throw std::runtime_error("a name is required");
        people::SetStanding(name, args.value("standing", std::string{}),
                            args.value("why", std::string{}));
        for (const people::Person& who : people::Everyone())
          if (who.name == name)
            return json{{"name", who.name},
                        {"standing", who.standing},
                        {"why", who.why}};
        return json{{"name", name}};
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
      "from zero, though 'choose' names a row by what it says instead. The server's own login dialog is answered by 'login' "
      "instead, which is where the password lives.",
      {{"type", "object"},
       {"properties",
        {{"item",
          {{"type", "integer"},
           {"minimum", 0},
           {"description", "Row of a list dialog to choose, counted from zero."}}},
         {"choose",
          {{"type", "string"},
           {"description", "The row that says this, instead of its number - "
                           "case-insensitive, matched on the row's text with "
                           "the colour codes removed. Prefer it: a server "
                           "renumbers its menus and orders them differently "
                           "for different players. Says which rows there are "
                           "when nothing matches."}}},
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

              // A row named rather than numbered. A server renumbers its
              // menus between updates and puts the rows in a different order
              // for a player of a different rank, so "the row that says
              // Мин. здравоохранения" survives what "row 11" does not.
              int item = -1;
              if (args.contains("choose") && args["choose"].is_string()) {
                const std::string want =
                    samp::WithoutColours(args["choose"].get<std::string>());
                std::vector<std::string> rows;
                std::string row;
                for (const char c : dialog.text) {
                  if (c == 0x0A) { rows.push_back(row); row.clear(); }
                  else row += c;
                }
                rows.push_back(row);
                std::vector<int> matches;
                for (std::size_t i = 0; i < rows.size(); ++i)
                  if (Mentions(samp::WithoutColours(rows[i]), want))
                    matches.push_back(static_cast<int>(i));
                if (matches.empty()) {
                  std::string listed;
                  for (std::size_t i = 0; i < rows.size() && i < 30; ++i)
                    listed += std::string("\n  ") + std::to_string(i) +
                              ": " + samp::WithoutColours(rows[i]);
                  throw std::runtime_error("no row says \"" + want +
                                           "\"; the rows are:" + listed);
                }
                if (matches.size() > 1)
                  throw std::runtime_error(
                      std::to_string(matches.size()) +
                      " rows say that - name it more exactly");
                item = matches.front();
                did.push_back("found \"" + want + "\" on row " +
                              std::to_string(item));
              }
              if (item >= 0 || (args.contains("item") &&
                                args["item"].is_number_integer())) {
                if (item < 0) item = args["item"].get<int>();
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
      "reconnect",
      "Rejoins the server without restarting the game - what a development "
      "loop needs after the server was restarted with a new gamemode build. "
      "Asks the client's own network layer to connect again to the address the "
      "launcher was given; the character is back in the world in half a second "
      "instead of the half minute GTA takes to load. Answers what it asked "
      "for, not whether the join finished: poll ready until spawned has been "
      "false and is true again, which is what says the character was spawned "
      "anew rather than carried over - measured at half a second. It works by "
      "handing the session to the client's own recovery for a server that "
      "restarted under it, so the whole way in is replayed and the gamemode "
      "loads the session as it would on any join. One case it cannot help "
      "with: once the client has given up on reaching a server it drops the "
      "object this goes through, and only starting the game again makes "
      "another.",
      {{"type", "object"},
       {"properties",
        {{"route",
          {{"type", "string"},
           {"enum", json::array({"restart", "calls"})},
           {"description",
            "How to get back in, and only worth passing to compare the two. "
            "\"restart\" hands the session to the client's own restart path, "
            "which replays the whole way in and spawns the character anew; "
            "\"calls\" only reconnects, which is just as quick and leaves the "
            "server with a player who joined and never spawned. Defaults to "
            "restart."}}}}}},
      [](const json& args) {
        const std::string route = args.value("route", std::string("restart"));
        samp::Route which = samp::Route::kRestart;
        if (route == "calls") which = samp::Route::kCalls;
        return Rpc::RunOnGameThread([which] { return samp::Reconnect(which); },
                                    kFastTimeoutMs);
      },
  });

  server->AddTool({
      "read_memory",
      "Reads a run of the client's memory by address and says what each word "
      "plausibly is - a pointer into samp.dll or gta_sa.exe, a heap pointer "
      "with the first word of what it points at, a small integer, a float. "
      "For settling a structure's layout when the shape search stopped short; "
      "nothing in the module reads the world through it.",
      {{"type", "object"},
       {"properties",
        {{"address",
          {{"type", "string"},
           {"description", "Where to start: hex (\"0x048B63A0\") or decimal."}}},
         {"words",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 256},
           {"description", "How many four-byte words. Defaults to 32."}}},
         {"stride",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 65536},
           {"description",
            "Bytes between the words read, for stepping along an array of "
            "records instead of reading it whole. Defaults to 4."}}},
         {"text",
          {{"type", "boolean"},
           {"description",
            "Also read each word's own bytes as text - how an array of names "
            "gives itself away. Off by default."}}}}},
       {"required", json::array({"address"})}},
      [](const json& args) {
        const std::string address = args.value("address", std::string{});
        const int words = args.value("words", 32);
        const int stride = args.value("stride", 4);
        const bool as_text = args.value("text", false);
        return Rpc::RunOnGameThread(
            [address, words, stride, as_text] {
              return samp::ReadWords(address, words, stride, as_text);
            },
            kFastTimeoutMs);
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
