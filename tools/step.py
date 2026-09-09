"""One call per decision.

The mod answers a `look` in about thirty milliseconds, so nothing about the
body is slow. What was slow was the brain's turn: reading files, listing
tools, writing a fresh script each time, sleeping inside it. A decision took
a minute and a half.

So a decision is one command now. It hands over what the mod is to do, waits
a moment, and prints the world back as a short page of text - small enough
to read at a glance and to think about quickly.

    python tools/step.py
    python tools/step.py '{"plan": {...}, "do": [{"tool": "...", "args": {}}]}'

The command is JSON:
    plan  {summary, steps, doing}  drawn on the game's panel for the human
    do    [{tool, args}, ...]      run in order, each answer printed
    wait  seconds before looking, at most three
"""
import json
import re
import sys
import time

sys.path.insert(0, "tools")
from mcp_http import Client

COLOUR = re.compile(r"\{[0-9A-Fa-f]{6}\}")


def plain(text, limit=90):
    """A server string with its colour codes and line breaks taken out."""
    text = COLOUR.sub("", str(text)).replace("\n", " / ").strip()
    return text if len(text) <= limit else text[: limit - 1] + "…"


def near(rows, how_many, line):
    """The closest few of something, already sorted by the mod."""
    rows = [r for r in rows if r.get("away_m", -1) >= 0]
    rows.sort(key=lambda r: r["away_m"])
    return [line(r) for r in rows[:how_many]]


def report(c):
    s = c.tool("look", {"radius": 30, "chat": 6})
    me = s.get("self") or {}
    act = c.tool("act_status")
    out = []

    pos = me.get("pos") or [0, 0, 0]
    out.append(
        "me: %s id=%s hp=%.0f/%.0f at %.1f,%.1f,%.1f facing %.0f deg %s"
        % (me.get("name"), me.get("id"), me.get("health", 0),
           me.get("max_health", 0), pos[0], pos[1], pos[2],
           (me.get("heading", 0) * 57.2958),
           "in a vehicle" if me.get("in_vehicle") else "on foot"))

    dialog = s.get("dialog") or {}
    if dialog.get("shown"):
        rows = dialog.get("rows") or []
        out.append("DIALOG IS OPEN - only answer_dialog works now")
        out.append("  title: %s" % plain(dialog.get("title", "")))
        for i, row in enumerate(rows[:12]):
            out.append("   [%d] %s" % (i, plain(row)))
        if len(rows) > 12:
            out.append("   ... %d more rows" % (len(rows) - 12))
    else:
        out.append("dialog: none open")

    cp = s.get("checkpoint") or c.tool("get_checkpoint")
    if isinstance(cp, dict) and cp.get("shown"):
        at = cp.get("at") or {}
        out.append("checkpoint: %.1f m away at %.0f,%.0f"
                   % (cp.get("away_m", 0), at.get("x", 0), at.get("y", 0)))

    travel = s.get("travel") or {}
    walk = travel.get("walk") or {}
    going = walk.get("note", "-")
    if walk.get("walking"):
        where = travel.get("destination") or [0, 0, 0]
        going += " %.0f m left to %.0f,%.0f" % (
            walk.get("remaining_m", 0), where[0], where[1])
    out.append("doing: %s%s | walking: %s"
               % (act.get("note", "idle"),
                  (" step %s of %s" % (act.get("at"), act.get("steps")))
                  if act.get("running") else "", going))

    npcs = s.get("npcs") or []
    if npcs:
        out.append("people near: " + "; ".join(near(
            npcs, 5,
            lambda r: "skin %s %.1fm stand_at %.0f,%.0f"
            % (r.get("skin"), r["away_m"],
               (r.get("stand_at") or {}).get("x", 0),
               (r.get("stand_at") or {}).get("y", 0)))))

    pickups = s.get("pickups") or []
    if pickups:
        out.append("pickups: " + "; ".join(near(
            pickups, 4,
            lambda r: "model %s %.0fm at %.0f,%.0f"
            % (r.get("model"), r["away_m"],
               (r.get("at") or {}).get("x", 0), (r.get("at") or {}).get("y", 0)))))

    doors = s.get("doors") or []
    if doors:
        out.append("doors: " + "; ".join(near(
            doors, 3, lambda r: "%.1fm at %.0f,%.0f"
            % (r["away_m"], (r.get("at") or {}).get("x", 0),
               (r.get("at") or {}).get("y", 0)))))

    # A label the server hung on a player or a car has no place of its own,
    # so it has no distance either. Those were being dropped by the sort,
    # and a shop's name is worth as much as a signpost's.
    labels = s.get("labels") or []
    # With their places, not just their distances. Without the coordinates a
    # sign is something the brain can see and cannot walk to, and it spends
    # its whole answer reasoning about where the thing might be instead of
    # going there - which is exactly what one of them did, at length, in
    # front of a row of free hospital beds.
    standing = near(labels, 4,
                    lambda r: "%.0f,%.0f (%.0fm) %s"
                    % ((r.get("at") or {}).get("x", 0), (r.get("at") or {}).get("y", 0),
                       r["away_m"], plain(r.get("text", ""), 60)))
    if standing:
        out.append("signs: " + " | ".join(standing))
    carried = [plain(r.get("text", ""), 40) for r in labels if r.get("away_m", -1) < 0]
    if carried:
        out.append("signs on people and cars: " + " | ".join(carried[:5]))

    out.append("chat:")
    for row in (s.get("chat") or [])[-6:]:
        out.append("  <%s> %s" % (row.get("kind", "?"), plain(row.get("text", ""), 110)))
    return "\n".join(out)


def main():
    c = Client(timeout=120)
    c.handshake("brain")
    order = {}
    if len(sys.argv) > 1 and sys.argv[1].strip():
        order = json.loads(sys.argv[1])

    plan = order.get("plan")
    if plan:
        c.tool("set_plan", plan)

    for one in order.get("do") or []:
        name = one.get("tool")
        answer = c.tool(name, one.get("args") or {})
        short = json.dumps(answer, ensure_ascii=False)
        print("%s -> %s" % (name, short if len(short) <= 300 else short[:299] + "…"))

    wait = float(order.get("wait", 0.6))
    time.sleep(max(0.0, min(3.0, wait)))
    print(report(c))


if __name__ == "__main__":
    main()
