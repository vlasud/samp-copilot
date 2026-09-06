"""End-to-end smoke test for gta-mcp.exe.

Drives the MCP server over stdio the way an agent would, while impersonating
bot.asi on the named pipe. Exercises the whole loop without launching the game.

    python tools/smoke_test.py build/x86/bin/Debug/gta-mcp.exe
"""
import ctypes
import json
import msvcrt
import subprocess
import sys
import time

PIPE = r"\\.\pipe\gtabot"

kernel32 = ctypes.windll.kernel32


class Mcp:
    """The agent side: JSON-RPC 2.0 over the child's stdin/stdout."""

    def __init__(self, exe):
        self.p = subprocess.Popen(
            [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self._id = 0

    def call(self, method, params=None):
        self._id += 1
        msg = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("server closed the transport")
        return json.loads(line)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)

    def _write(self, msg):
        self.p.stdin.write(json.dumps(msg) + chr(10))
        self.p.stdin.flush()

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            print("  [WARN] server did not exit on EOF; killing", flush=True)
            self.p.kill()


class FakeAsi:
    """Stands in for the in-game module: dials the pipe, talks the protocol.

    Reads are peeked before they are issued, so no call ever blocks. A blocking
    read on a background thread would wedge close() at teardown, since the
    server keeps its end of the pipe open.
    """

    def __init__(self):
        self.f = None
        self.handle = None
        self.received = []
        self._buf = b""

    def connect(self, attempts=40):
        for _ in range(attempts):
            try:
                self.f = open(PIPE, "r+b", buffering=0)
                self.handle = msvcrt.get_osfhandle(self.f.fileno())
                return True
            except OSError:
                time.sleep(0.1)
        return False

    def _available(self):
        avail = ctypes.c_ulong(0)
        ok = kernel32.PeekNamedPipe(ctypes.c_void_p(self.handle), None, 0, None,
                                    ctypes.byref(avail), None)
        return avail.value if ok else 0

    def drain(self, settle=0.4):
        """Collects whatever the server has sent by now."""
        time.sleep(settle)
        n = self._available()
        if n:
            self._buf += self.f.read(n)
        while b"" + bytes([10]) in self._buf:
            line, self._buf = self._buf.split(bytes([10]), 1)
            if line:
                self.received.append(json.loads(line))

    def send(self, type_, payload):
        env = {"v": 1, "type": type_, "id": 0,
               "ts": int(time.time() * 1000), "payload": payload}
        self.f.write((json.dumps(env) + chr(10)).encode("utf-8"))

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


class Report:
    def __init__(self):
        self.failures = 0

    def check(self, label, ok, detail=""):
        if not ok:
            self.failures += 1
        mark = "PASS" if ok else "FAIL"
        suffix = " - " + detail if detail else ""
        print("  [" + mark + "] " + label + suffix, flush=True)
        return ok


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else r"build\x86\bin\Debug\gta-mcp.exe"
    r = Report()
    mcp = Mcp(exe)
    asi = FakeAsi()

    try:
        print("MCP handshake", flush=True)
        resp = mcp.call("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "smoke-test", "version": "0"}})
        info = resp.get("result", {}).get("serverInfo", {})
        r.check("initialize", info.get("name") == "gta-mcp", str(info))
        mcp.notify("notifications/initialized")

        resp = mcp.call("tools/list")
        names = sorted(t["name"] for t in resp["result"]["tools"])
        r.check("tools/list", names == ["bot_status", "get_events",
                                        "get_snapshot", "send_chat"], str(names))

        print("Pipe link", flush=True)
        if not r.check("fake asi connects", asi.connect()):
            return 1
        asi.send("hello", {"component": "fake-asi", "version": "test",
                           "samp": {"version": "0.3.7-R1"}})
        asi.send("event", {"kind": "chat", "text": "hello from the smoke test"})
        asi.send("snapshot", {"stub": True, "tick": 1234})
        time.sleep(0.6)

        print("Tools reflect the link", flush=True)
        status = mcp.call("tools/call", {"name": "bot_status",
                                         "arguments": {}})["result"]["structuredContent"]
        r.check("bot_status.link_up", status["link_up"] is True, str(status))
        r.check("bot_status sees the handshake",
                bool(status["asi"]) and status["asi"]["component"] == "fake-asi")

        events = mcp.call("tools/call", {"name": "get_events",
                                         "arguments": {"limit": 10}})["result"]["structuredContent"]
        r.check("get_events returns the chat line",
                len(events) == 1 and events[0]["payload"]["kind"] == "chat",
                str(events))

        snap = mcp.call("tools/call", {"name": "get_snapshot",
                                       "arguments": {}})["result"]["structuredContent"]
        r.check("get_snapshot returns the snapshot",
                snap["snapshot"] == {"stub": True, "tick": 1234}, str(snap))

        print("Actions reach the game side", flush=True)
        result = mcp.call("tools/call", {"name": "send_chat",
                                         "arguments": {"text": "/stats"}})["result"]
        r.check("send_chat queued", result["isError"] is False, str(result))
        asi.drain()
        actions = [m for m in asi.received if m["type"] == "action"]
        r.check("asi received the action",
                len(actions) == 1 and actions[0]["payload"]["text"] == "/stats",
                str(actions))

        print("Error paths", flush=True)
        result = mcp.call("tools/call", {"name": "send_chat",
                                         "arguments": {"text": ""}})["result"]
        r.check("empty chat text is a tool error", result["isError"] is True)
        resp = mcp.call("tools/call", {"name": "nope", "arguments": {}})
        r.check("unknown tool is a JSON-RPC error", "error" in resp, str(resp))
        resp = mcp.call("does/not/exist")
        r.check("unknown method is a JSON-RPC error", "error" in resp, str(resp))
    finally:
        asi.close()
        mcp.close()

    print("", flush=True)
    print("FAILURES: " + str(r.failures), flush=True)
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
