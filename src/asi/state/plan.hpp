#pragma once
//
// What the brain says it is doing, on screen.
//
// The module is the hands: it walks, presses, reads. Watching it from outside
// tells you where the character went and nothing about why, which makes a
// perfectly sensible plan look like a man wandering a hospital at random.
//
// So the brain says. One line of what it is trying to do, and the steps it
// means to take, posted whenever the plan changes and drawn in the panel with
// the current step marked. Nothing here decides anything or checks anything -
// it is a caption, and it is worth exactly as much as the brain's honesty
// about its own intentions.
//
#include <string>
#include <vector>

namespace gtabot::state {

struct Plan {
  std::string summary;              // what he is trying to do, in a line
  std::vector<std::string> steps;   // and how, in order
  int         doing = -1;           // which step is under way, -1 for none
  long long   posted_ms = 0;        // when it was last said
  long long   age_ms = 0;
};

void SetPlan(const std::string& summary, std::vector<std::string> steps,
             int doing);
Plan GetPlan();

}  // namespace gtabot::state
