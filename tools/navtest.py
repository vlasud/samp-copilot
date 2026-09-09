"""Walk somewhere and write down what actually happened.

The complaint is that the character wanders, and the way to settle that is
not to watch him but to measure him: how far it was in a straight line, how
far he actually walked, how long it took, and what the walker said it was
doing while it happened. A ratio near one is a person walking to a place; a
ratio near two is a person looking for it.

    python tools/navtest.py 1521 -1691 [seconds]
"""
import json
import math
import sys
import time

sys.path.insert(0, "tools")
from mcp_http import Client

TRACK = ("C:/Users/vlasud/AppData/Local/Temp/claude/D--bot/"
         "c3b0aefa-ee41-4adb-9355-6965408250cd/scratchpad/track.json")


def flat(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def main():
    to_x, to_y = float(sys.argv[1]), float(sys.argv[2])
    patience = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0

    c = Client(timeout=120)
    c.handshake("navtest")
    c.tool("set_movement", {"on": True})
    c.tool("act_stop")

    start = c.tool("look", {"radius": 1, "chat": 0})["self"]["pos"]
    straight = flat(start, (to_x, to_y))
    print("from %.0f,%.0f to %.0f,%.0f - %.0f m in a straight line"
          % (start[0], start[1], to_x, to_y, straight))

    said = c.tool("travel_to", {"x": to_x, "y": to_y})
    print("travel_to -> %s" % json.dumps(said, ensure_ascii=False)[:200])

    track = [[start[0], start[1], start[2], 0.0]]
    notes, walked, began = [], 0.0, time.time()
    arrived, why = False, "ran out of patience"

    while time.time() - began < patience:
        time.sleep(0.25)
        s = c.tool("look", {"radius": 1, "chat": 0})
        if "self" not in s:
            # The client stopped answering mid-walk: the anticheat kicks every
            # few minutes, and everything measured after that means nothing.
            why = "the client stopped answering: %s" % json.dumps(
                s, ensure_ascii=False)[:120]
            notes.append("%5.1f s  %s" % (time.time() - began, why))
            break
        here = s["self"]["pos"]
        gone = flat(track[-1], here)
        # A jump of more than a sprint in a quarter second is the game moving
        # him, not him walking: a respawn, a kick, a teleport by the server.
        if gone > 4.0:
            notes.append("jumped %.0f m at %.0f s" % (gone, time.time() - began))
        else:
            walked += gone
        track.append([here[0], here[1], here[2], round(time.time() - began, 2)])

        t = s.get("travel") or {}
        w = t.get("walk") or {}
        note = "%s / %s" % (t.get("note", "-"), w.get("note", "-"))
        if not notes or not notes[-1].endswith(note):
            notes.append("%5.1f s  %s" % (time.time() - began, note))
        if not t.get("travelling") and not w.get("walking"):
            arrived = flat(here, (to_x, to_y)) < 4.0
            why = t.get("note", "stopped")
            break

    end = track[-1]
    left = flat(end, (to_x, to_y))
    with open(TRACK, "w", encoding="utf-8") as f:
        json.dump({"start": start, "target": [to_x, to_y], "track": track,
                   "notes": notes}, f)

    print("\n".join(notes[-14:]))
    print("-" * 60)
    print("straight   %.0f m" % straight)
    print("walked     %.0f m   (%.2f times the straight line)"
          % (walked, walked / straight if straight > 1 else 0))
    print("took       %.0f s" % end[3])
    print("ended      %.0f m from the target - %s"
          % (left, "arrived" if arrived else "DID NOT ARRIVE: " + why))
    print("track      %d samples in %s" % (len(track), TRACK))


main()
