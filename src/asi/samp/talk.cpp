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

// Lower case, in the two alphabets a Russian server writes its menus in.
//
// Latin is a byte. Cyrillic is two, in UTF-8: А-П are D0 90..D0 9F and lower
// to D0 B0..D0 BF, Р-Я are D0 A0..D0 AF and lower across the lead byte to
// D1 80..D1 8F, and Ё is D0 81 to ё's D1 91. Folding only the Latin half is
// what made a step naming "Мин. здравоохранения" miss a row that shouted it.
std::string Folded(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte >= 'A' && byte <= 'Z') {
      out += static_cast<char>(byte - 'A' + 'a');
      continue;
    }
    if (byte == 0xD0 && i + 1 < text.size()) {
      const auto next = static_cast<unsigned char>(text[i + 1]);
      if (next >= 0x90 && next <= 0x9F) {
        out += static_cast<char>(0xD0);
        out += static_cast<char>(next + 0x20);
        ++i;
        continue;
      }
      if (next >= 0xA0 && next <= 0xAF) {
        out += static_cast<char>(0xD1);
        out += static_cast<char>(next - 0x20);
        ++i;
        continue;
      }
      if (next == 0x81) {          // Ё
        out += static_cast<char>(0xD1);
        out += static_cast<char>(0x91);
        ++i;
        continue;
      }
    }
    out += text[i];
  }
  return out;
}

bool Mentions(const std::string& line, const std::string& want) {
  if (want.empty()) return false;
  return Folded(line).find(Folded(want)) != std::string::npos;
}

std::vector<std::string> Rows(const std::string& dialog_text) {
  std::vector<std::string> rows;
  std::string row;
  for (const char c : dialog_text) {
    if (c == 0x0A) { rows.push_back(WithoutColours(row)); row.clear(); }
    else row += c;
  }
  rows.push_back(WithoutColours(row));
  return rows;
}

int RowSaying(const std::vector<std::string>& rows, const std::string& want) {
  int found = -1;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!Mentions(rows[i], want)) continue;
    if (found >= 0) return -2;
    found = static_cast<int>(i);
  }
  return found;
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
