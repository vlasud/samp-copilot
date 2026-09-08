#include "state/people.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace gtabot::people {
namespace {

// How often the world is looked at for this. A person's standing does not
// change in a tenth of a second, and reading the pool does cost something.
constexpr unsigned long long kEveryMs = 1000;
// Near enough to matter, and near enough to have done it.
constexpr float kNear  = 30.0f;
constexpr float kClose = 15.0f;
// A drop worth explaining. Small ones are falls, fists and the server's own
// arithmetic.
constexpr float kHurtBy = 5.0f;

std::mutex g_mutex;
std::map<std::string, Person> g_people;
unsigned long long g_looked_ms = 0;
float g_last_health = -1.0f;
std::string g_note = "nobody seen yet";

float Distance2D(float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  return std::sqrt(dx * dx + dy * dy);
}

// What the record adds up to, when nobody has said otherwise.
void Judge(Person* who) {
  if (who->set_by_hand) return;
  if (who->near_when_hurt >= 2) {
    who->standing = "enemy";
    who->why = "armed and close on " + std::to_string(who->near_when_hurt) +
               " occasions when this character lost health - which the client "
               "cannot prove, only notice";
    return;
  }
  if (who->seen_armed_near >= 3) {
    who->standing = "wary";
    who->why = "seen carrying a weapon nearby " +
               std::to_string(who->seen_armed_near) + " times";
    return;
  }
  if (who->spoke_to_me >= 2 && who->seen_armed_near == 0) {
    who->standing = "friend";
    who->why = "has spoken to this character by name " +
               std::to_string(who->spoke_to_me) +
               " times and has never been seen armed";
    return;
  }
  who->standing = "neutral";
  who->why = who->times_seen > 0
                 ? "seen " + std::to_string(who->times_seen) +
                       " times, nothing for or against"
                 : "known only from the chat";
}

Person& Find(const std::string& name, unsigned long long now) {
  auto found = g_people.find(name);
  if (found == g_people.end()) {
    Person fresh;
    fresh.name = name;
    fresh.first_seen_ms = static_cast<long long>(now);
    fresh.standing = "neutral";
    fresh.why = "not seen yet";
    found = g_people.emplace(name, std::move(fresh)).first;
  }
  return found->second;
}

}  // namespace

void SawWorld(const json& world) {
  const unsigned long long now = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (now - g_looked_ms < kEveryMs) return;
    g_looked_ms = now;
  }
  if (!world.is_object() || !world.contains("self")) return;
  const json& self = world["self"];
  if (!self.contains("pos") || self["pos"].size() < 3) return;
  const float sx = self["pos"][0].get<float>();
  const float sy = self["pos"][1].get<float>();
  const float health = self.value("health", -1.0f);

  std::lock_guard<std::mutex> lock(g_mutex);
  // Did this character just lose blood? Then whoever was armed and close is
  // worth remembering - not as the culprit, as a coincidence that repeated.
  const bool hurt = g_last_health >= 0 && health >= 0 &&
                    g_last_health - health >= kHurtBy;
  if (health >= 0) g_last_health = health;

  for (auto& entry : g_people) entry.second.streamed = false;

  int near_count = 0;
  const json players = world.value("players", json::array());
  for (const json& player : players) {
    if (player.value("npc", false)) continue;
    const std::string name = player.value("name", std::string{});
    if (name.empty()) continue;
    Person& who = Find(name, now);
    who.last_id = player.value("id", -1);
    who.last_seen_ms = static_cast<long long>(now);
    if (!player.value("streamed", false) || !player.contains("pos") ||
        player["pos"].size() < 3) {
      Judge(&who);
      continue;
    }
    who.streamed = true;
    const float away = Distance2D(sx, sy, player["pos"][0].get<float>(),
                                  player["pos"][1].get<float>());
    who.last_away_m = away;
    if (who.closest_m == 0 || away < who.closest_m) who.closest_m = away;
    if (away <= kNear) {
      ++who.times_seen;
      ++near_count;
      const int weapon = player.value("weapon", 0);
      if (weapon > 0) ++who.seen_armed_near;
      if (hurt && weapon > 0 && away <= kClose) ++who.near_when_hurt;
    }
    Judge(&who);
  }
  g_note = std::to_string(g_people.size()) + " people on record, " +
           std::to_string(near_count) + " of them within " +
           std::to_string(static_cast<int>(kNear)) + " metres";
}

void HeardLine(const std::string& speaker, bool to_me) {
  if (speaker.empty()) return;
  const unsigned long long now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_mutex);
  Person& who = Find(speaker, now);
  ++who.spoke_near;
  if (to_me) ++who.spoke_to_me;
  Judge(&who);
}

void SetStanding(const std::string& name, const std::string& standing,
                 const std::string& why) {
  const unsigned long long now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_mutex);
  Person& who = Find(name, now);
  if (standing.empty()) {
    who.set_by_hand = false;
    Judge(&who);
    return;
  }
  who.set_by_hand = true;
  who.standing = standing;
  who.why = why.empty() ? "set by hand" : why;
}

std::vector<Person> Everyone() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Person> out;
  out.reserve(g_people.size());
  for (const auto& entry : g_people) out.push_back(entry.second);
  std::sort(out.begin(), out.end(), [](const Person& a, const Person& b) {
    return a.last_seen_ms > b.last_seen_ms;
  });
  return out;
}

std::string Note() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_note;
}

}  // namespace gtabot::people
