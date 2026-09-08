// The classifier, against lines taken off the real server.
//
// Every string below was read out of the chat during a session on Advance
// RolePlay and pasted here unchanged, colour codes and all. That is the whole
// point: the shapes these rules key off are the shapes the server actually
// writes, and a rule that only works on an invented example is a rule that
// has not been checked.
#include "check.hpp"

#include "samp/talk.hpp"

using gtabot::samp::Classify;
using gtabot::samp::TalkLine;
using gtabot::samp::WithoutColours;

namespace {

const char* kMe = "Lo_Vlasuddd";

void Kind(const char* text, const char* want, const char* speaker = "") {
  const TalkLine said = Classify(text, "", kMe);
  check::Is(said.kind, want, text);
  if (speaker[0] != '\0') check::Is(said.speaker, speaker, text);
}

}  // namespace

void TestTalk() {
  std::printf("talk\n");

  // The colours a server writes into everything.
  check::Is(WithoutColours("Медперсонал не может отпустить Вас в таком "
                           "{ff9966}состоянии{FFFFFF}."),
            "Медперсонал не может отпустить Вас в таком состоянии.",
            "colour codes come out");
  check::Is(WithoutColours("nothing {to} strip {12345} {GGGGGG}"),
            "nothing {to} strip {12345} {GGGGGG}",
            "only six hex digits count as a colour");

  // A paid advert. It carries a name and reads like speech, and treating it
  // as somebody talking is the mistake this exists to stop.
  Kind("LV | В казино г. Los Santos играют до 1.000.000$. Мы в GPS 8 - 22 | "
       "Отправил Denis_Degtyarev[180] (тел. 66)",
       "advert");
  Kind("LV | Продам дом эконом класса в ш.San-Fierro. Цена: 42.000.000$ | "
       "Отправил Rey_Relito[282] (тел. 787878)",
       "advert");

  // The press desk and the administration.
  Kind("  Объявление проверил сотрудник СМИ Cristiano_Ronaldonenko", "news");
  Kind("Администратор Heath_Bush поставил затычку игроку Conor_Nurmagomedov "
       "на 30 мин. Причина: mat",
       "admin");

  // Somebody speaking, with the name in brackets the way the server puts it.
  Kind("- мы в пложой компании {FFFFFF}(Rim_Uolker)", "say", "Rim_Uolker");
  Kind("- Можешь вылечить? {FF6666}(Storm_Buda)", "say", "Storm_Buda");

  // Shouting, which puts the name and the id in front instead.
  Kind("Dmitriy_Pussenko[183] крикнул: я тебя не знаю сукааа", "shout",
       "Dmitriy_Pussenko");

  // An action, which is a name and a verb and no colon.
  Kind("Nika_Hardon показал свой паспорт", "action", "Nika_Hardon");
  Kind("Nika_Hardon смеётся", "action", "Nika_Hardon");

  // Out of character.
  Kind("(( Barbara_Cassini[386]: :( ))", "ooc");

  // The server talking to this character. "Вы" is not a name, and the line
  // must not be filed as somebody doing something.
  Kind("Вы заняли койку. В зависимости от состояния здоровья лечение может "
       "занять время",
       "server");
  Kind("Медперсонал не может отпустить Вас в таком {ff9966}состоянии{FFFFFF}. "
       "Отправляйтесь на {00e6ac}лечение",
       "server");

  // Addressed to this character, and by him.
  const TalkLine at_me =
      Classify("- Lo_Vlasuddd, ты живой? {FFFFFF}(Storm_Buda)", "", kMe);
  check::True(at_me.to_me, "a line using the name is addressed to him");
  check::True(!at_me.from_me, "and it is not from him");
  const TalkLine by_me =
      Classify("- да, живой {FFFFFF}(Lo_Vlasuddd)", "", kMe);
  check::True(by_me.from_me, "his own line is from him");
  check::True(!by_me.to_me, "and is not addressed to him");

  // No name to compare against: nothing is claimed either way.
  const TalkLine nameless =
      Classify("- Lo_Vlasuddd, ты живой? {FFFFFF}(Storm_Buda)", "", "");
  check::True(!nameless.to_me, "without a name, nothing is addressed to him");
}
