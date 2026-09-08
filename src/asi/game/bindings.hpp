#pragma once
//
// Which key the player has bound to each thing the character can do.
//
// The game keeps a table of actions, each with a primary key and an
// alternative, in CControllerConfigManager. Everything this module presses
// is read out of it rather than assumed: somebody who walks on the arrows
// and gets into cars on Enter should be driven by their own keys, not by
// WASD and F.
//
#include <string>
#include <vector>

namespace gtabot::game {

// The action numbers this module has any use for, in the game's own order.
enum Action {
  kGoForward = 4,
  kGoBack = 5,
  kGoLeft = 6,
  kGoRight = 7,
  kVehicleEnterExit = 10,
  kJumping = 12,
  kSprint = 13,
  kVehicleSteerLeft = 20,
  kVehicleSteerRight = 21,
  kVehicleAccelerate = 24,
  kVehicleBrake = 25,
  kVehicleHorn = 29,
  kVehicleHandbrake = 31,
  // The one servers listen for as KEY_WALK, and the one nearly every Russian
  // roleplay server tells the player to press to use whatever he is standing
  // in front of. Left Alt on a default setup.
  kPedWalk = 17,
  kPedDuck = 15,
  kPedAnswerPhone = 16,
};

// One row of the game's controller table.
struct Binding {
  int         action = 0;
  int         primary_vk = 0;
  int         alternative_vk = 0;
  std::string primary;
  std::string alternative;
};

// Every action the table holds a key for, in the game's own order. Game
// thread. What this is for: a server's prompt says "press Alt", and which
// key that actually is on this installation is a question with an answer
// rather than a guess.
std::vector<Binding> AllBindings();

// The virtual key behind a name a person would use: "alt", "lalt", "enter",
// "space", "tab", "f4", "y", "2", "vk1B". Zero when the name means nothing.
int KeyFromName(const std::string& name);

// The virtual key for an action, or `fallback` when the table cannot be read
// or holds something with no virtual key of its own. Game thread.
int KeyForAction(int action, int fallback);

// "W", "Space", "LShift", "vk1B" - for saying in the log which keys are being
// pressed without a table of magic numbers.
std::string KeyName(int virtual_key);

}  // namespace gtabot::game
