"""How much of the walking overhead is the bot and how much is the city.

Gets him outdoors if he is not, then for each errand asks the field for its
own route first - the field's A* is optimal on its own grid, so its length
is as short as walking that way can be - and compares what he actually
walked against it. A walked/optimal near one means the walker is not
wandering; whatever is left over the straight line is the city's buildings.
"""
import math
import sys
import time

sys.path.insert(0, "tools")
from mcp_http import Client

# Offsets from wherever he starts, so the test works in any town: four
# errands out and one back, each a few hundred metres.
# Short enough that the field reaches the mark in one plan: only then is
# its route the true shortest way, and only then does walked/optimal mean
# anything. A hundred and twenty to a hundred and eighty metres.
# Short enough that the field reaches the mark in one plan - only then is
# its route the shortest way there is, and only then does walked/optimal
# mean anything. Sixty to a hundred metres.
OFFSETS = [(70.0, -50.0), (-80.0, 40.0), (50.0, 70.0), (-60.0, -60.0),
           (90.0, 0.0), (0.0, -90.0), (-70.0, 70.0), (60.0, 60.0)]


def main():
    c = Client(timeout=90)
    c.handshake("optimal")

    def look(radius=1, chat=0):
        for _ in range(4):
            s = c.tool("look", {"radius": radius, "chat": chat})
            if "self" in s:
                return s
            time.sleep(2)
        return s

    def go(x, y, within=0.8, patience=150):
        c.tool("travel_to", {"x": x, "y": y, "stop_within": within})
        began = time.time()
        while time.time() - began < patience:
            time.sleep(1.5)
            st = look()
            if "self" not in st or not (st.get("travel") or {}).get("travelling"):
                break
        return look(6)

    s = look()
    if (s.get("dialog") or {}).get("shown"):
        c.tool("answer_dialog", {"button": 2})
        time.sleep(1)
    c.tool("set_movement", {"on": True})
    me = look()["self"]
    print("старт: %s hp %d" % ([round(v, 2) for v in me["pos"]], round(me["health"])))

    if me["pos"][2] > 500:
        if me["health"] < 55:
            go(1346.5, -814.5, 1.0)
            for _ in range(5):
                c.tool("press_key", {"key": "LAlt", "ms": 250})
                time.sleep(2.2)
                r = look(1, 8)
                said = [(l.get("text") or "") for l in (r.get("chat") or [])[-6:]
                        if "койк" in (l.get("text") or "").lower()]
                if said and "заняли" in said[-1]:
                    print("занял койку")
                    break
            for _ in range(14):
                time.sleep(40)
                hp = round(look()["self"]["health"])
                if hp >= 70:
                    break
        s = go(1374.0, -834.7, 0.4)
        if "self" in s:
            print("вышел: %s hp %d"
                  % ([round(v, 2) for v in s["self"]["pos"]], round(s["self"]["health"])))
        else:
            print("клиент молчит после выхода")
            time.sleep(10)

    s = look()
    if "self" not in s or s["self"]["pos"][2] > 500:
        print("не вышел")
        return

    home = look()["self"]["pos"]
    for dx, dy in OFFSETS:
        tx, ty = home[0] + dx, home[1] + dy
        s = look()
        if "self" not in s:
            print("клиент отвалился")
            return
        if (s.get("dialog") or {}).get("shown"):
            c.tool("answer_dialog", {"button": 2})
            time.sleep(1)
        start = s["self"]["pos"]
        straight = math.hypot(start[0] - tx, start[1] - ty)
        if straight < 25:
            continue
        plan = c.tool("plan_field", {"x": tx, "y": ty, "deadline_ms": 20000})
        best = plan.get("length_m") or 0
        reaches = plan.get("reaches_target")

        c.tool("travel_to", {"x": tx, "y": ty})
        walked, last, began = 0.0, start, time.time()
        while time.time() - began < 300:
            time.sleep(0.5)
            st = look()
            if "self" not in st:
                break
            here = st["self"]["pos"]
            step = math.hypot(here[0] - last[0], here[1] - last[1])
            if step <= 4.0:
                walked += step
            last = here
            t = st.get("travel") or {}
            if not t.get("travelling") and not (t.get("walk") or {}).get("walking"):
                break
        left = math.hypot(last[0] - tx, last[1] - ty)
        print("-> %.0f,%.0f: прямая %.0f, поле %.0f%s, прошёл %.0f | к прямой %.2f, "
              "к оптимуму %s | осталось %.0f м%s"
              % (tx, ty, straight, best, "" if reaches else " (не доходит)", walked,
                 walked / straight,
                 ("%.2f" % (walked / best)) if best > 1 and reaches else "-",
                 left, "  ДОШЁЛ" if left < 4.0 else ""))
        sys.stdout.flush()


main()
