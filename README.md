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

Start the game. **F11** toggles the in-game panel (F8 is GTA's screenshot key), which shows the frame counter,
hook integrity, the SA-MP build, the MCP endpoint and the tail of the log -
everything that used to require alt-tabbing to a terminal.

To drive it from outside:

```
python tools/console.py     # interactive, stands in for the agent loop
python tools/selftest.py    # checks the endpoint end to end
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

## Status

Working: the frame hook, the game-thread bridge, validated read-only memory
access via `probe_memory`, the MCP server, and the overlay.

Stubs: the world collector (snapshots carry frame and module stats, not players
or vehicles) and every action - those answer with an explicit "not implemented
yet" rather than a false success.

Not covered by tests: with the server inside the game process, nothing can be
exercised without the game running. `tools/selftest.py` is the replacement and
needs a live session.
