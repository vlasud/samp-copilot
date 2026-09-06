# gtabot

A bridge between an AI agent and a GTA San Andreas / SA-MP client.

One artifact: `bot.asi`, loaded into the game. Inside it sit the frame hook, the
state collector, the in-game debug overlay, and an MCP server the agent talks to
over loopback HTTP. There is no second process and no IPC.

```
gta_sa.exe
+-- bot.asi
    +-- frame hook (d3d9 Present / EndScene / Reset)
    |     +-- world snapshots, queued actions, ImGui overlay
    +-- MCP server -> http://127.0.0.1:8765/mcp  <- the agent
```

## Build

`gta_sa.exe` is 32-bit, so everything here is x86 - the top-level
`CMakeLists.txt` refuses to configure otherwise.

```
cmake --preset x86
cmake --build --preset release
```

`bot.asi` lands in `build/x86/bin/RelWithDebInfo/`. Requires Visual Studio 2026
(or any MSVC with an x86 toolset) and CMake 3.25+. nlohmann/json, spdlog,
MinHook and Dear ImGui are fetched at configure time.

## Install

Copy `bot.asi` into the game folder (`D:\SAMP`). The ASI loader already there
(`vorbisFile.dll`, with the original renamed to `vorbisHooked.dll`) picks up any
`*.asi` in that directory.

## Use

Start the game. **F11** cycles the in-game panel through hidden, passive and
interactive. Interactive gives it the mouse so it can be dragged and resized;
where it ends up is remembered in `bot.imgui.ini` next to the .asi. (F8 is
GTA's screenshot key and F9 was taken, hence F11.)

While dragging, the camera still turns: GTA reads the mouse through
DirectInput rather than window messages, so swallowing those messages does not
reach it. The easy way round it is to position the panel with the game paused
on the ESC menu - the overlay keeps drawing there and mouse look is off., which shows the frame counter,
hook integrity, the SA-MP build, the MCP endpoint and the tail of the log -
everything that used to require alt-tabbing to a terminal.

To drive it from outside:

```
python tools/console.py     # interactive, stands in for the agent loop
python tools/selftest.py    # checks the endpoint end to end
```

Both of those have a limit worth knowing: to type in a terminal you have to
leave the game, and the render thread parks when the window is not in the
foreground. Anything that needs the game thread - `probe_memory`, and later
every action - times out while you are there. So the module probes its own
memory access once at startup and writes the result to the log, and the panel
has a **Run memory probe** button in its interactive state. Both work without
alt-tabbing anywhere.

```
Get-Content D:\SAMPot.asi.log | Select-String probe
```

Both talk plain JSON-RPC over HTTP; `tools/mcp_http.py` is the whole client and
is short enough to paste into an agent loop.

The mod also writes `bot.asi.log` beside itself.

## When it crashes

`bot.asi.log` is written before anything else and holds three things worth
reading:

- `Present -> ...`, `EndScene -> ...`, `Reset -> ...` name the module that owns
  each vtable slot. On this machine Present and EndScene are `d3d9.dll` but
  Reset is `apphelp.dll`: GTA SA runs under a Windows compatibility shim that
  owns that slot.
- `game device Reset -> ... (we hooked ...)` compares the entry point the game's
  own device uses against the one resolved at startup from a throwaway device.
  They are not always the same, and the hook moves onto the game's if they
  differ.
- `overlay faulted while drawing` means the panel hit an access violation and
  switched itself off. The rest of the module keeps running.
- `CRASH ...` names the exception, the faulting module and offset, and the
  address being accessed. It is logged before the game's own handler runs.

## A frozen frame counter

The frame hook only runs while the game renders, and GTA SA stops presenting
when it is minimised out of exclusive fullscreen. `bot_status` says which case
you are in:

- `verdict: ok` - frames are flowing.
- `patch intact but no frames` - the game is not rendering. Alt-tab back in.
- `our patch bytes are gone` - something else rewrote the entry point.

Tools that need the game thread (`probe_memory`) time out with that same
explanation rather than hanging.

## Two mistakes worth not repeating

**Drawing on every EndScene bakes the panel into game textures.** GTA ends a
scene for each off-screen target it renders - the radar, mirrors, the text on
signs - so an overlay drawn unconditionally ends up inside those textures and
then appears, huge, on a prison wall. The panel now draws only when render
target 0 is the swap chain back buffer.

**Holding D3D resources across a device loss makes Reset fail.** Alt-tabbing
out of exclusive fullscreen loses the device; `Reset` then returns
`D3DERR_INVALIDCALL` (0x8876086C) while any D3DPOOL_DEFAULT resource is still
alive, and the game gives up with its own error box. Resources are released on
the first of three signals: `Present` returning `D3DERR_DEVICELOST`,
`TestCooperativeLevel` reporting anything but `D3D_OK`, or the Reset hook.

Releasing is only half of it - the release has to happen *before* somebody
calls Reset, and here that somebody is not the game. The "Device::Reset()
result 8876086C" box comes from `vc.asi`, a Rust client that wraps the device
and resets it itself across an alt-tab, without passing through any hook of
ours. So the trigger cannot be a Reset hook at all: resources are dropped as
soon as the game window stops being the foreground window, checked in both the
EndScene and the Present hook. The Reset hook stays as a backstop and logs
every call it does see.

## Where the SA-MP offsets come from

The field order comes from the public
[SAMP-API](https://github.com/BlastHackNet/SAMP-API) headers for 0.3.7-R1. The
byte offsets do not: working those out by hand from a class declaration means
one wrong assumption about padding produces plausible-looking nonsense. So
each one is either anchored to something checkable or found by the shape of
the data:

| What | How it is established |
|---|---|
| `CNetGame` | `*(samp.dll + 0x21A0F8)`, and only accepted if the host address at +0x20 matches the `-h` the launcher was given |
| alignment | the client's structures are packed: the port sits at +0x225 and the pools pointer at +0x3CD, neither on a four-byte boundary, so every search steps one byte at a time |
| `CNetGame::Pools` | nine consecutive heap pointers near the tail of CNetGame, found by that shape |
| `CPlayerPool` slots | 1004 `CPlayerInfo*` followed by 1004 flags that are only ever 0 or 1 - a signature nothing else matches |
| `std::string` | two plausible MSVC layouts; the one that yields a readable local player name is the one this client was built with |

When any of that fails to line up, `get_world` reports `resolved: false` with a
note saying where it stopped, rather than reporting something wrong.

## Establishing the player pool's layout

Every offset inside the SA-MP client is build-specific, and copying a table of
them off a forum means the mod either works or quietly reports nonsense, with
no way to tell which. So the layout is derived from evidence instead.

The one fact that did not come from memory is the nickname the launcher passed
on the command line. Every live copy of that string sits inside a real SA-MP
structure, so `dump_samp_structures` (also a button on the panel, also
attempted automatically every 20s until it succeeds) writes
`bot.samp-report.txt` next to the module: each occurrence, what points at it,
and the words around it with each one classified - a pointer into samp.dll, a
pointer into the heap, a small integer, a float, or text.

That report is what the offsets get written from. Until it exists, the world
snapshot stays a set of empty placeholders rather than a guess.

## SA-MP versions

Every client structure offset is version-specific, so the mod refuses to read
client memory unless it recognises the build. Detection uses the loaded
module's `SizeOfImage` and `TimeDateStamp`. Only `0.3.7-R1` is verified today.
To add another:

```
python tools/fingerprint_samp.py path/to/samp.dll
```

and paste the resulting line into `kKnown[]` in `src/asi/samp/version.cpp`.

## Layout

```
src/asi/  dllmain.cpp     entry point and worker thread
          types.*         shared json alias and action names
          log.*           file log plus the ring the overlay draws
          bridge.*        post work to the game thread; world snapshot slot
          hooks/frame.*   per-frame callback via the d3d9 vtable
          samp/           SA-MP client version detection
          state/memory.*  validated read-only access to the process
          state/probe.*   snapshot builder and the memory probe
          mcp/server.*    JSON-RPC 2.0 and the tool registry
          mcp/http.*      loopback HTTP transport
          mcp/rpc.*       makes a game-thread round trip synchronous
          mcp/tools.*     the tool definitions
          ui/overlay.*    the ImGui panel
tools/    console.py, selftest.py, mcp_http.py, fingerprint_samp.py
```

## What it reads

| | Source | Checked against |
|---|---|---|
| players: id, name, NPC, team, state | `CPlayerPool` | ids match the in-game scoreboard exactly |
| positions and distances | `CPed` -> game entity -> `CPlaceable` matrix | the person beside you reads 1.0 m, one walking away 10 m then 33 m |
| our health, armour, weapon, ammo | game ped, plugin-sdk offsets | 79 hp against the HUD bar; weapon 15 the minute the chat said a cane was bought |
| vehicles: id, model, position | GTA's own `CPool` at `gta_sa.exe+0x774494` | a row of scooters at 1.6, 3.3, 9.1 m, all model 462 - the id another mod's overlay showed for them |

Not read, and not guessed at either:

- **Score and ping.** Absent from `CPlayerInfo` and from `CRemotePlayer`; none
  of the values the scoreboard shows appears anywhere in either. They arrive
  from the server in an RPC and belong with that work.
- **Other players' health and weapons.** Their game ped is a local puppet -
  SA-MP gives it a large health so it cannot die on our machine, which is why
  every player read back as 1000 hp holding a fist. The true value is
  `m_fReportedHealth`, which also arrives by RPC.
- **Money.** The HUD shows more than fits in the ped field that holds it.

## How offsets get established

Every offset here was either taken from a public declaration and then checked
against something observable, or derived from the data outright. The pattern
that kept working:

- **Anchor on something known from outside memory.** `CNetGame` is only
  believed when the host address inside it matches the `-h` the launcher was
  given.
- **Find structures by shape, not arithmetic.** Deriving an offset by hand
  from a class declaration means one wrong padding assumption produces
  plausible nonsense. The client's structures are packed - the port sits at
  +0x225, the pools pointer at +0x3CD - so every search steps one byte at a
  time.
- **Prefer a signature that cannot be satisfied by accident.** Vehicles are
  found by being entities with a vehicle model index at a sensible place on
  the map, which is not something arbitrary bytes produce. Looking for the
  shape of their array failed four times.
- **Say where a search stopped.** "Did not check out" costs a round trip;
  naming the stage and printing what it read does not.

Three findings cost a day between them and are worth not rediscovering:

1. Two arrays shifted by the same amount stay correlated, so a shape test that
   only checks agreement between them can land slots early and pass. The
   player slots were eight bytes out and the ids two too high.
2. A field that is small, non-zero and different for everyone describes a ping
   and also describes `std::string`'s own length field. "doom" had a ping of 4.
3. A measurement taken once at startup is not a property of the layout. Scores
   and pings are all legitimately zero seconds after connecting, and the
   vehicle pool is legitimately empty.

## Status


Working: the frame hook, the game-thread bridge, the MCP server, the overlay,
and the world reading described above.

Not started: the RPC layer - chat, dialogs, and the values that only arrive in
packets - and every action. Actions answer with an explicit "not implemented
yet" rather than a false success.

Not covered by tests: with the server inside the game process, nothing can be
exercised without the game running. `tools/selftest.py` is the replacement and
needs a live session.
