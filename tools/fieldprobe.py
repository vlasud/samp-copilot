"""Where the field disagrees with the pavement graph, and what is there.

    python tools/fieldprobe.py 1550 -1700

The graph's route out of here is asked for, sampled every half metre, and
each sample is put to the field: the first samples the field calls shut are
where it has painted a wall across a way the graph walks. For each of those
the collision module writes to the log which entity is under the point, so
the thing can be named rather than guessed at.
"""
import math
import sys
import time

sys.path.insert(0, "tools")
from mcp_http import Client


def main():
    to_x, to_y = float(sys.argv[1]), float(sys.argv[2])
    c = Client(timeout=120)
    c.handshake("fieldprobe")

    # The graph's way, from the journey planner which runs it in full.
    p = c.tool("plan_path", {"x": to_x, "y": to_y})
    legs = p.get("legs") or []
    if not legs:
        print("the graph has no route: %s" % (p.get("note") or "")[:120])
        return
    print("graph route: %d legs - %s" % (len(legs), (p.get("note") or "")[:100]))
    samples = []
    for leg in legs:
        a, b = leg.get("from"), leg.get("to")
        if not a or not b:
            continue
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        n = max(1, int(d / 0.5))
        for i in range(n + 1):
            t = i / n
            samples.append({"x": a[0] + (b[0] - a[0]) * t, "y": a[1] + (b[1] - a[1]) * t})
    samples = samples[:400]
    print("sampling %d points along it against the field" % len(samples))

    r = c.tool("plan_field", {"x": to_x, "y": to_y, "deadline_ms": 15000,
                              "probe": samples})
    print("field: %s" % (r.get("note") or "")[:120])
    probes = r.get("probes") or []
    last = None
    shown = 0
    for i, one in enumerate(probes):
        cls = ("outside" if one.get("outside") else
               "free" if one.get("passable") else
               "SHUT" if one.get("blocked") else
               "unknown ground")
        if cls != last:
            print("  %3d  %.1f,%.1f  %s  ground %s clear %s"
                  % (i, one["x"], one["y"], cls,
                     "%.2f" % one["ground"] if "ground" in one else "-",
                     "%.1f" % one["clear"] if "clear" in one else "-"))
            last = cls
            shown += 1
        if shown > 40:
            break


main()
