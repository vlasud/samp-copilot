"""Checks the MCP endpoint bot.asi serves, against the running game.

Replaces the old offline smoke test: with the server living inside the game
process there is nothing left to exercise without the game up. Run it with the
game running and the mod loaded.

    python tools/selftest.py [http://127.0.0.1:8765/mcp]
"""
import sys

from mcp_http import Client, NotRunning

EXPECTED_TOOLS = ["bot_status", "get_world", "probe_memory", "send_chat"]


class Report:
    def __init__(self):
        self.failures = 0

    def check(self, label, ok, detail=""):
        if not ok:
            self.failures += 1
        suffix = " - " + str(detail) if detail else ""
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label, suffix), flush=True)
        return ok


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else None
    client = Client(url) if url else Client()
    r = Report()

    print("Handshake", flush=True)
    try:
        info = client.handshake("selftest")
    except NotRunning as e:
        print("  " + str(e), flush=True)
        return 1
    r.check("initialize", info["serverInfo"]["name"] == "gtabot",
            info["serverInfo"])
    r.check("tools/list", sorted(t["name"] for t in client.tools()) ==
            EXPECTED_TOOLS)

    print("Status", flush=True)
    status = client.tool("bot_status")
    r.check("hook installed", status["frame"]["hook_installed"] is True, status.get("verdict"))
    r.check("patch bytes intact", status["hook"]["present_intact"] is True or
            status["hook"]["endscene_intact"] is True, status.get("hook"))
    rendering = status["frame"]["idle_ms"] < 2000
    r.check("game is rendering", rendering,
            "idle_ms=%s - alt-tab into the game and rerun" % status["frame"]["idle_ms"])
    r.check("SA-MP recognised", status["samp"]["loaded"] and
            status["samp"]["version"] != "unknown", status.get("samp"))

    if not rendering:
        print("\nThe game thread is parked, so the tools below cannot run.",
              flush=True)
        print("FAILURES: " + str(r.failures), flush=True)
        return 1 if r.failures else 0

    print("Memory access", flush=True)
    probe = client.tool("probe_memory")
    if "error" in probe:
        r.check("probe_memory ran", False, probe["error"])
    else:
        r.check("probe_memory ran", True)
        r.check("samp.dll located", probe["modules"]["samp.dll"] is not None)
        needle = probe["search"].get("needle")
        found = probe["search"].get("found", 0)
        r.check("a needle was available", bool(needle),
                "no -n on the command line; pass one by hand")
        r.check("the needle was found in samp.dll memory", found > 0,
                "needle=%r found=%s" % (needle, found))

    print("Unimplemented actions fail loudly", flush=True)
    r.check("send_chat says so",
            "not implemented" in str(client.tool("send_chat", {"text": "/x"})))

    print("", flush=True)
    print("FAILURES: " + str(r.failures), flush=True)
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
