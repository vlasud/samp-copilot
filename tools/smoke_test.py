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
import threading
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

    def tool(self, name, arguments=None):
        return self.call("tools/call",
                         {"name": name, "arguments": arguments or {}})["result"]

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
    read on the responder thread would wedge close() at teardown, since the
    server keeps its end of the pipe open.
    """

    def __init__(self, responder=None):
        self.f = None
        self.handle = None
        self.received = []
        self.responder = responder
        self._buf = b""
        self._stop = threading.Event()
        self._thread = None
        self._lock = threading.Lock()

    def connect(self, attempts=40):
        for _ in range(attempts):
            try:
                self.f = open(PIPE, "r+b", buffering=0)
                self.handle = msvcrt.get_osfhandle(self.f.fileno())
                self._thread = threading.Thread(target=self._pump, daemon=True)
                self._thread.start()
                return True
            except OSError:
                time.sleep(0.1)
        return False

    def _available(self):
        avail = ctypes.c_ulong(0)
        ok = kernel32.PeekNamedPipe(ctypes.c_void_p(self.handle), None, 0, None,
                                    ctypes.byref(avail), None)
        return avail.value if ok else 0

    def _pump(self):
        while not self._stop.is_set():
            n = self._available()
            if not n:
                time.sleep(0.01)
                continue
            self._buf += self.f.read(n)
            while bytes([10]) in self._buf:
                line, self._buf = self._buf.split(bytes([10]), 1)
                if not line:
                    continue
                env = json.loads(line)
                self.received.append(env)
                if env["type"] == "action" and self.responder:
                    self.send("result", self.responder(env), env["id"])

    def send(self, type_, payload, id_=0):
        env = {"v": 1, "type": type_, "id": id_,
               "ts": int(time.time() * 1000), "payload": payload}
        with self._lock:
            self.f.write((json.dumps(env) + chr(10)).encode("utf-8"))

    def close(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self.f:
            self.f.close()
            self.f = None


def respond(env):
    """Answers actions the way the real module would at this stage."""
    kind = env["payload"].get("kind")
    if kind == "probe_memory":
        return {"ok": True, "data": {"scan": {"scope": "samp.dll", "regions": 7},
                                     "search": {"needle": env["payload"].get("needle"),
                                                "found": 1}}}
    return {"ok": False, "error": "action not implemented yet: " + str(kind)}


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
    asi = FakeAsi(responder=respond)

    try:
        print("MCP handshake", flush=True)
        resp = mcp.call("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "smoke-test", "version": "0"}})
        info = resp.get("result", {}).get("serverInfo", {})
        r.check("initialize", info.get("name") == "gta-mcp", str(info))
        mcp.notify("notifications/initialized")

        names = sorted(t["name"] for t in mcp.call("tools/list")["result"]["tools"])
        r.check("tools/list", names == ["bot_status", "get_events", "get_snapshot",
                                        "probe_memory", "send_chat"], str(names))

        print("Before the game is attached", flush=True)
        r.check("bot_status works with no link",
                mcp.tool("bot_status")["structuredContent"]["link_up"] is False)
        r.check("actions refuse with no link",
                mcp.tool("send_chat", {"text": "/stats"})["isError"] is True)

        print("Pipe link", flush=True)
        if not r.check("fake asi connects", asi.connect()):
            return 1
        asi.send("hello", {"component": "fake-asi", "version": "test",
                           "samp": {"version": "0.3.7-R1"}})
        asi.send("event", {"kind": "chat", "text": "hello from the smoke test"})
        asi.send("snapshot", {"frame": {"hook_installed": True, "frames": 900,
                                        "fps": 60.0, "driver": "Present"}})
        time.sleep(0.6)

        print("Tools reflect the link", flush=True)
        status = mcp.tool("bot_status")["structuredContent"]
        r.check("bot_status.link_up", status["link_up"] is True, str(status))
        r.check("bot_status sees the handshake",
                bool(status["asi"]) and status["asi"]["component"] == "fake-asi")

        events = mcp.tool("get_events", {"limit": 10})["structuredContent"]
        r.check("get_events returns the chat line",
                len(events) == 1 and events[0]["payload"]["kind"] == "chat",
                str(events))

        snap = mcp.tool("get_snapshot")["structuredContent"]
        r.check("get_snapshot carries the frame stats",
                snap["snapshot"]["frame"]["driver"] == "Present", str(snap))

        print("Request and response are correlated", flush=True)
        result = mcp.tool("probe_memory", {"needle": "Ivan_Petrov"})
        r.check("probe_memory returns the game's answer",
                result["isError"] is False and
                result["structuredContent"]["search"]["needle"] == "Ivan_Petrov",
                str(result["structuredContent"]))

        actions = [m for m in asi.received if m["type"] == "action"]
        r.check("the action carried its id",
                len(actions) == 1 and actions[0]["id"] > 0, str(actions))

        print("A failing action surfaces as a tool error", flush=True)
        result = mcp.tool("send_chat", {"text": "/stats"})
        r.check("send_chat reports it is unimplemented",
                result["isError"] is True and
                "not implemented" in result["content"][0]["text"],
                str(result["content"]))

        print("Error paths", flush=True)
        r.check("empty chat text is a tool error",
                mcp.tool("send_chat", {"text": ""})["isError"] is True)
        r.check("unknown tool is a JSON-RPC error",
                "error" in mcp.call("tools/call", {"name": "nope",
                                                   "arguments": {}}))
        r.check("unknown method is a JSON-RPC error",
                "error" in mcp.call("does/not/exist"))
    finally:
        asi.close()
        mcp.close()

    print("", flush=True)
    print("FAILURES: " + str(r.failures), flush=True)
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
