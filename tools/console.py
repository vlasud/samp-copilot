"""Interactive console for the MCP endpoint bot.asi serves - a stand-in for the
agent loop.

The game must be running with bot.asi loaded; there is no separate server
process to start.

    python tools/console.py [http://127.0.0.1:8765/mcp]

Commands:
    status              hooked or not, SA-MP build, and the verdict when the
                        frame counter is not moving
    world               latest world state and how stale it is
    probe [text]        search samp.dll for text; with no text, the nickname
                        from the launcher command line
    scan <text>         search the whole process (stutters the game once)
    chat <text>         send a line to the server chat
    watch [seconds]     print the frame counter once a second
    tools               list what the agent can call
    quit
"""
import json
import sys
import time

from mcp_http import Client, NotRunning


def show(value):
    print(json.dumps(value, indent=2, ensure_ascii=False), flush=True)


def watch(client, seconds):
    """Prints the frame counter each second.

    A climbing counter means the hook is executing on the game thread. A frozen
    one is ambiguous on its own, so the patch-integrity check and the module's
    own verdict are printed alongside it.
    """
    previous = None
    last_verdict = None
    for _ in range(seconds):
        status = client.tool("bot_status")
        frame = status.get("frame", {})
        hook = status.get("hook", {})
        frames = frame.get("frames")
        delta = "" if previous is None or frames is None else (
            "  (+%d)" % (frames - previous))
        previous = frames
        print("  frames=%s fps=%.1f idle=%sms driver=%s intact=%s%s" % (
            frames, frame.get("fps", 0.0), frame.get("idle_ms"),
            frame.get("driver"), hook.get("present_intact"), delta), flush=True)
        verdict = status.get("verdict")
        if verdict and verdict != last_verdict:
            print("    verdict: " + str(verdict), flush=True)
            last_verdict = verdict
        time.sleep(1)


def run_command(client, line):
    parts = line.split(" ", 1)
    command = parts[0]
    argument = parts[1].strip() if len(parts) > 1 else ""

    if command == "status":
        show(client.tool("bot_status"))
    elif command == "world":
        show(client.tool("get_world"))
    elif command == "probe":
        show(client.tool("probe_memory", {"needle": argument} if argument else {}))
    elif command == "scan":
        if not argument:
            print("  scan needs text to look for", flush=True)
            return
        print("  sweeping the whole process, this takes a moment...", flush=True)
        show(client.tool("probe_memory", {"needle": argument, "scope": "process"}))
    elif command == "chat":
        show(client.tool("send_chat", {"text": argument}))
    elif command == "watch":
        watch(client, int(argument or 10))
    elif command == "tools":
        for tool in client.tools():
            print("  %-14s %s" % (tool["name"], tool["description"].split(".")[0]),
                  flush=True)
    else:
        print("  unknown command; see the top of this file", flush=True)


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else None
    client = Client(url) if url else Client()

    try:
        info = client.handshake("console")
        print("connected to %s %s at %s" % (info["serverInfo"]["name"],
                                            info["serverInfo"]["version"],
                                            client.url), flush=True)
    except NotRunning as e:
        print(str(e), flush=True)
        return 1

    print("type a command, or 'quit'", flush=True)
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        if line in ("quit", "exit"):
            break
        try:
            run_command(client, line)
        except NotRunning as e:
            print("  " + str(e), flush=True)
        except Exception as e:
            print("  " + type(e).__name__ + ": " + str(e), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
