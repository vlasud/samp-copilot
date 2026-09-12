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
// join: waiting to connect, connecting, waiting to join, connected. The first
// is the one the client connects out of by itself - about two seconds after it
// gets there, its own Process calls its own Connect - which is the whole point
// of writing it rather than calling anything.
constexpr std::int32_t kGameStateWaitConnect = 9;
constexpr std::int32_t kGameStateConnecting   = 13;
constexpr std::int32_t kGameStateConnected    = 14;
constexpr std::int32_t kGameStateAwaitJoin    = 15;

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

const char* RouteName(Route route) {
  switch (route) {
    case Route::kState:         return "state";
    case Route::kPartThenState: return "part-then-state";
    case Route::kCalls:         return "calls";
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
  if (!asi::mem::Read<std::uint32_t>(client.base + 0x21A0F8, &net_game) || net_game == 0)
    return json{{"error",
                 "the client has no CNetGame: it gives the object up once it "
                 "stops trying to reach a server, and from there only starting "
                 "the game again can make another one"}};

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
  if (route != Route::kState) {
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

  // Ask for the disconnect where the route says to, so the server is told at
  // once. The client's own connect does disconnect first, but on its own
  // terms; asking here blocks long enough for RakNet to put the notification
  // on the wire.
  if (held && route != Route::kState) {
    const auto disconnect = reinterpret_cast<DisconnectFn>(calls.disconnect);
    disconnect(reinterpret_cast<void*>(rak), kDisconnectBlockMs, 0);
    out["parted"] = true;
  }

  const std::int32_t write_state =
      route == Route::kCalls ? kGameStateConnecting : kGameStateWaitConnect;
  if (!WriteInt32(net_game + kGameState, write_state))
    return json{{"error", "the game state at CNetGame+0x3BD could not be written, "
                          "and nothing else here is worth doing without it"}};
  out["state_written"] = write_state;

  if (route == Route::kCalls) {
    const auto connect = reinterpret_cast<ConnectFn>(calls.connect);
    out["asked"] = connect(reinterpret_cast<void*>(rak), host.c_str(),
                           static_cast<std::uint16_t>(port), 0, 0, sleep_timer);
    out["note"] = out["asked"].get<bool>()
                      ? "the client is joining again - poll ready until it says "
                        "spawned; the world reads again once the pool refills"
                      : "the client refused the connect call";
  } else {
    out["note"] = "the client is back to waiting to connect and does the rest "
                  "itself, about two seconds from now - poll ready until it "
                  "says spawned";
  }

  // Whatever the pools held belonged to the session that just ended.
  ForgetLayout();

  LOG_INFO("reconnect: route {}, was {}, state written {} -> {}", RouteName(route), was,
           write_state, out.dump());
  return out;
}

}  // namespace gtabot::samp
