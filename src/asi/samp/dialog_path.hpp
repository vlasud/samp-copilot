#pragma once
//
// Walking a server's menus, several steps at a time.
//
// Everything a roleplay server offers sits behind a chain of dialogs: /menu,
// then the list of command groups, then the group. Answering one of those
// takes a keypress and then a wait, because the next dialog does not exist
// until the server has heard the answer and sent the next one - which is a
// round trip, over the internet, in the middle of a frame.
//
// So a caller answering them one at a time is really writing a loop of "press
// something, poll until the dialog changes, press the next thing", and
// getting the waiting wrong is how a step lands in the wrong menu. This does
// that loop once, here, on the game thread, and reports where it got to.
//
// The steps are named, not numbered. A server renumbers its menus between
// updates and orders them differently for a player of a different rank, so a
// path written as row indices is a path that silently chooses the wrong thing
// later. A step that names nothing on screen stops the walk and says what was
// on screen instead.
//
#include <string>
#include <vector>

namespace gtabot::samp {

struct PathStatus {
  bool        walking = false;
  std::size_t step = 0;        // how many steps have been answered
  std::size_t steps = 0;
  std::string note;
  // What was on screen when a step found nothing, so the caller can see what
  // the server actually offered.
  std::vector<std::string> rows;
};

// Starts a walk, replacing any walk already running. Each step is text that
// one row of the dialog on screen must say.
void WalkDialogs(std::vector<std::string> steps);
void StopWalkingDialogs(const char* why);

// Game thread, a few times a second.
void DialogPathTick();

PathStatus DialogPathGet();

}  // namespace gtabot::samp
