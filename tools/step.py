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
import math
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


# What the world looked like the last time anybody asked. The brain's hardest
# question is not "where am I" but "did what I just did do anything at all" -
# it pressed a key at a bed and read back the same health at the same place,
# and could not tell a key that missed from a key that worked and a treatment
# that takes a while. So the change since the last look is spelt out.
_before = {}


def report(c):
    s = c.tool("look", {"radius": 40, "chat": 14})
    me = s.get("self") or {}
    act = c.tool("act_status")
    out = []

    pos = me.get("pos") or [0, 0, 0]

    was = _before.get("state")
    if was:
        moved = math.hypot(pos[0] - was["pos"][0], pos[1] - was["pos"][1])
        hp = me.get("health", 0) - was["health"]
        bits = ["moved %.1f m" % moved]
        if abs(hp) >= 0.5:
            bits.append("health %+.0f" % hp)
        else:
            bits.append("health unchanged")
        fresh = len([r for r in (s.get("chat") or [])
                     if r.get("text") not in was["chat"]])
        if fresh:
            bits.append("%d new line(s) in chat" % fresh)
        out.append("since your last look: " + ", ".join(bits))
    _before["state"] = {"pos": list(pos), "health": me.get("health", 0),
                        "chat": [r.get("text") for r in (s.get("chat") or [])]}

    out.append(
        "me: %s id=%s hp=%.0f/%.0f at %.1f,%.1f,%.1f facing %.0f deg %s"
        % (me.get("name"), me.get("id"), me.get("health", 0),
           me.get("max_health", 0), pos[0], pos[1], pos[2],
           (me.get("heading", 0) * 57.2958),
           "in a vehicle" if me.get("in_vehicle") else "on foot"))

    dialog = s.get("dialog") or {}
    if dialog.get("shown"):
        # The mod calls it a caption, and reading it as a title got an empty
        # line - so at the one moment that matters most, with a dialog in the
        # way of everything, the brain was shown nothing at all and had to
        # guess. A dialog with no rows still has its text, and that is where
        # a server writes the question.
        rows = dialog.get("rows") or []
        out.append("DIALOG IS OPEN - only answer_dialog works now")
        out.append("  caption: %s" % plain(dialog.get("caption", ""), 120))
        out.append("  style: %s" % dialog.get("style", "?"))
        for i, row in enumerate(rows[:14]):
            out.append("   [%d] %s" % (i, plain(row, 100)))
        if len(rows) > 14:
            out.append("   ... %d more rows" % (len(rows) - 14))
        if not rows:
            body = plain(dialog.get("text", ""), 600)
            if body:
                out.append("  text: %s" % body)
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
        # Said out loud because it was not obvious: a journey under way is
        # not an obligation. He ran to the town hall through the very people
        # he had been sent to beg from, because the route was still running
        # and finishing it looked like the job.
        going += " - you can end it any turn with stop"
    # A chain that has stopped says why, in the words the notes use -
    # stopped_by: done, dialog, spoken_to, hurt, blocked, cancelled - so the
    # brain reads on the page exactly what it was taught to look for.
    ended = ""
    if not act.get("running") and act.get("stopped_by"):
        ended = " stopped_by: %s" % act["stopped_by"]
    out.append("doing: %s%s%s%s | walking: %s"
               % (act.get("note", "idle"),
                  (" step %s of %s" % (act.get("at"), act.get("steps")))
                  if act.get("running") else "", ended,
                  # Still saying the last thing. Worth a word on the page as
                  # well as a wait in the loop: a sentence asked for is not a
                  # sentence said.
                  " (STILL TYPING - your last line is not finished)"
                  if act.get("typing") else "", going))

    # People, and which of them are people.
    #
    # A player and a shopkeeper look the same in a list of peds, and the
    # difference is most of what a character needs: one can be spoken to,
    # asked the way, avoided; the other sells things and stands still. They
    # were not even both here - players were dropped before they reached the
    # page at all.
    players = s.get("players") or []
    if players:
        out.append("PLAYERS near (real people - you may talk to them): " + "; ".join(near(
            players, 8,
            # With their places. Without them a player is somebody the brain
            # can see and cannot walk to: asked to go and talk to Revor it
            # sent the character to his own position, because that was the
            # only pair of coordinates on the page.
            lambda r: "%s[%s] at %.0f,%.0f (%.0fm)%s%s%s%s"
            % (r.get("name", "?"), r.get("id", "?"),
               (r.get("at") or {}).get("x", 0), (r.get("at") or {}).get("y", 0),
               r["away_m"],
               " hp%.0f" % r["health"] if r.get("health") is not None else "",
               " in a car" if r.get("in_vehicle") else "",
               " **%s**" % r["standing"] if r.get("standing") else "",
               " (has spoken to you)" if r.get("has_spoken_to_me") else ""))))
    else:
        out.append("PLAYERS near: none")
    # The "none" above belongs to having no players at all. It had been
    # hanging off this next test instead, so a page that listed six people by
    # name went on to say there was nobody about.
    bad = [r for r in players if r.get("standing") in ("enemy", "wary")]
    if bad:
        out.append("KEEP AWAY: " + "; ".join(
            "%s %.0fm - %s" % (r.get("name", "?"), r.get("away_m", 0),
                               r.get("why", "no reason recorded"))
            for r in bad[:4]))

    npcs = s.get("npcs") or []
    if npcs:
        out.append("NPCs near (server characters, not people): " + "; ".join(near(
            npcs, 8,
            lambda r: "skin %s %.1fm stand_at %.0f,%.0f"
            % (r.get("skin"), r["away_m"],
               (r.get("stand_at") or {}).get("x", 0),
               (r.get("stand_at") or {}).get("y", 0)))))

    # A pickup on the ground at a door is how a character gets inside; a door
    # itself is mostly scenery. So they are worth their place on the page.
    pickups = s.get("pickups") or []
    if pickups:
        out.append("pickups (standing on one is how you enter a place): " + "; ".join(near(
            pickups, 8,
            lambda r: "model %s %.0fm at %.0f,%.0f"
            % (r.get("model"), r["away_m"],
               (r.get("at") or {}).get("x", 0), (r.get("at") or {}).get("y", 0)))))

    doors = s.get("doors") or []
    if doors:
        out.append("doors: " + "; ".join(near(
            doors, 6, lambda r: "%.1fm at %.0f,%.0f"
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
    standing = near(labels, 10,
                    lambda r: "%.0f,%.0f (%.0fm) %s"
                    % ((r.get("at") or {}).get("x", 0), (r.get("at") or {}).get("y", 0),
                       r["away_m"], plain(r.get("text", ""), 60)))
    if standing:
        out.append("signs: " + " | ".join(standing))
    carried = [plain(r.get("text", ""), 60) for r in labels if r.get("away_m", -1) < 0]
    if carried:
        out.append("signs on people and cars: " + " | ".join(carried[:8]))

    # What the server has written on the screen. Its prompts say which key
    # opens the thing in front of him, and the money and the hunger bar are
    # here as well; none of it reaches the chat.
    try:
        drawn = c.tool("get_textdraws", {"limit": 40}).get("textdraws") or []
    except Exception:
        drawn = []
    # Not only the ones addressed to this player: on this server almost
    # nothing is, and the money and the hunger bar are drawn for everybody.
    mine = [plain(d.get("text", ""), 70) for d in drawn]
    mine = [t for t in mine if len(t) > 2][:8]
    if mine:
        out.append("on screen: " + " | ".join(mine))

    # The cars near him. He is meant to drive, and a world with no vehicles
    # in it is one he will always cross on foot.
    try:
        world = (c.tool("get_world") or {}).get("world") or {}
    except Exception:
        world = {}
    cars = []
    for v in world.get("vehicles") or []:
        at = v.get("pos") or []
        if len(at) != 3:
            continue
        away = math.hypot(at[0] - pos[0], at[1] - pos[1])
        if away > 60:
            continue
        cars.append((away, "%s %.0fm at %.0f,%.0f%s"
                     % (v.get("model_name") or ("model %s" % v.get("model", "?")),
                        away, at[0], at[1],
                        " (someone in it)" if v.get("occupied") else "")))
    cars.sort()
    if cars:
        out.append("vehicles near: " + "; ".join(t for _, t in cars[:8]))

    out.append("chat (newest last):")
    # A line meant for this character, and a line from an administrator, are
    # the two that cannot wait - and they were arriving indistinguishable
    # from the shop adverts that fill the rest of the log. The mod marks
    # both; the page had been hiding the marks.
    said = s.get("chat") or []
    for_him = [r for r in said if r.get("to_me")]
    admins = [r for r in said if r.get("kind") == "admin"]
    if admins:
        out.append("AN ADMINISTRATOR IS SPEAKING - answer at once, and never "
                   "claim to be a person: " +
                   " | ".join(plain(r.get("text", ""), 140) for r in admins[-3:]))
    if for_him:
        out.append("SOMEBODY IS TALKING TO YOU - answer in Russian, in "
                   "character: " +
                   " | ".join("%s: %s" % (r.get("speaker", "?"),
                                          plain(r.get("text", ""), 140))
                              for r in for_him[-3:]))
    if not said:
        out.append("  (nothing said lately)")
    for row in said[-14:]:
        who = row.get("speaker")
        out.append("  %s<%s%s> %s"
                   % (">>> " if row.get("to_me") else "",
                      row.get("kind", "?"), (" " + who) if who else "",
                      plain(row.get("text", ""), 160)))
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
