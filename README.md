# gtabot

A bridge between an AI agent and a GTA San Andreas / SA-MP client.

Three pieces:

| Piece | What it is | Where it runs |
|---|---|---|
| `bot.asi` | x86 DLL, collects client state and performs actions | inside `gta_sa.exe` |
| named pipe | NDJSON, versioned, reconnects on its own | between the two |
| `gta-mcp.exe` | MCP server on stdio, owns the pipe and the collected data | its own process |

The MCP server owns the pipe rather than the mod, so restarting the game does
not drop the agent session or the data collected so far. Load order between the
two never matters: whichever starts second dials in.

## Build

`gta_sa.exe` is 32-bit, so everything here is x86 - the top-level `CMakeLists.txt`
refuses to configure otherwise.

```
cmake --preset x86
cmake --build --preset debug
```

Artifacts land in `build/x86/bin/<config>/`:
`bot.asi` and `gta-mcp.exe`.

Requires Visual Studio 2026 (or any MSVC with an x86 toolset) and CMake 3.25+.
`nlohmann/json` and `spdlog` are fetched at configure time.

## Test without the game

`tools/smoke_test.py` drives the MCP server exactly as an agent would while
impersonating `bot.asi` on the pipe. It covers the handshake, tool listing,
every tool, action delivery and the error paths.

```
python tools/smoke_test.py build/x86/bin/Debug/gta-mcp.exe
```

## Install

Copy `bot.asi` into the game folder (`D:\SAMP`). The ASI loader already
present there (`vorbisFile.dll`, with the original renamed to
`vorbisHooked.dll`) picks up any `*.asi` in that directory. The mod writes
`bot.asi.log` beside itself; read that first when something does not work.

Then run `gta-mcp.exe`. It is a stdio MCP server, so an agent launches it as a
subprocess rather than you starting it by hand.

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
src/common/   protocol.hpp   wire format, shared by both sides
              pipe.*         overlapped named-pipe transport
src/asi/      dllmain.cpp    entry point and worker thread
              samp/          SA-MP client version detection
              hooks/         (empty) frame, chat and dialog hooks
              state/         (empty) world snapshot collector
              actions/       (empty) action execution on the game thread
src/mcp/      server.*       JSON-RPC 2.0 and the tool registry
              state.*        in-memory cache of everything received
              main.cpp       tool definitions
tools/        smoke_test.py, fingerprint_samp.py
```

## Status

The transport works end to end. The collector and the action executor are
stubs: `bot.asi` sends a heartbeat snapshot and logs incoming actions without
performing them.
