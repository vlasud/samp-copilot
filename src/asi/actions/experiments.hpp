#pragma once
//
// Two experiments, run from the F11 menu, that tell the two halves of what
// the walker does apart for SA-MP's protection.
//
// A run with the bot loaded and never armed was not punished; a walk is,
// within seconds to minutes. A walk is two things at once: keys pressed
// through the system, and calls into the game's world functions from the
// frame hook. One of these is what the protection sees. Each experiment
// does one of them alone for half a minute and writes what happened - the
// keyboard taken, or not - so the next build knows which half to rework.
//
namespace gtabot::act {

// Forward and sprint held through the system for this long. No call into
// the game is made; the character runs straight ahead, so stand facing
// somewhere open.
void StartKeyRun(unsigned seconds);

// About thirty world queries a frame around the character - the walker's
// own probe, without the walk - for this long. No key is pressed.
void StartWorldCalls(unsigned seconds);

void StopExperiments();
bool ExperimentRunning();

// For the menu: what is running and how long is left, or empty.
const char* ExperimentLine();

// Game thread, every frame.
void ExperimentTick();

}  // namespace gtabot::act
