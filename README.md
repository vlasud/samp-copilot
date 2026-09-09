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

Start the game. A small badge sits at the right of the screen with the bot's
state; **F11** opens the menu. The menu is driven from the keyboard - arrows
or W/S to choose, Enter to pick, left/right to switch a setting on or off,
Esc or F11 to close - and never takes the mouse, so the camera and the cursor
stay the game's. While it is open its keys do not reach the game or SA-MP.

What the menu offers: go to the marker you put on the map (Esc, Map, click -
the usual waypoint), stop, allow or forbid the bot to move the character,
sprint, bunny hop, the route drawn on the ground, the badge, and the
developer's panel.

The developer's panel is the last item. It opens interactive, with the mouse
(drag and resize it; where it ends up is remembered in `bot.imgui.ini` next
to the .asi); F11 makes it passive, and F11 again closes it back to the
badge. While dragging, the camera still turns: GTA reads the mouse through
DirectInput rather than window messages, so swallowing those messages does
not reach it. Position the panel with the game paused on the ESC menu if that
bothers you. (F8 is GTA's screenshot key and F9 was taken, hence F11.)

Hotkeys outside the menu: **Ctrl+F11** arms or disarms the bot's movement,
**Ctrl+F12** stops everything, disarms and closes the panel.

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

### Driving a test run

`tools/testrun.py` does the whole loop from a command line: start the game,
wait until the character can be driven, send him somewhere and watch him get
there, read the log, close the game.

```
python tools/testrun.py launch
python tools/testrun.py wait
python tools/testrun.py travel 1468 -1689
python tools/testrun.py log --grep "plan|walk" --since 19:08
python tools/testrun.py quit
```

`ready` is the call worth polling: it answers where the session stands -
client loaded, character spawned, dialog on screen, movement armed, world
readable - and names the next call to make in `next`. `wait` also answers
message-box dialogs on the way in, since a message box has no way past but
its button and nothing else happens until it is pressed.

### One look, then act

`look` answers with the whole picture at once - himself, the dialog on screen,
what has just been said and by whom, the people and things around, what he is
in the middle of doing. It is meant to be called on a loop, about once a
second, so that deciding what to do next happens against a snapshot rather
than a dozen separate questions asked at slightly different moments.

That is the shape of the whole thing: a brain that looks, consults the rules
for the server it is on, and sends a chain of actions; and this module, which
looks and does and decides nothing.

### Where knowledge lives

The module gathers facts and works the controls. It does not know anything
about any particular server, and must not: which skin is a medic, which key a
prompt means, what order a hospital wants things done in - all of that is one
server's habits, and a different server has different ones.

So the module reports and never interprets. `get_npcs` gives a ped's skin;
what a skin means is written in `servers/<name>.md`, next to the rest of that
server's rules, for whoever is deciding what to do. Strategy is the agent's;
the module is the hands and the eyes.

### Tests

```
cmake --build --preset release --target bot_tests
build/x86/bin/RelWithDebInfo/bot_tests.exe
```

Most of this project only means anything inside gta_sa.exe and can only be
tried by playing. Some of it is not like that - reading a chat line, deciding
what a menu row says, adding up what is known about a stranger - and that part
was being shipped on the strength of one look at the screen. It runs off the
game now, against lines pasted out of the real server's chat unchanged.

### Walking a menu

`follow_dialog` takes a path - `["Список команд", "Мин. здравоохранения"]` -
and walks it, optionally opening the chain first with `open_with: "/menu"`.
Each step names a row rather than counting to it, because a server renumbers
its menus between updates and orders them differently for a player of a
different rank. Answering one dialog at a time means pressing and then polling
until the server has sent the next one, which is a round trip each time, and
getting that wait wrong is how a step lands in the wrong menu. `follow_dialog`
does the waiting; `dialog_path_status` says where it got to, and lists what
was on screen when a step named something that was not there.

### Whether he fits

Two questions look the same and are not: "is the line ahead clear" and "does
his body fit through there". The walker asked the first, so a chair leg half
a metre off the centre line was invisible until he walked into it, and the
room map drew routes through gaps between beds that his shoulders do not go
through - after which he spent twenty seconds finding that out with his face,
in the middle of a ward, in front of everybody.

Both ask the second now. Each whisker is swept at the width of his shoulders,
and a square of the room map counts only if a person could stand in the
middle of it without touching anything - four short lines out to shoulder
width at knee and chest, asked of the squares the flood actually reaches. In
the picture such a square is an `o`: floor, but too narrow for him.

Measured on the trip that used to fail - reception to the far ward - it went
from stopping seven metres short after two minutes to arriving in seventy
seconds, with the number of "leaned on it, stepped round it, dead end" moments
down from twenty-seven to eleven.

### Who is who

`get_people` keeps a record of everyone seen or heard and puts a standing on
each - friend, neutral, wary, enemy - with the reason. The reasons are only
what a client can honestly know: how near somebody has been, whether he was
carrying anything, whether he has addressed this character by name, and
whether he happened to be armed and close at the moment this character lost
health. The last of those is a coincidence counted twice before it means
anything, and it is labelled as a coincidence, because a server sends a
health value and never says who took it. `set_standing` overrides the
judgement and always wins.

### The keyboard layout

Under a Russian layout the game sees no letter keys at all. It asks Windows
what character a key produces and files the key under that character, so W
arrives as "ц", T does not open the chat, and nothing bound to a letter
works. The module now notices and asks its own window for English; from
outside, `tools/layout.ps1 -Lang en` does the same. Everything that looked
like a mysterious half-working input - the walk moving but the chat never
opening - was this.

### Looking at the screen

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/screenshot.ps1 -Path shot.png
powershell -NoProfile -ExecutionPolicy Bypass -File tools/focus.ps1
```

The tools report positions, models and text, and there are things none of
that says. A character standing the wrong side of a counter, a server's
refusal painted across the chat, a prompt drawn over the view: one picture
settles in a second what an hour of coordinates argues about. It grabs the
game's own window, so it works with the game behind other windows as long as
it is not minimised. `focus.ps1` puts the game back in front, which matters
because keystrokes go to whichever window has the focus - taking a screenshot
from a terminal is enough to lose it, and then a password typed into the
server's dialog lands in the terminal instead.

The `-ExecutionPolicy Bypass` is not optional on this machine: without it
PowerShell refuses to run either script and says so on stderr, which a script
calling them is likely to throw away.

### The server's password

A server that asks for a password asks a person. When there is no person
about, put the password in `bot.login` beside the module and the mod types it
into the server's password dialog itself, one character a frame through the
system's own input.

```
# bot.login - the whole file may be just the password.
password=whatever it is
# Optional: only a dialog whose caption contains this is answered.
caption=Авторизация
# Optional: auto=off leaves it for an explicit `login` call.
auto=on
```

It is deliberately narrow: only a dialog the server marked as a *password
input* is answered, only before the character first spawns, only once in a
session, and only while the game's window is in front. A bank PIN asked later
in the evening is not the account password and does not get one.

The password never reaches the log, the interface or the network, and
`bot.login` is in `.gitignore`. Plain text is accepted, and so is the output
of PowerShell's `ConvertFrom-SecureString`, which ties the file to the Windows
account that wrote it and costs one command:

```
Read-Host "password" -AsSecureString | ConvertFrom-SecureString |
    Set-Content D:\SAMPot.login
```

Either way, anyone who can use your unlocked Windows session can now log into
your game account. That is what automating a login means.

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
          samp/           SA-MP client version detection, the player pool,
                          the chat log
          game/exe.*      fingerprints gta_sa.exe; only 1.0 US may be called
          game/world_query.* calls into the game: ground under a point, line
                          of sight, world-to-screen
          game/paths.*    the game's own ped/vehicle node graph, found by shape
          nav/planner.*   standable / walkable / plan a route
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
| other players' health and armour | `CRemotePlayer+0x1BC`, what the server reported | one armed player on 63 hp and 100 armour among civilians on 100 |
| what other players are holding | their game ped, same fields as ours | a desert eagle on the one player carrying one |
| the chat log | a ring of entries behind `samp.dll+0x21A0E4`, recognised by its shape | every line matches the screen; the connect line names the server address we already verified |

Not read, and not guessed at either:

- **Score and ping.** Absent from `CPlayerInfo` and from `CRemotePlayer`; none
  of the values the scoreboard shows appears anywhere in either. They arrive
  from the server in an RPC and belong with that work.
- **Money.** The HUD shows more than fits in the ped field that holds it.

## What it asks the game

Movement needs answers reading memory cannot give: whether there is ground
under a point, whether a line between two points passes through a wall. For
those the module calls the functions the game's own pedestrians use, in
`gta_sa.exe`:

| Question | Call | Verified by |
|---|---|---|
| ground under a point | `CWorld::FindGroundZFor3DCoord` | the ground under the player is a ped's height below him, or nothing is called |
| anything solid between two points | `CWorld::GetIsLineOfSightClear` | the red spokes on screen stop at the walls they stop at |
| where a world point lands on screen | `CSprite::CalcScreenCoors` | the drawn route lies on the pavement it runs over |

A call to the wrong address is not a wrong answer, it is the session gone,
so two gates stand in front of every one of them. The executable has to be
the build the addresses belong to - identified by its link timestamp, the
same value Windows prints in a crash report - and the ground function has to
pass its self-check against the player's own position before any other call
is allowed. Each call is also guarded, so a fault becomes "no answer".

These only know about what is streamed in. "No ground" a few hundred metres
away is the game saying how far it can see, and the planner says so rather
than calling it a hole.

### Where the offsets came from

The reversed source at gitlab.com/gtahackers/gta-reversed settled in one
reading what shape-searching had failed at for a day, and corrected a mistake
that had been quietly steering the whole investigation:

- `CPathFind` has **seventy-two** areas, not sixty-four - sixty-four of map and
  eight of interiors - so the stride between its arrays is 288 bytes, and its
  node counts are 32-bit. The search was looking for three 64-entry arrays 256
  bytes apart, which is not a thing that exists there.
- `CPad::DisablePlayerControls` is at **0x10E**. This module read 0xF6, which
  lands inside `PCTempMouseState`, so every "the game says its controls are
  enabled" it logged was reading mouse bytes. That reading was the reason the
  game disabling the player's controls kept being ruled out.

Both were confirmed against data already collected before being trusted: the
node counts against the numbers walking each area's array had measured, and
the layout arithmetic against `field_EA4`, which names its own offset.

### The path graph

GTA keeps its roads and pavements as a graph of nodes in an 8x8 grid of
areas, loaded a few at a time around the player. The ped nodes are what an
NPC follows when it walks somewhere, and a route along them is what "natural"
means here.

The arrays are found the way the player pool was. Every node records which
area it belongs to, so the per-area pointer array is the one whose i-th entry
leads to nodes that say "i". The counts are the three 64-entry arrays where
one is the sum of the other two, and the total for every loaded area names an
array whose last node carries that index. The link table is the one whose
entries lead to nodes a walk away. The self-check is that the nearest ped
node to a player on a street is metres off, not hundreds.

### Planning

`plan_path` goes straight when the straight line is walkable - sampled a
metre at a time: ground the whole way, no step or drop bigger than a person
takes, nothing solid at knee or chest height. Otherwise it joins the start and
the target to the graph by walkable legs, runs A* over the ped nodes, and pulls
the route tight so he does not visit every node. Every leg is then checked
against the world again: the graph says the pavement is there, the world says
whether something is parked on it today. Legs the call budget could not reach
are marked unverified, never assumed.

The panel draws all of it in the world - the route, a fan of short walks
around the character's feet, the nodes on the pavements - because a number
says a leg is blocked and a red line on the ground says by what.

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
   and pings are all legitimately zero seconds after connecting, the vehicle
   pool is legitimately empty, and nobody is streamed in. This one cost three
   separate mistakes before it was learned.
4. Identify a field by something structural, not by how a server usually
   looks. "Most players are on exactly 100" found nothing, because two of six
   were. Health is stored in a float but arrives as a byte, so it is always a
   whole number - that holds whatever state the players are in.
5. How often the game is called matters more than whether the call is right.
   A reach fan making three hundred collision queries a second out of the
   frame hook took the player's input away and did not give it back; the same
   calls a few times a second, for an hour, did nothing. The call was correct
   the whole time - it passes a self-check against where the player is
   standing. Six mechanisms were reasoned out and all six were wrong, and what
   settled it was recording the state instead: which window had the focus,
   what mode the panel was in, whose cursor it was, what the game said about
   its own controls, and - the field that ended the argument - how far the
   player actually moved with the keys held.
6. A reader must not be stricter than the search that found the field for it.
   Insisting health be above zero discarded the offset the moment one player
   was dead, and discarded it for everyone else in the same pass.

## Status


Working: the frame hook, the game-thread bridge, the MCP server, the overlay,
the world reading and the chat log described above, and the standable /
walkable / route planning that reads the game's path graph and asks its
collision.

Not yet: making the character walk the route. That is input synthesis, and it
sits on top of everything above.

Not started: the RPC layer - chat, dialogs, and the values that only arrive in
packets - and every action. Actions answer with an explicit "not implemented
yet" rather than a false success.

Not covered by tests: with the server inside the game process, nothing can be
exercised without the game running. `tools/selftest.py` is the replacement and
needs a live session.
