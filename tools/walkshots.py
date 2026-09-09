"""Walk somewhere and photograph what happens, every few seconds.

    python tools/walkshots.py 1700 -1700 [seconds] [every]

The numbers say what happened; the pictures say why. Each shot is saved
beside the last with the position, the height and what the walk said it was
doing at that moment, so a run can be read back frame by frame afterwards.
"""
import json
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from mcp_http import Client

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = os.path.join(HERE, "shot.ps1")
INTO = os.environ.get("WALKSHOTS_DIR") or os.path.join(HERE, "..", "shots")


def flat(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def shoot(name):
    try:
        subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", SHOT, name],
                       capture_output=True, timeout=25)
        return os.path.exists(name)
    except Exception:
        return False


def main():
    to_x, to_y = float(sys.argv[1]), float(sys.argv[2])
    patience = float(sys.argv[3]) if len(sys.argv) > 3 else 240.0
    every = float(sys.argv[4]) if len(sys.argv) > 4 else 20.0

    os.makedirs(INTO, exist_ok=True)
    for old in os.listdir(INTO):
        if old.endswith(".png"):
            os.remove(os.path.join(INTO, old))

    c = Client(timeout=120)
    c.handshake("walkshots")
    c.tool("set_movement", {"on": True})
    c.tool("act_stop")
    start = c.tool("look", {"radius": 1, "chat": 0})["self"]["pos"]
    straight = flat(start, (to_x, to_y))
    print("from %.0f,%.0f to %.0f,%.0f - %.0f m in a straight line"
          % (start[0], start[1], to_x, to_y, straight))
    c.tool("travel_to", {"x": to_x, "y": to_y})

    began, last_shot = time.time(), 0.0
    walked, last, shots = 0.0, start, []
    while time.time() - began < patience:
        time.sleep(0.5)
        s = c.tool("look", {"radius": 1, "chat": 0})
        if "self" not in s:
            print("the client stopped answering")
            break
        here = s["self"]["pos"]
        gone = flat(last, here)
        if gone <= 4.0:
            walked += gone
        last = here
        now = time.time() - began
        if now - last_shot >= every:
            last_shot = now
            t = s.get("travel") or {}
            w = t.get("walk") or {}
            name = os.path.join(INTO, "%03ds.png" % int(now))
            note = "%s / %s" % (t.get("note", "-"), w.get("note", "-"))
            if shoot(name):
                shots.append((int(now), here, note))
                print("%3ds  %.0f,%.0f z%.1f  %s" % (int(now), here[0], here[1], here[2], note[:90]))
                sys.stdout.flush()
        t = s.get("travel") or {}
        if not t.get("travelling") and not (t.get("walk") or {}).get("walking"):
            print("stopped: %s" % t.get("note", "-"))
            break

    left = flat(last, (to_x, to_y))
    print("-" * 60)
    print("straight %.0f m, walked %.0f m (%.2fx), %.0f s, ended %.0f m away"
          % (straight, walked, walked / straight if straight > 1 else 0,
             time.time() - began, left))
    print("shots in %s" % os.path.abspath(INTO))


main()
