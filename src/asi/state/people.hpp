#pragma once
//
// Who is who.
//
// A server is a few hundred strangers, and a character who treats all of them
// the same walks into the one who has been following him with a rifle. What
// can honestly be known about a stranger from inside the client is small and
// worth keeping: how often he has been near, how near he got, whether he was
// carrying anything, whether he has ever spoken to this character by name,
// and whether he happened to be armed and close at the moment this
// character's health went down.
//
// That last one is a guess and is labelled as one. The client is never told
// who shot it - a server sends a health value, not an attacker - so the most
// that can be said is that somebody was armed and standing close when it
// happened. Two of those and he is treated as an enemy; one is a coincidence.
//
// Nothing here decides anything on its own. It keeps the record and says what
// it thinks, and a standing set by hand always wins.
//
#include <string>
#include <vector>

#include "types.hpp"

namespace gtabot::people {

struct Person {
  std::string name;
  int         last_id = -1;
  int         times_seen = 0;
  long long   first_seen_ms = 0;
  long long   last_seen_ms  = 0;
  float       closest_m  = 0;      // the nearest he has ever been
  float       last_away_m = 0;
  int         spoke_to_me = 0;     // lines naming this character
  int         spoke_near  = 0;     // anything he said that was read
  int         seen_armed_near = 0;
  int         near_when_hurt  = 0;
  bool        streamed = false;    // in the world right now
  std::string standing;            // friend, neutral, wary, enemy
  bool        set_by_hand = false;
  std::string why;
};

// A world snapshot, as ReadWorld returns it. Throttled inside, so calling it
// every tick costs nothing. Game thread.
void SawWorld(const json& world);

// A line somebody said, already classified. `to_me` means it named this
// character.
void HeardLine(const std::string& speaker, bool to_me);

// A standing decided outside: by the person driving the bot, or by an agent
// that knows something this module cannot see. Passing an empty standing
// hands the judgement back to the module.
void SetStanding(const std::string& name, const std::string& standing,
                 const std::string& why);

// Everyone on record, the most recently seen first.
std::vector<Person> Everyone();

// How many are known, and how the last snapshot went.
std::string Note();

}  // namespace gtabot::people
