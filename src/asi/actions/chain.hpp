#pragma once
//
// A chain of actions, run by the module a step at a time.
//
// A brain that thinks in language cannot decide once a second: a look, a
// thought and an answer is seconds of work however short the thought. But the
// character has to act every second, and something has to be watching the
// world in between.
//
// So the brain does not drive; it hands over a chain - walk here, press this,
// say that, answer the dialog - and the module walks it, checking the world
// every tick. The chain stops by itself the moment something happens that the
// brain ought to know about: a dialog on screen, somebody using his name,
// blood on him, or simply running out of steps. Then it says which of those
// it was, and the brain thinks again about a world that has changed rather
// than about the one it last looked at.
//
// The division is the point. Reflexes belong here, where they are cheap and
// immediate; judgement belongs to whoever can weigh a roleplay server's
// etiquette against a locked door. Neither can do the other's job.
//
#include <string>
#include <vector>

namespace gtabot::act {

struct Step {
  // go: walk to (x, y), done on arrival within stop_within
  // press: hold a key for a moment - "walk", "enter_exit", "jump", "y"
  // say: type a line into the chat
  // answer: answer the dialog on screen
  // wait: do nothing for ms
  std::string kind;
  float x = 0, y = 0, stop_within = 2.0f;
  std::string key;
  std::string text;          // say, and the text of an input dialog
  std::string choose;        // answer: the row that says this
  int  item = -1;            // answer: the row by number
  int  button = 1;           // answer: which button
  int  ms = 0;               // wait, and how long to hold a key
};

// What makes the chain stop early and hand back.
struct StopWhen {
  bool on_dialog = true;     // a dialog appeared that no step asked for
  bool on_spoken_to = true;  // somebody used this character's name
  bool on_hurt = true;       // health dropped
  float hurt_by = 5.0f;
};

struct ChainStatus {
  bool        running = false;
  std::size_t at = 0;        // steps finished
  std::size_t steps = 0;
  std::string doing;         // the step under way, in words
  std::string stopped_by;    // done, dialog, spoken_to, hurt, blocked, cancelled
  std::string note;
  long long   ran_ms = 0;
};

// Replaces whatever chain is running.
void RunChain(std::vector<Step> steps, const StopWhen& when);
void StopChain(const char* why);

// Game thread, every frame.
void ChainTick();

ChainStatus ChainGet();

}  // namespace gtabot::act
