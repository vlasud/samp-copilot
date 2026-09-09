"""Ask the field for a route and look at it.

    python tools/fieldtest.py 1550 -1700

Prints what the field made of the world between here and there - how much
was painted, how long it took, how many legs the route has and how much
longer it is than the straight line - and a slice of the picture round the
route, so a route that hugs a wall or crosses one is seen rather than
inferred from a ratio.
"""
import math
import sys

sys.path.insert(0, "tools")
from mcp_http import Client


def main():
    to_x, to_y = float(sys.argv[1]), float(sys.argv[2])
    c = Client(timeout=120)
    c.handshake("fieldtest")
    me = c.tool("look", {"radius": 1, "chat": 0})["self"]["pos"]
    straight = math.hypot(to_x - me[0], to_y - me[1])
    print("from %.0f,%.0f to %.0f,%.0f - %.0f m straight" % (me[0], me[1], to_x, to_y, straight))

    r = c.tool("plan_field", {"x": to_x, "y": to_y, "deadline_ms": 15000, "picture": True})
    print("field: %s" % r.get("note"))
    print("  ok %s, reaches %s, short by %.0f m, %d legs, %.0f m (%.2f x straight), "
          "%d ms, %d cells, %d expanded"
          % (r.get("ok"), r.get("reaches_target"), r.get("short_by_m", 0),
             r.get("legs", 0), r.get("length_m", 0),
             r.get("length_m", 0) / straight if straight else 0,
             r.get("took_ms", 0), r.get("cells", 0), r.get("expanded", 0)))
    floors = r.get("tile_floors") or []
    if floors:
        print("  ref_z %.1f; tile floors: min %.1f max %.1f, %d tiles: %s"
              % (r.get("ref_z", 0), min(floors), max(floors), len(floors),
                 " ".join("%.0f" % f for f in floors[:40])))
    line = r.get("ground_line") or []
    if line:
        def show(v):
            return "#" if v == -3 else "?" if v == -1 else "x" if v == -2 else "%.1f" % v
        print("  ground along the line, a metre a step (# solid, ? unknown):")
        print("    " + " ".join(show(v) for v in line[:40]))
        print("    " + " ".join(show(v) for v in line[40:80]))
    ref = r.get("refused") or {}
    if ref:
        print("  neighbours refused: shut %d, step %d (tallest %.2f m), corner %d"
              % (ref.get("shut", 0), ref.get("step", 0), ref.get("tallest_step", 0), ref.get("corner", 0)))
    for line in r.get("end_neighbours") or []:
        print("  " + line)
    for p in r.get("points", [])[:12]:
        print("    %.1f, %.1f, %.1f" % (p["x"], p["y"], p["z"]))

    pic = r.get("picture") or []
    if pic:
        # The rows the route touches, with a little either side.
        rows = [i for i, row in enumerate(pic) if "o" in row or "@" in row or "*" in row]
        if rows:
            lo, hi = max(0, rows[0] - 8), min(len(pic), rows[-1] + 9)
            cols = [j for row in pic[lo:hi] for j, ch in enumerate(row) if ch in "o@*"]
            c0, c1 = max(0, min(cols) - 40), min(len(pic[0]), max(cols) + 41)
            print("picture rows %d..%d, columns %d..%d (@ start, * end, o route, # solid, ' ' no ground):"
                  % (lo, hi, c0, c1))
            for row in pic[lo:hi]:
                print("  " + row[c0:c1])

    # And what the whole planner says, field and fallbacks together.
    p = c.tool("plan_path", {"x": to_x, "y": to_y})
    legs = p.get("legs") or []
    total = sum(math.hypot(l["to"][0] - l["from"][0], l["to"][1] - l["from"][1])
                for l in legs if l.get("from") and l.get("to"))
    print("planner: %d legs, %.0f m (%.2f x straight): %s"
          % (len(legs), total, total / straight if straight else 0,
             (p.get("note") or "")[:120]))


main()
