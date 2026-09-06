"""Interactive console for gta-mcp.exe - a stand-in for the agent loop.

Launches the MCP server, performs the handshake, then reads commands. Use it to
poke at the running game by hand before there is an agent to do it.

    python tools/console.py build/x86/bin/Debug/gta-mcp.exe

Commands:
    status              is the module attached, what did it find
    snap                latest world snapshot (frame counter, fps, samp)
    events [n]          recent in-game events
    probe [text]        search samp.dll for text; with no text, the nickname
                        from the launcher command line
    scan <text>         search the whole process (stutters the game once)
    chat <text>         send a line to the server chat
    watch [seconds]     print the frame counter once a second
    quit
"""
import json
import os
import subprocess
import sys
import threading
import time


class Mcp:
    def __init__(self, exe):
        # CreateProcess rejects a relative path written with forward
        # slashes, so the caller gets to type it either way.
        self.p = subprocess.Popen(
            [os.path.abspath(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self._id = 0
        threading.Thread(target=self._drain_stderr, daemon=True).start()

    def _drain_stderr(self):
        for line in self.p.stderr:
            print("  server| " + line.rstrip(), flush=True)

    def call(self, method, params=None):
        self._id += 1
        msg = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg) + chr(10))
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("server closed the transport")
        return json.loads(line)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg) + chr(10))
        self.p.stdin.flush()

    def tool(self, name, arguments=None):
        resp = self.call("tools/call", {"name": name,
                                        "arguments": arguments or {}})
        if "error" in resp:
            return {"transport_error": resp["error"]}
        result = resp["result"]
        if result.get("isError"):
            return {"error": result["content"][0]["text"]}
        return result.get("structuredContent", result)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def show(value):
    print(json.dumps(value, indent=2, ensure_ascii=False), flush=True)


def watch(mcp, seconds):
    """Prints the frame counter each second, which is the liveness signal for
    the hook: if it climbs, we are executing on the game thread."""
    previous = None
    for _ in range(seconds):
        snap = mcp.tool("get_snapshot").get("snapshot") or {}
        frame = snap.get("frame", {})
        frames = frame.get("frames")
        delta = "" if previous is None or frames is None else (
            "  (+%d)" % (frames - previous))
        previous = frames
        print("  frames=%s fps=%.1f driver=%s hook=%s%s" % (
            frames, frame.get("fps", 0.0), frame.get("driver"),
            frame.get("hook_installed"), delta), flush=True)
        time.sleep(1)


def run_command(mcp, line):
    parts = line.split(" ", 1)
    command = parts[0]
    argument = parts[1].strip() if len(parts) > 1 else ""

    if command == "status":
        show(mcp.tool("bot_status"))
    elif command == "snap":
        show(mcp.tool("get_snapshot"))
    elif command == "events":
        show(mcp.tool("get_events", {"limit": int(argument or 20)}))
    elif command == "probe":
        show(mcp.tool("probe_memory",
                      {"needle": argument} if argument else {}))
    elif command == "scan":
        if not argument:
            print("  scan needs text to look for", flush=True)
            return
        print("  sweeping the whole process, this takes a moment...", flush=True)
        show(mcp.tool("probe_memory", {"needle": argument, "scope": "process"}))
    elif command == "chat":
        show(mcp.tool("send_chat", {"text": argument}))
    elif command == "watch":
        watch(mcp, int(argument or 10))
    else:
        print("  unknown command; see the top of this file", flush=True)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else r"build\x86\bin\Debug\gta-mcp.exe"
    mcp = Mcp(exe)
    try:
        resp = mcp.call("initialize", {"protocolVersion": "2025-06-18",
                                       "capabilities": {},
                                       "clientInfo": {"name": "console",
                                                      "version": "0"}})
        mcp.notify("notifications/initialized")
        info = resp["result"]["serverInfo"]
        print("connected to %s %s" % (info["name"], info["version"]), flush=True)
        print("waiting for bot.asi - start the game whenever you like", flush=True)
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
                run_command(mcp, line)
            except Exception as e:
                print("  " + type(e).__name__ + ": " + str(e), flush=True)
    finally:
        mcp.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
