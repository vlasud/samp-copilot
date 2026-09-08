#include "samp/talk.hpp"

#include <algorithm>
#include <cctype>

namespace gtabot::samp {
namespace {

bool HasAt(const std::string& text, std::size_t at, const char* what) {
  const std::size_t n = std::char_traits<char>::length(what);
  return text.size() >= at + n && text.compare(at, n, what) == 0;
}

bool Contains(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

// A roleplay name: Word_Word, letters and an underscore, no spaces.
bool LooksLikeAName(const std::string& text) {
  if (text.size() < 3 || text.size() > 48) return false;
  bool underscore = false;
  for (const char c : text) {
    if (c == '_') { underscore = true; continue; }
    if (std::isalpha(static_cast<unsigned char>(c)) ||
        static_cast<unsigned char>(c) >= 0x80)
      continue;
    return false;
  }
  return underscore;
}

// The "(Имя_Фамилия)" a say line ends with, if it ends with one.
std::string NameInBracketsAtTheEnd(const std::string& plain) {
  if (plain.empty() || plain.back() != ')') return {};
  const std::size_t open = plain.rfind('(');
  if (open == std::string::npos) return {};
  const std::string inside = plain.substr(open + 1, plain.size() - open - 2);
  return LooksLikeAName(inside) ? inside : std::string{};
}

// "Имя_Фамилия[42] крикнул: ..." - the name before the bracketed id.
std::string NameBeforeAnId(const std::string& plain) {
  const std::size_t open = plain.find('[');
  if (open == std::string::npos || open == 0) return {};
  const std::size_t close = plain.find(']', open);
  if (close == std::string::npos) return {};
  for (std::size_t i = open + 1; i < close; ++i)
    if (!std::isdigit(static_cast<unsigned char>(plain[i]))) return {};
  std::size_t start = plain.rfind(' ', open);
  start = start == std::string::npos ? 0 : start + 1;
  const std::string name = plain.substr(start, open - start);
  return LooksLikeAName(name) ? name : std::string{};
}

}  // namespace

std::string WithoutColours(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    // {RRGGBB} and nothing else: six hexadecimal digits between braces.
    if (text[i] == '{' && i + 7 < text.size() && text[i + 7] == '}') {
      bool hex = true;
      for (std::size_t d = i + 1; d < i + 7; ++d)
        if (!std::isxdigit(static_cast<unsigned char>(text[d]))) { hex = false; break; }
      if (hex) { i += 7; continue; }
    }
    out += text[i];
  }
  return out;
}

TalkLine Classify(const std::string& text, const std::string& from,
                  const std::string& me) {
  TalkLine line;
  line.plain = WithoutColours(text);
  const std::string& plain = line.plain;

  // Trim the leading spaces a server indents its own notices with.
  std::size_t start = plain.find_first_not_of(" \t");
  if (start == std::string::npos) start = plain.size();

  if (HasAt(plain, start, "((")) {
    line.kind = "ooc";
  } else if (Contains(plain, "Отправил ") && Contains(plain, "тел.")) {
    line.kind = "advert";
  } else if (HasAt(plain, start, "Администратор ") ||
             HasAt(plain, start, "Гос. новости") ||
             HasAt(plain, start, "Объявление проверил")) {
    line.kind = HasAt(plain, start, "Администратор ") ? "admin" : "news";
  } else if (Contains(plain, " крикнул")) {
    line.kind = "shout";
    line.speaker = NameBeforeAnId(plain);
  } else {
    const std::string said_by = NameInBracketsAtTheEnd(plain);
    if (!said_by.empty()) {
      line.kind = "say";
      line.speaker = said_by;
    } else if (!from.empty()) {
      line.kind = "say";
      line.speaker = from;
    } else {
      // "Имя_Фамилия улыбается", "Имя_Фамилия показал свой паспорт": an
      // action begins with a name and has no colon after it.
      const std::size_t space = plain.find(' ', start);
      const std::string first =
          space == std::string::npos ? std::string{}
                                     : plain.substr(start, space - start);
      if (LooksLikeAName(first)) {
        line.kind = "action";
        line.speaker = first;
      } else {
        line.kind = "server";
      }
    }
  }

  if (!me.empty()) {
    line.from_me = line.speaker == me;
    // Named in the text by somebody who is not this character. The server
    // says "Вы" rather than the name when it means him, so a line carrying
    // the name is one where somebody else brought him up.
    line.to_me = !line.from_me && Contains(plain, me.c_str());
  }
  return line;
}

}  // namespace gtabot::samp
