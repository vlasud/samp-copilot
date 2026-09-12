#include "samp/reconnect.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "log.hpp"
#include "samp/input_state.hpp"
#include "samp/version.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// Inside CNetGame. The game-state field is the one the rest of the module
// already reads; the RakClient pointer is the slot immediately before the
// pools pointer at +0x3CD, which is what fixed this end of the structure. The
// client's structures are packed, so neither sits on a four-byte boundary.
constexpr std::uint32_t kGameState     = 0x3BD;
constexpr std::uint32_t kRakClientView = 0x3C9;

// CNetGame::ShutdownForRestart, the client's own answer to a server that went
// away under it. It is what makes the difference between reconnecting and
// coming back properly: it destroys every remote player, takes the local
// player down, resets all nine pools, and puts the game state in "restarting"
// - so the way back in has to be replayed from the start, spawn included,
// which is the only thing that leaves the server with a player who spawned.
//
// The address is where the public 0.3.7-R1 declarations say it is, but that is
// only where to look: what accepts it is the code in this client. The checks
// below ask it to write 18 to the game state offset this module established on
// its own, to reach the pools through the offset it established on its own,
// and - the part nothing else would satisfy - to hand the chat a string that
// reads "The server is restarting..", which is the message the client was
// watched printing when the server really did restart under it.
constexpr std::uint32_t kShutdownForRestart = 0xA060;
constexpr unsigned char kShutdownPrologue[] = {
    0x53, 0x55, 0x56, 0x57,  // push ebx, ebp, esi, edi
    0x33, 0xDB, 0x33, 0xFF,  // xor ebx, ebx; xor edi, edi
    0x8B, 0xF1, 0x33, 0xED,  // mov esi, ecx; xor ebp, ebp
};
// mov dword ptr [esi + 0x3BD], 18
constexpr unsigned char kShutdownWritesState[] = {0xC7, 0x86, 0xBD, 0x03, 0x00,
                                                  0x00, 0x12, 0x00, 0x00, 0x00};
constexpr char kRestartMessage[] = "The server is restarting..";

using ShutdownForRestartFn = void(__thiscall*)(void* net_game);

// And the handler that calls it: what the client runs when the server it was
// in goes away. Watched end to end when the server really was restarted under
// a client standing in the world, it is the whole recovery in one call -
// disconnect, say so, tear the session down, and go back to waiting to
// connect, out of which its own Process rejoins and replays the entry, spawn
// included. Reproducing its steps by hand got everything but the last one, and
// the last one is the one that spawns the character.
//
// Its argument is the packet that brought the news, and it is never read - the
// body touches the stack once, to clean up after the chat call - so it is
// called with nothing.
constexpr std::uint32_t kConnectionLost = 0xA800;
constexpr unsigned char kLostPrologue[] = {
    0x57,                                // push edi
    0x8B, 0xF9,                          // mov edi, ecx
    0x8B, 0x8F, 0xC9, 0x03, 0x00, 0x00,  // mov ecx, [edi + 0x3C9]  (m_pRakClient)
    0x85, 0xC9,                          // test ecx, ecx
    0x74, 0x09,                          // je past the call
    0x8B, 0x01,                          // mov eax, [ecx]          (its vtable)
    0x6A, 0x00,                          // push 0
    0x6A, 0x00,                          // push 0
    0xFF, 0x50, 0x08,                    // call [eax + 8]          (Disconnect)
};
// mov dword ptr [edi + 0x3BD], 9 - the last thing it does, and the step that
// makes the difference between a client that joins and one that spawns.
constexpr unsigned char kLostWritesWaitConnect[] = {0xC7, 0x87, 0xBD, 0x03, 0x00,
                                                     0x00, 0x09, 0x00, 0x00, 0x00};
constexpr char kLostMessage[] = "Lost connection to the server. Reconnecting..";

using ConnectionLostFn = void(__thiscall*)(void* net_game, void* packet);

// RakClientInterface's vtable, in the order RakNet declares it: the destructor
// first, which is why calling slot 0 for Connect would free the object instead
// of joining a server. Both slots are checked against the code they point at
// before either is called.
constexpr int kConnectSlot    = 1;
constexpr int kDisconnectSlot = 2;

// Inside the RakPeer view of the same object: the sleep the client asked its
// own network thread for when it first connected. Read rather than chosen, so
// the reconnect hands RakNet back what SA-MP handed it.
constexpr std::uint32_t kThreadSleepTimer = 0xC06;
constexpr int kDefaultSleepTimer = 5;

// The states CNetGame holds on the way in, watched in this order on a live
// join: 9 waiting to connect, 13 connecting, 15 waiting to join, 14
// connected. Only the two that mean "there is a session to end" and the one
// the bare reconnect leaves behind are needed here.
constexpr std::int32_t kGameStateConnecting   = 13;
constexpr std::int32_t kGameStateConnected    = 14;
constexpr std::int32_t kGameStateAwaitJoin    = 15;
// The one it holds while it puts itself back into a server that went away
// under it is 18, and nothing here writes it: the client's own restart path
// does, and that is reported back as state_left.

// Long enough for RakNet to put the disconnect notification on the wire, so
// the server frees the slot now instead of timing the old session out a minute
// and a half later and turning the rejoin away as already in the game. RakNet
// skips sending it altogether when this is zero.
constexpr std::uint32_t kDisconnectBlockMs = 300;

using ConnectFn = bool(__thiscall*)(void* self, const char* host, std::uint16_t server_port,
                                    std::uint16_t client_port, std::uint32_t depreciated,
                                    int thread_sleep_timer);
using DisconnectFn = void(__thiscall*)(void* self, std::uint32_t block_duration,
                                       std::uint8_t ordering_channel);

std::string Hex(std::uintptr_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(value));
  return buffer;
}

// Walks a chain of `jmp rel32` and returns where it ends up. RakClient's
// forwarders are a jump to a jump, and a stop short of the real function would
// compare against a trampoline instead.
std::uintptr_t FollowJumps(std::uintptr_t code, int links) {
  for (int i = 0; i < links; ++i) {
    unsigned char op = 0;
    std::int32_t rel = 0;
    if (!asi::mem::Read<unsigned char>(code, &op) || op != 0xE9) break;
    if (!asi::mem::Read<std::int32_t>(code + 1, &rel)) return 0;
    code = code + 5 + static_cast<std::uintptr_t>(rel);
  }
  return code;
}

// The one write this module makes. Committed, writable, not a guard page, or
// it does not happen.
bool WriteInt32(std::uintptr_t address, std::int32_t value) {
  MEMORY_BASIC_INFORMATION info = {};
  if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != sizeof(info))
    return false;
  if (info.State != MEM_COMMIT) return false;
  if ((info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY)) == 0)
    return false;
  if ((info.Protect & PAGE_GUARD) != 0) return false;
  const std::uintptr_t end =
      reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
  if (address + sizeof(value) > end) return false;
  std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  return true;
}

// What the launcher was told to connect to. Ground truth: it is not read out of
// the client's memory, so it survives the client losing its netgame object.
std::string CommandLineValue(const char* key) {
  const std::string line = GetCommandLineA();
  const std::size_t at = line.find(key);
  if (at == std::string::npos) return {};
  std::size_t begin = at + std::string(key).size();
  while (begin < line.size() && line[begin] == ' ') ++begin;
  std::size_t end = line.find(' ', begin);
  if (end == std::string::npos) end = line.size();
  return line.substr(begin, end - begin);
}

// What the two vtable slots turned out to be, and the offset their own code
// says the RakClientInterface view sits at inside the object.
struct Calls {
  std::uintptr_t connect = 0;
  std::uintptr_t disconnect = 0;
  std::uint32_t  interface_at = 0;
  // True when something has put a detour over Connect's first bytes. The
  // open.mp client does, and SA-MP's own connect goes through it too, so this
  // is worth reporting but not worth refusing over.
  bool           connect_detoured = false;
  std::string    error;
};

// Reads the two calls out of the client's code, and refuses unless the code
// says what it has to say.
//
// RakClient inherits both RakPeer and RakClientInterface, so one object has two
// views of itself and every interface slot that forwards to a method compiled
// for the primary view begins by taking the difference back off `this`. That
// instruction is where the subobject offset comes from - not from arithmetic on
// two pointers that could be anything - and Disconnect is one of those
// forwarders. Connect is compiled in place, and it opens the way RakNet's
// RakClient::Connect does: it computes the primary view with the same offset
// and calls RakPeer::Disconnect(100, 0) on it to drop whatever is still held.
// So the two slots have to agree about the offset, and the function Disconnect
// forwards to has to be the one Connect calls first. Nothing here is taken on
// declaration order.
Calls ReadCalls(std::uintptr_t vtable, const asi::mem::Module& samp) {
  Calls out;
  std::uint32_t connect_at = 0;
  std::uint32_t disconnect_at = 0;
  if (!asi::mem::Read<std::uint32_t>(vtable + kConnectSlot * 4, &connect_at) ||
      !asi::mem::Read<std::uint32_t>(vtable + kDisconnectSlot * 4, &disconnect_at) ||
      !samp.contains(connect_at) || !samp.contains(disconnect_at)) {
    out.error = "Connect and Disconnect do not both point into samp.dll";
    return out;
  }

  unsigned char thunk[16] = {};
  if (asi::mem::ReadGuarded(disconnect_at, thunk, sizeof(thunk)) != sizeof(thunk) ||
      thunk[0] != 0x81 || thunk[1] != 0xE9) {
    out.error = "slot 2 at " + Hex(disconnect_at) +
                " does not begin by adjusting `this`, so it is not the "
                "forwarder Disconnect has to be";
    return out;
  }
  std::uint32_t interface_at = 0;
  std::memcpy(&interface_at, thunk + 2, sizeof(interface_at));
  if (interface_at == 0 || interface_at > 0x10000) {
    out.error = "slot 2 adjusts `this` by " + Hex(interface_at) +
                ", which is not a subobject offset";
    return out;
  }

  unsigned char head[24] = {};
  if (asi::mem::ReadGuarded(connect_at, head, sizeof(head)) != sizeof(head)) {
    out.error = "slot 1 at " + Hex(connect_at) + " cannot be read";
    return out;
  }
  // Offsets 0 to 4 are the ones a five-byte detour takes, so the signature
  // starts past them: `lea ebx, [edi - interface_at]` and `push 100`, the two
  // instructions that set up RakPeer::Disconnect(100, 0).
  std::int32_t lea_displacement = 0;
  std::memcpy(&lea_displacement, head + 10, sizeof(lea_displacement));
  if (head[8] != 0x8D || head[9] != 0x9F ||
      lea_displacement != -static_cast<std::int32_t>(interface_at) || head[14] != 0x6A ||
      head[15] != 0x64) {
    out.error = "slot 1 at " + Hex(connect_at) +
                " does not set up RakPeer::Disconnect(100, 0) on a view " +
                Hex(interface_at) + " back, the way RakClient::Connect does";
    return out;
  }
  // And the call that follows has to land where Disconnect forwards to.
  std::int32_t call_relative = 0;
  std::memcpy(&call_relative, head + 19, sizeof(call_relative));
  if (head[16] != 0x8B || head[17] != 0xCB || head[18] != 0xE8) {
    out.error = "slot 1 at " + Hex(connect_at) + " does not call anything where "
                "RakClient::Connect calls RakPeer::Disconnect";
    return out;
  }
  const std::uintptr_t called =
      connect_at + 23 + static_cast<std::uintptr_t>(call_relative);
  const std::uintptr_t forwarded = FollowJumps(disconnect_at + 6, 3);
  if (called == 0 || forwarded == 0 || called != forwarded) {
    out.error = "slot 2 forwards to " + Hex(forwarded) + " but slot 1 calls " +
                Hex(called) + ", so they are not Disconnect and Connect";
    return out;
  }

  out.connect = connect_at;
  out.disconnect = disconnect_at;
  out.interface_at = interface_at;
  out.connect_detoured = head[0] != 0x53 || head[1] != 0x55 || head[2] != 0x56 ||
                         head[3] != 0x57;
  return out;
}

// Finds the client's own restart path and proves it is that, out of the code
// itself. Returns 0 with a reason when anything fails to line up.
std::uintptr_t FindShutdownForRestart(const asi::mem::Module& samp, std::string* why) {
  const std::uintptr_t at = samp.base + kShutdownForRestart;
  unsigned char code[0x200] = {};
  if (asi::mem::ReadGuarded(at, code, sizeof(code)) != sizeof(code)) {
    *why = "the restart path at " + Hex(at) + " cannot be read";
    return 0;
  }
  if (std::memcmp(code, kShutdownPrologue, sizeof(kShutdownPrologue)) != 0) {
    *why = "the code at " + Hex(at) + " does not begin the way the client's "
           "restart path does";
    return 0;
  }
  bool writes_state = false;
  for (std::size_t i = 0; i + sizeof(kShutdownWritesState) <= sizeof(code); ++i) {
    if (std::memcmp(code + i, kShutdownWritesState, sizeof(kShutdownWritesState)) == 0) {
      writes_state = true;
      break;
    }
  }
  if (!writes_state) {
    *why = "the code at " + Hex(at) + " never puts the game state into "
           "restarting, so it is not the path that does this";
    return 0;
  }
  // And it has to say so. Every `push imm32` in the body is followed to see
  // whether it is the message the client prints when a server restarts under
  // it; nothing else in the client would be pushing that string.
  for (std::size_t i = 0; i + 5 <= sizeof(code); ++i) {
    if (code[i] != 0x68) continue;
    std::uint32_t pushed = 0;
    std::memcpy(&pushed, code + i + 1, sizeof(pushed));
    if (!samp.contains(pushed)) continue;
    if (asi::mem::ReadCString(pushed, sizeof(kRestartMessage) + 4) == kRestartMessage)
      return at;
  }
  *why = "the code at " + Hex(at) + " does not print \"" + kRestartMessage +
         "\", which is how the client's own restart path ends";
  return 0;
}

// Finds the recovery the client runs for itself, and proves it out of the code
// rather than out of a table: it has to reach RakClient at the offset this
// module established, call the vtable slot this module identified as
// Disconnect, print the message the client was watched printing, call the
// restart path already proved above, and end by putting the game state back to
// waiting to connect. Nothing else in the client satisfies all five.
std::uintptr_t FindConnectionLost(const asi::mem::Module& samp, std::uintptr_t restart_at,
                                  std::string* why) {
  const std::uintptr_t at = samp.base + kConnectionLost;
  unsigned char code[0x100] = {};
  if (asi::mem::ReadGuarded(at, code, sizeof(code)) != sizeof(code)) {
    *why = "the recovery at " + Hex(at) + " cannot be read";
    return 0;
  }
  if (std::memcmp(code, kLostPrologue, sizeof(kLostPrologue)) != 0) {
    *why = "the code at " + Hex(at) +
           " does not open by asking RakClient to disconnect, so it is not the "
           "client's own recovery";
    return 0;
  }
  bool goes_back_to_waiting = false;
  bool calls_the_restart = false;
  bool says_so = false;
  for (std::size_t i = 0; i + sizeof(kLostWritesWaitConnect) <= sizeof(code); ++i) {
    if (std::memcmp(code + i, kLostWritesWaitConnect,
                    sizeof(kLostWritesWaitConnect)) == 0) {
      goes_back_to_waiting = true;
      break;
    }
  }
  for (std::size_t i = 0; i + 5 <= sizeof(code); ++i) {
    if (code[i] == 0xE8) {
      std::int32_t rel = 0;
      std::memcpy(&rel, code + i + 1, sizeof(rel));
      if (at + i + 5 + static_cast<std::uintptr_t>(rel) == restart_at)
        calls_the_restart = true;
    } else if (code[i] == 0x68) {
      std::uint32_t pushed = 0;
      std::memcpy(&pushed, code + i + 1, sizeof(pushed));
      if (samp.contains(pushed) &&
          asi::mem::ReadCString(pushed, sizeof(kLostMessage) + 4) == kLostMessage)
        says_so = true;
    }
  }
  if (!calls_the_restart) {
    *why = "the code at " + Hex(at) + " does not call the restart path at " +
           Hex(restart_at);
    return 0;
  }
  if (!says_so) {
    *why = "the code at " + Hex(at) + " does not print \"" + kLostMessage + "\"";
    return 0;
  }
  if (!goes_back_to_waiting) {
    *why = "the code at " + Hex(at) +
           " does not end by going back to waiting to connect, which is the "
           "step that gets the character spawned again";
    return 0;
  }
  return at;
}

const char* RouteName(Route route) {
  switch (route) {
    case Route::kRestart: return "restart";
    case Route::kCalls:   return "calls";
  }
  return "?";
}

}  // namespace

json Reconnect(Route route) {
  const Client client = Detect();
  if (client.base == 0) return json{{"error", "samp.dll is not loaded"}};
  if (client.version != Version::k037R1)
    return json{{"error", std::string("the RakClient layout was established on "
                                      "0.3.7-R1 and this client is ") +
                              ToString(client.version) +
                              " - nothing here may be called against it"}};
  const asi::mem::Module samp = asi::mem::FindModule(L"samp.dll");
  if (!samp.valid()) return json{{"error", "samp.dll has no module entry"}};

  std::uint32_t net_game = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + 0x21A0F8, &net_game) || net_game == 0) {
    // The one case with no way back from in here: all of this goes through
    // CNetGame, and the client makes one only when it first tries to reach a
    // server. So the refusal names the command that starts the game again,
    // filled in with the address this client was given - whoever reads this
    // cannot ask the client for it any more.
    const std::string host = CommandLineValue("-h ");
    const std::string port = CommandLineValue("-p ");
    const std::string nick = CommandLineValue("-n ");
    std::string again = "python tools/testrun.py quit, then python tools/testrun.py launch";
    if (!host.empty()) again += " --host " + host;
    if (!port.empty()) again += " --port " + port;
    if (!nick.empty()) again += " --nick " + nick;
    return json{{"error",
                 "the client has no CNetGame, and everything here goes through "
                 "one: it makes one when it first tries to reach a server, so "
                 "this client has either not got that far yet or has stopped "
                 "trying, and nothing in here can make another"},
                {"next",
                 "if the game is still starting up, wait and ask again; "
                 "otherwise it has to be started over, which is the one thing "
                 "this cannot do for itself: " + again}};
  }

  const std::string was = ConnectionState();
  std::int32_t state = 0;
  asi::mem::Read<std::int32_t>(net_game + kGameState, &state);
  // Whether there is still a session to end. The client holds these two while
  // it believes it is in a server, which it goes on believing after the server
  // has closed the connection on it.
  const bool held = state == kGameStateConnected || state == kGameStateAwaitJoin;

  json out{{"route", RouteName(route)}, {"was", was}, {"had_a_session", held}};

  // Only the routes that call something need the RakClient found and its code
  // checked. Putting the state back needs neither, which is the whole
  // attraction of it.
  Calls calls;
  std::uint32_t rak = 0;
  std::string host;
  int port = 0;
  std::int32_t sleep_timer = kDefaultSleepTimer;
  {
    if (!asi::mem::Read<std::uint32_t>(net_game + kRakClientView, &rak) || rak == 0)
      return json{{"error", "CNetGame is not holding a RakClient"}};
    std::uint32_t vtable = 0;
    if (!asi::mem::Read<std::uint32_t>(rak, &vtable) || !samp.contains(vtable))
      return json{{"error", "the RakClientInterface vtable at " + Hex(vtable) +
                                " is not inside samp.dll"}};
    calls = ReadCalls(vtable, samp);
    if (!calls.error.empty()) return json{{"error", calls.error}};

    // The primary view of the same object, at the offset its own thunks name.
    // It is a second C++ object as far as the compiler is concerned, so it
    // carries a vtable of its own, and that is what says the offset landed
    // where it should.
    const std::uintptr_t peer = rak - calls.interface_at;
    std::uint32_t peer_vtable = 0;
    if (!asi::mem::Read<std::uint32_t>(peer, &peer_vtable) || !samp.contains(peer_vtable))
      return json{{"error", "the object " + Hex(calls.interface_at) +
                                " back from the interface has no vtable of its "
                                "own inside samp.dll, so it is not RakClient"}};
    if (!asi::mem::Read<std::int32_t>(peer + kThreadSleepTimer, &sleep_timer) ||
        sleep_timer <= 0 || sleep_timer > 1000)
      sleep_timer = kDefaultSleepTimer;
    out["rak_client"] = Hex(rak);
    out["interface_at"] = Hex(calls.interface_at);
    if (calls.connect_detoured)
      out["connect_detoured"] = "something is hooking Connect - the open.mp "
                                "client does, and SA-MP's own connect goes "
                                "through it as well";
  }

  if (route == Route::kCalls) {
    // The launcher's address first; the client's own copy is the fallback, and
    // it is only there while the netgame object still holds one.
    host = CommandLineValue("-h ");
    const std::string port_text = CommandLineValue("-p ");
    if (host.empty()) host = asi::mem::ReadCString(net_game + 0x20, 64);
    port = port_text.empty() ? 0 : std::atoi(port_text.c_str());
    if (port <= 0 || port > 65535) {
      std::uint16_t stored = 0;
      if (asi::mem::Read<std::uint16_t>(net_game + 0x225, &stored)) port = stored;
    }
    if (host.empty() || port <= 0 || port > 65535)
      return json{{"error", "no address to reconnect to: host '" + host + "', port " +
                                std::to_string(port)}};
    out["host"] = host;
    out["port"] = port;
    out["thread_sleep_timer"] = sleep_timer;
  }

  // The restart path has to be found and proved before anything is called,
  // since it is the whole route.
  std::uintptr_t recovery_at = 0;
  if (route == Route::kRestart) {
    std::string why;
    const std::uintptr_t shutdown_at = FindShutdownForRestart(samp, &why);
    if (shutdown_at == 0) return json{{"error", why}};
    recovery_at = FindConnectionLost(samp, shutdown_at, &why);
    if (recovery_at == 0) return json{{"error", why}};
    out["restart_path"] = Hex(shutdown_at);
    out["recovery"] = Hex(recovery_at);
  }

  // Tell the server first, so it frees the slot now rather than timing the old
  // session out. The client's own connect does disconnect, but without a block
  // duration; asking here waits long enough for RakNet to put the notification
  // on the wire.
  if (held) {
    const auto disconnect = reinterpret_cast<DisconnectFn>(calls.disconnect);
    disconnect(reinterpret_cast<void*>(rak), kDisconnectBlockMs, 0);
    out["parted"] = true;
  }

  if (route == Route::kRestart) {
    // One call, and all of it is the client's own doing: the players go, the
    // local player goes, the pools are reset, and it puts itself back to
    // waiting to connect. Nothing here writes a state by hand.
    const auto recover = reinterpret_cast<ConnectionLostFn>(recovery_at);
    recover(reinterpret_cast<void*>(net_game), nullptr);
    std::int32_t left = 0;
    asi::mem::Read<std::int32_t>(net_game + kGameState, &left);
    out["state_left"] = left;
  } else if (!WriteInt32(net_game + kGameState, kGameStateConnecting)) {
    return json{{"error", "the game state at CNetGame+0x3BD could not be written, "
                          "and nothing else here is worth doing without it"}};
  }

  if (route == Route::kCalls) {
    const auto connect = reinterpret_cast<ConnectFn>(calls.connect);
    out["asked"] = connect(reinterpret_cast<void*>(rak), host.c_str(),
                           static_cast<std::uint16_t>(port), 0, 0, sleep_timer);
    out["note"] = out["asked"].get<bool>()
                      ? "the client is joining again - poll ready until it says "
                        "spawned; the world reads again once the pool refills"
                      : "the client refused the connect call";
  } else {
    out["note"] = "the client has torn the session down the way it does when a "
                  "server restarts under it, and joins again by itself from "
                  "here - poll ready until spawned has been false and is true "
                  "again, which is what says the character was spawned anew";
  }

  // Whatever the pools held belonged to the session that just ended.
  ForgetLayout();

  LOG_INFO("reconnect: route {}, was {} -> {}", RouteName(route), was, out.dump());
  return out;
}

}  // namespace gtabot::samp
