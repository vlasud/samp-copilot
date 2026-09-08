#pragma once
//
// Telling one kind of chat line from another.
//
// The chat is one column of text carrying half a dozen different things: the
// server's own notices, adverts other players paid to broadcast, an
// administrator addressing everybody, somebody speaking two metres away, and
// - rarely, and the only one that needs an answer - somebody speaking to
// this character. An agent reading the raw column has to work that out again
// on every line, and gets it wrong on the adverts, which look like speech
// and are not.
//
// A roleplay server writes each of them in its own shape. Speech ends with
// the speaker's name in brackets, an advert carries "Отправил <name>" and a
// telephone number, an administrator's line begins with the word. None of
// that is guessed at from the server's source: it is the shape the lines
// actually have, and a line that fits none of them is reported as unknown
// rather than forced into a kind.
//
#include <string>
#include <vector>

#include "types.hpp"

namespace gtabot::samp {

struct TalkLine {
  std::string kind;      // server, advert, admin, say, shout, action, ooc, unknown
  std::string speaker;   // the character who said it, when the line names one
  std::string plain;     // the text with the colour codes taken out
  bool        to_me   = false;
  bool        from_me = false;
};

// `from` is the chat log's own speaker column, which is empty for most lines
// on a roleplay server. `me` is this character's name; pass it empty and
// nothing is marked as addressed to him.
TalkLine Classify(const std::string& text, const std::string& from,
                  const std::string& me);

// The colour codes a server writes into its text, taken out: "{FF0000}" and
// the like. What is left is what a person reads on the screen.
std::string WithoutColours(const std::string& text);

}  // namespace gtabot::samp
