"""Interactive console for the MCP endpoint bot.asi serves - a stand-in for the
agent loop.

The game must be running with bot.asi loaded; there is no separate server
process to start.

    python tools/console.py [http://127.0.0.1:8765/mcp]

Commands:
    status              hooked or not, SA-MP build, and the verdict when the
                        frame counter is not moving
    world               summary of the world: self, counts, staleness
    players [n]         the first n players in the pool, in full
    near [n]            the n nearest streamed players, with distances
    cars [n]            the n nearest vehicles, with distances
    reach x y [z]       can the character stand there, and walk there straight
    path x y [z]        plan a route there; also drawn in the game
    path ahead [m]      plan a route m metres ahead of where he is facing
    nodes [radius]      the game's ped nodes around him
    chat [n]            the last n lines of the in-game chat
    chatdump            write bot.chat-dump.txt, for correcting a column the
                        shape search labelled wrongly
    probe [text]        search samp.dll for text; with no text, the nickname
                        from the launcher command line
    scan <text>         search the whole process (stutters the game once)
    chat <text>         send a line to the server chat
    watch [seconds]     print the frame counter once a second
    tools               list what the agent can call
    quit
"""
import json
import math
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
        # Six hundred players is not something to read in a terminal; the
        # summary is what a person wants and `players` is what a machine does.
        result = client.tool("get_world")
        world = result.get("world", {})
        summary = {k: v for k, v in world.items() if k != "players"}
        summary["world_age_ms"] = result.get("world_age_ms")
        show(summary)
    elif command == "near":
        # The point of positions: not 650 names, but who is actually around.
        world = client.tool("get_world").get("world", {})
        self_pos = (world.get("self") or {}).get("pos")
        if not self_pos:
            print("  no position for the local player yet", flush=True)
            return
        rows = []
        for player in world.get("players", []):
            pos = player.get("pos")
            if not pos:
                continue
            distance = sum((a - b) ** 2 for a, b in zip(pos, self_pos)) ** 0.5
            rows.append((distance, player))
        rows.sort(key=lambda row: row[0])
        for distance, player in rows[:int(argument or 10)]:
            marks = []
            if player.get("in_vehicle"):
                marks.append("in a vehicle")
            if player.get("health") is not None:
                marks.append("hp %d" % player["health"])
            if player.get("armour"):
                marks.append("armour %d" % player["armour"])
            # Weapon 0 is a fist, not an absent value - "if weapon" hid it.
            if player.get("weapon_name") is not None:
                marks.append(player["weapon_name"])
            if player.get("state") == 32:
                marks.append("wasted")
            print("  %6.1f m  id %-4d %-22s %s" % (
                distance, player["id"], player["name"],
                ", ".join(marks)), flush=True)
        print("  (%d streamed of %d in the pool)" % (
            len(rows), world.get("player_count", 0)), flush=True)
    elif command == "cars":
        world = client.tool("get_world").get("world", {})
        self_pos = (world.get("self") or {}).get("pos")
        if not self_pos:
            print("  no position for the local player yet", flush=True)
            return
        rows = []
        for car in world.get("vehicles", []):
            distance = sum((a - b) ** 2
                           for a, b in zip(car["pos"], self_pos)) ** 0.5
            rows.append((distance, car))
        rows.sort(key=lambda row: row[0])
        for distance, car in rows[:int(argument or 10)]:
            print("  %6.1f m  id %-5d model %s" % (
                distance, car["id"], car.get("model", "?")), flush=True)
        print("  (%d vehicles streamed)" % len(rows), flush=True)
    elif command == "reach":
        parts = argument.split()
        if len(parts) < 2:
            print("  reach needs x y", flush=True)
            return
        args = {"x": float(parts[0]), "y": float(parts[1])}
        if len(parts) > 2:
            args["z"] = float(parts[2])
        show(client.tool("check_point", args))
    elif command == "path":
        parts = argument.split()
        if parts and parts[0] == "ahead":
            metres = float(parts[1]) if len(parts) > 1 else 15.0
            world = client.tool("get_world").get("world", {})
            me = world.get("self") or {}
            pos, heading = me.get("pos"), me.get("heading")
            if not pos or heading is None:
                print("  no position or heading for the local player", flush=True)
                return
            args = {"x": pos[0] + math.cos(heading) * metres,
                    "y": pos[1] + math.sin(heading) * metres, "z": pos[2]}
        elif len(parts) >= 2:
            args = {"x": float(parts[0]), "y": float(parts[1])}
            if len(parts) > 2:
                args["z"] = float(parts[2])
        else:
            print("  path needs x y, or ahead [metres]", flush=True)
            return
        plan = client.tool("plan_path", args)
        print("  %s: %s (%.1f m, %d game calls)" % (
            "ok" if plan.get("ok") else "NO", plan.get("note"),
            plan.get("length_m", 0), plan.get("game_calls", 0)), flush=True)
        for i, leg in enumerate(plan.get("legs", [])):
            to = leg["to"]
            mark = "ok" if leg["ok"] else "BLOCKED"
            if not leg.get("verified"):
                mark = "unverified"
            print("  leg %d -> (%.1f, %.1f, %.1f)  %s%s" % (
                i + 1, to[0], to[1], to[2], mark,
                ("  " + leg["why"]) if leg.get("why") else ""), flush=True)
    elif command == "nodes":
        result = client.tool("get_nav_nodes",
                             {"radius": float(argument or 60)})
        if not result.get("graph"):
            print("  " + str(result.get("note")), flush=True)
            return
        world = client.tool("get_world").get("world", {})
        me = (world.get("self") or {}).get("pos") or [0, 0, 0]
        for node in result.get("nodes", []):
            p = node["pos"]
            d = ((p[0] - me[0]) ** 2 + (p[1] - me[1]) ** 2) ** 0.5
            print("  %6.1f m  area %-2d node %-5d (%.1f, %.1f, %.1f)  %d links" % (
                d, node["area"], node["index"], p[0], p[1], p[2],
                len(node["links"])), flush=True)
        print("  (%d nodes, %d areas loaded, %d ped nodes in them)" % (
            result.get("count", 0), result.get("areas_loaded", 0),
            result.get("ped_nodes_loaded", 0)), flush=True)
    elif command == "chat":
        result = client.tool("get_chat", {"limit": int(argument or 25)})
        if not result.get("valid"):
            print("  " + str(result.get("note")), flush=True)
            return
        for line in result.get("lines", []):
            age = line.get("age_ms")
            when = "%5.0fs" % (age / 1000.0) if age is not None else "     ?"
            who = line.get("from")
            print("  %s  %s%s" % (when, (who + "  ") if who else "",
                                  line.get("text", "")), flush=True)
        print("  (%d lines held, order: %s, from %s)" % (
            result.get("count", 0), result.get("order"),
            result.get("source")), flush=True)
    elif command == "chatdump":
        show(client.tool("dump_chat"))
    elif command == "players":
        world = client.tool("get_world").get("world", {})
        limit = int(argument or 15)
        show(world.get("players", [])[:limit])
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
