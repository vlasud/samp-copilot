#include "state/events.hpp"

#include <windows.h>

#include <deque>
#include <mutex>
#include <string>

#include "actions/travel.hpp"
#include "samp/chat.hpp"
#include "samp/dialog.hpp"
#include "samp/dialog_path.hpp"
#include "samp/input_state.hpp"
#include "samp/login.hpp"
#include "samp/talk.hpp"
#include "samp/world.hpp"
#include "state/people.hpp"

namespace gtabot::state {
namespace {

// Two memories, not one. A busy server says a hundred things a minute and
// would push a death, a dialog or the end of a journey out of a single ring
// long before anybody read them.
constexpr std::size_t kKeep = 400;
constexpr std::size_t kKeepChat = 600;
constexpr unsigned long long kEveryMs = 200;
constexpr int kChatLines = 12;

struct Event {
  long long   seq = 0;
  long long   at_ms = 0;
  std::string kind;
  std::string text;
  json        extra;
};

std::mutex g_mutex;
std::deque<Event> g_events;
std::deque<Event> g_chat;
long long g_next_seq = 1;
long long g_dropped = 0;

// What the last look saw, so that only changes are noted.
bool g_had_dialog = false;
int  g_dialog_id = -1;
std::string g_dialog_caption;
int  g_active = -1;
int  g_wasted = -1;
bool g_travelling = false;
bool g_login_sent = false;
std::string g_last_chat_line;
bool g_chat_started = false;
std::string g_my_name;
unsigned long long g_looked_ms = 0;

std::string ChatSignature(const json& line) {
  return line.value("from", std::string{}) + "\x01" + line.value("text", std::string{});
}

}  // namespace

void Note(const std::string& kind, const std::string& text, json extra) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Event event;
  event.seq   = g_next_seq++;
  event.at_ms = static_cast<long long>(GetTickCount64());
  event.kind  = kind;
  event.text  = text;
  event.extra = std::move(extra);
  const bool is_chat = event.kind == "chat";
  std::deque<Event>& into = is_chat ? g_chat : g_events;
  const std::size_t keep = is_chat ? kKeepChat : kKeep;
  into.push_back(std::move(event));
  while (into.size() > keep) {
    into.pop_front();
    ++g_dropped;
  }
}

void WatchEvents() {
  const unsigned long long now = GetTickCount64();
  if (now - g_looked_ms < kEveryMs) return;
  g_looked_ms = now;

  // A menu being walked, if one is.
  samp::DialogPathTick();

  // The dialog on screen.
  const samp::Dialog dialog = samp::CurrentDialog();
  if (dialog.valid) {
    const bool changed = dialog.shown != g_had_dialog || dialog.id != g_dialog_id ||
                         dialog.caption != g_dialog_caption;
    if (changed) {
      if (dialog.shown)
        Note("dialog", "the server is showing a dialog",
             json{{"id", dialog.id},
                  {"style", samp::DialogStyleName(dialog.style)},
                  {"caption", dialog.caption}});
      else if (g_had_dialog)
        Note("dialog", "the dialog has gone", json{{"id", g_dialog_id}});
      g_had_dialog = dialog.shown;
      g_dialog_id = dialog.id;
      g_dialog_caption = dialog.caption;
    }
  }

  // Being alive, and being here at all.
  const samp::InputSwitch state = samp::ReadInputSwitch();
  if (state.player_known) {
    if (g_active >= 0 && state.active != g_active)
      Note(state.active != 0 ? "spawn" : "unspawn",
           state.active != 0 ? "the character has spawned"
                             : "the character is no longer in the world");
    if (g_wasted >= 0 && state.wasted != g_wasted && state.wasted != 0)
      Note("death", "the character has died");
    g_active = state.active;
    g_wasted = state.wasted;
  }

  // The journey.
  const act::TravelStatus trip = act::TravelGet();
  if (trip.travelling != g_travelling) {
    g_travelling = trip.travelling;
    Note("travel", trip.travelling ? "a journey has started" : trip.note,
         json{{"straight_m", trip.straight_m}});
  }

  if (!g_login_sent && samp::LoginSent()) {
    g_login_sent = true;
    Note("login", "the password has been given to the server");
  }

  // Everyone in sight, for the record of who is who. Throttled inside.
  {
    const json world = samp::ReadWorld();
    if (world.is_object()) {
      if (world.contains("self"))
        g_my_name = world["self"].value("name", g_my_name);
      people::SawWorld(world);
    }
  }

  // The chat, from wherever it was left.
  const json chat = samp::ReadChat(kChatLines);
  const json lines = chat.value("lines", json::array());
  if (!lines.empty()) {
    std::size_t from = 0;
    if (g_chat_started) {
      from = lines.size();   // nothing new unless the mark is found
      for (std::size_t i = 0; i < lines.size(); ++i)
        if (ChatSignature(lines[i]) == g_last_chat_line) {
          from = i + 1;
          break;
        }
      // The mark has scrolled away: take the last few rather than all.
      if (from == lines.size() && ChatSignature(lines.back()) != g_last_chat_line)
        from = lines.size() > 4 ? lines.size() - 4 : 0;
    } else {
      g_chat_started = true;
      from = lines.size();   // the backlog on the first look is not news
    }
    for (std::size_t i = from; i < lines.size(); ++i) {
      const std::string who = lines[i].value("from", std::string{});
      const std::string what = lines[i].value("text", std::string{});
      Note("chat", who.empty() ? what : who + ": " + what);
      // And on the record of who is who: a name in the text is somebody
      // addressing this character, which is the one signal a stranger gives
      // for free.
      const samp::TalkLine said = samp::Classify(what, who, g_my_name);
      if (!said.speaker.empty() && !said.from_me)
        people::HeardLine(said.speaker, said.to_me);
    }
    g_last_chat_line = ChatSignature(lines.back());
  }
}

json EventsSince(long long since, int limit, const std::vector<std::string>& kinds) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto wanted = [&kinds](const std::string& kind) {
    if (kinds.empty()) return true;
    for (const std::string& k : kinds)
      if (k == kind) return true;
    return false;
  };

  // The two memories walked together, oldest first, so the order is the one
  // things happened in whichever ring they went into.
  json out = json::array();
  std::size_t a = 0, b = 0;
  long long next = since;
  long long newest = 0;
  if (!g_events.empty()) newest = g_events.back().seq;
  if (!g_chat.empty() && g_chat.back().seq > newest) newest = g_chat.back().seq;

  while (static_cast<int>(out.size()) < limit) {
    const Event* event = nullptr;
    if (a < g_events.size() && (b >= g_chat.size() ||
                                g_events[a].seq <= g_chat[b].seq))
      event = &g_events[a++];
    else if (b < g_chat.size())
      event = &g_chat[b++];
    else
      break;
    if (event->seq <= since) continue;
    next = event->seq;
    if (!wanted(event->kind)) continue;
    json one{{"seq", event->seq},
             {"at_ms", event->at_ms},
             {"kind", event->kind},
             {"text", event->text}};
    if (!event->extra.empty()) one["about"] = event->extra;
    out.push_back(std::move(one));
  }
  // Nothing matched, but everything up to here has been looked at.
  if (a >= g_events.size() && b >= g_chat.size() && newest > next) next = newest;
  return json{{"events", std::move(out)},
              {"next", next},
              {"more", next < newest},
              {"dropped", g_dropped}};
}

}  // namespace gtabot::state
