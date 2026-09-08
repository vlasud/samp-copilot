// The record of who is who.
//
// What is checked here is the part that has to be right for the wrong reason
// to be visible: that an enemy is only called one after the coincidence has
// repeated, that a standing set by hand wins, and that handing the judgement
// back restores what the record says.
#include "check.hpp"

#include <windows.h>

#include "state/people.hpp"
#include "types.hpp"

using gtabot::json;
namespace people = gtabot::people;

namespace {

const people::Person* Find(const std::vector<people::Person>& all,
                           const char* name) {
  for (const people::Person& one : all)
    if (one.name == name) return &one;
  return nullptr;
}

json World(float my_health, const char* name, float away, int weapon) {
  json world;
  world["self"] = json{{"pos", {0.0f, 0.0f, 10.0f}}, {"health", my_health}};
  json player{{"id", 7},
              {"name", name},
              {"npc", false},
              {"streamed", true},
              {"pos", {away, 0.0f, 10.0f}}};
  if (weapon > 0) player["weapon"] = weapon;
  world["players"] = json::array({player});
  return world;
}

// The module looks at the world once a second and ignores what comes in
// between, which is what makes it cheap to call every tick.
void Wait() { Sleep(1100); }

}  // namespace

void TestPeople() {
  std::printf("people\n");

  // Somebody who talks to this character and never carries anything.
  people::HeardLine("Storm_Buda", true);
  people::HeardLine("Storm_Buda", true);
  const auto after_talking = people::Everyone();
  const people::Person* friendly = Find(after_talking, "Storm_Buda");
  check::True(friendly != nullptr, "the talker is on record");
  if (friendly) {
    check::Is(friendly->standing, "friend", "twice by name and never armed");
    check::Is(friendly->spoke_to_me, 2, "both lines counted");
  }

  // A standing set by hand wins, and giving it back restores the judgement.
  people::SetStanding("Storm_Buda", "enemy", "he said so himself");
  const auto after_setting = people::Everyone();
  const people::Person* by_hand = Find(after_setting, "Storm_Buda");
  if (by_hand) {
    check::Is(by_hand->standing, "enemy", "set by hand wins");
    check::True(by_hand->set_by_hand, "and says it was set by hand");
    check::Is(by_hand->why, "he said so himself", "with the reason given");
  }
  people::SetStanding("Storm_Buda", "", "");
  const auto after_giving_back = people::Everyone();
  const people::Person* given_back = Find(after_giving_back, "Storm_Buda");
  if (given_back) {
    check::Is(given_back->standing, "friend", "handing it back re-judges");
    check::True(!given_back->set_by_hand, "and it is no longer by hand");
  }

  // An armed stranger standing close while this character bleeds. Once is a
  // coincidence and must not be enough.
  people::SawWorld(World(100.0f, "Vlad_Saveliev", 5.0f, 24));
  Wait();
  people::SawWorld(World(80.0f, "Vlad_Saveliev", 5.0f, 24));
  const auto after_one = people::Everyone();
  const people::Person* once = Find(after_one, "Vlad_Saveliev");
  check::True(once != nullptr, "the armed one is on record");
  if (once) {
    check::Is(once->near_when_hurt, 1, "one coincidence counted");
    check::True(once->standing != "enemy", "one is not enough to be an enemy");
  }

  // Twice is the rule.
  Wait();
  people::SawWorld(World(60.0f, "Vlad_Saveliev", 5.0f, 24));
  const auto after_two = people::Everyone();
  const people::Person* twice = Find(after_two, "Vlad_Saveliev");
  if (twice) {
    check::Is(twice->near_when_hurt, 2, "two coincidences counted");
    check::Is(twice->standing, "enemy", "two makes an enemy");
    check::True(twice->why.find("cannot prove") != std::string::npos,
                "and the reason says it is not proof");
  }

  // Somebody far away when it happened is not implicated.
  Wait();
  people::SawWorld(World(60.0f, "Passer_By", 90.0f, 24));
  // `far` and `near` are still macros in the Windows headers.
  const auto after_passer = people::Everyone();
  const people::Person* distant = Find(after_passer, "Passer_By");
  if (distant) {
    check::Is(distant->near_when_hurt, 0, "too far to be counted");
    check::Is(distant->seen_armed_near, 0, "and too far to count as armed nearby");
  }
}
