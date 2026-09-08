"""Drives a test run against the running game, from the command line.

Everything the mod can be asked to do, it can be asked over the loopback
interface it serves: arm the character, send him somewhere, watch how it
goes, read what he thinks happened. That makes a route test something a
script can run - no map marker, no F11 menu, no hand on the keyboard.

    python tools/testrun.py launch          start the game and wait for the mod
    python tools/testrun.py wait            until the character can be driven
    python tools/testrun.py travel 1468 -1689
    python tools/testrun.py ready
    python tools/testrun.py status
    python tools/testrun.py stop
    python tools/testrun.py log --grep plan --since 19:08
    python tools/testrun.py quit            close the game

The server's password dialog is answered by the mod itself, from bot.login
next to it, when the player has put one there. Nothing here ever holds the
password.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_http import Client, NotRunning   # noqa: E402

GAME_DIR = os.environ.get("GTABOT_GAME_DIR", r"D:\SAMP")
LAUNCHER = os.path.join(GAME_DIR, "Advance.exe")
LOG = os.path.join(GAME_DIR, "bot.asi.log")

# The launcher takes the server and the name on its command line, which is
# how it starts the game when somebody presses Play. Given them, it needs no
# window and no press.
HOST = os.environ.get("GTABOT_HOST", "185.169.134.239")
PORT = os.environ.get("GTABOT_PORT", "7777")
NICK = os.environ.get("GTABOT_NICK", "Lo_Vlasuddd")


def say(text):
    print(text, flush=True)


def connect(timeout=0):
    """A client, waiting up to `timeout` seconds for the game to answer."""
    client = Client()
    deadline = time.time() + timeout
    while True:
        try:
            client.handshake("testrun")
            return client
        except NotRunning:
            if time.time() >= deadline:
                raise
            time.sleep(2)


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gta_sa.exe"],
                         capture_output=True, text=True).stdout
    return "gta_sa.exe" in out


# ---- the steps ----

def cmd_launch(args):
    if game_running():
        say("the game is already running")
    else:
        if not os.path.exists(LAUNCHER):
            say("no launcher at %s" % LAUNCHER)
            return 1
        # -g gamepath, -h host, -p port, -n name; -P is the server's own
        # password, which this one does not have.
        command = [LAUNCHER, "-g", GAME_DIR, "-h", args.host,
                   "-p", str(args.port), "-n", args.nick]
        subprocess.Popen(command, cwd=GAME_DIR)
        say("started: %s" % " ".join(command))
    say("waiting for the mod to answer (up to %d s)" % args.timeout)
    try:
        client = connect(args.timeout)
    except NotRunning as e:
        say("  %s" % e)
        return 1
    state = client.tool("ready")
    say("  mod up: %s" % state.get("next", state))
    say("now: testrun.py wait")
    return 0


def cmd_wait(args):
    """Until the character can be driven. The mod says what it is waiting for."""
    client = connect(10)
    deadline = time.time() + args.timeout
    last = ""
    while time.time() < deadline:
        state = client.tool("ready")
        note = state.get("next", state.get("error", "?"))
        if note != last:
            last = note
            say("  %s" % note)
        if state.get("ready_to_travel"):
            say("ready at %s" % json.dumps(state.get("position", {})))
            return 0
        # Arming is ours to ask for; everything else is waiting.
        if state.get("spawned") and not state.get("world_readable"):
            client.tool("set_movement", {"on": True})
        time.sleep(2)
    say("not ready after %d s" % args.timeout)
    return 1


def cmd_travel(args):
    client = connect(10)
    armed = client.tool("set_movement", {"on": True})
    if not armed.get("ready"):
        say("not ready to move: %s" % armed.get("note", armed))
        return 1
    started = time.time()
    reply = client.tool("travel_to", {"x": args.x, "y": args.y})
    if "error" in reply:
        say("travel_to refused: %s" % reply["error"])
        return 1
    say("going to (%.1f, %.1f), %.0f m away" %
        (args.x, args.y, reply.get("straight_m", 0)))
    last = ""
    while time.time() - started < args.timeout:
        time.sleep(2)
        trip = client.tool("travel_status")
        walk = trip.get("walk", {})
        line = "%5.0f m left  leg %s/%s  %s | %s" % (
            trip.get("straight_m", 0), walk.get("leg", "?"), walk.get("legs", "?"),
            trip.get("note", ""), walk.get("note", ""))
        if line != last:
            last = line
            say("  %s" % line)
        if not trip.get("travelling"):
            took = time.time() - started
            arrived = "arrived" in str(trip.get("note", ""))
            say("%s after %.0f s, %.0f m from the target" %
                ("ARRIVED" if arrived else "STOPPED", took, trip.get("straight_m", 0)))
            return 0 if arrived else 2
    say("still going after %d s - giving up on watching" % args.timeout)
    client.tool("stop")
    return 3


def cmd_status(args):
    client = connect(5)
    say(json.dumps(client.tool("travel_status"), indent=2, ensure_ascii=False))
    return 0


def cmd_ready(args):
    client = connect(5)
    say(json.dumps(client.tool("ready"), indent=2, ensure_ascii=False))
    return 0


def cmd_stop(args):
    client = connect(5)
    client.tool("stop")
    client.tool("set_movement", {"on": False})
    say("stopped and stood down")
    return 0


def cmd_quit(args):
    if not game_running():
        say("the game is not running")
        return 0
    subprocess.run(["taskkill", "/IM", "gta_sa.exe", "/F"], capture_output=True)
    for _ in range(20):
        if not game_running():
            say("the game is closed")
            return 0
        time.sleep(1)
    say("the game did not close")
    return 1


def cmd_log(args):
    if not os.path.exists(LOG):
        say("no log at %s" % LOG)
        return 1
    with open(LOG, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    if args.since:
        lines = [l for l in lines if l[1:9] >= args.since]
    if args.grep:
        pattern = re.compile(args.grep)
        lines = [l for l in lines if pattern.search(l)]
    if not args.input_lines:
        lines = [l for l in lines if "] input: " not in l]
    for line in lines[-args.tail:]:
        say(line.rstrip()[:400])
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("launch", help="start the game and wait for the mod")
    p.add_argument("--timeout", type=int, default=180)
    p.add_argument("--host", default=HOST)
    p.add_argument("--port", default=PORT)
    p.add_argument("--nick", default=NICK)
    p.set_defaults(run=cmd_launch)

    p = sub.add_parser("wait", help="wait until the character can be driven")
    p.add_argument("--timeout", type=int, default=300)
    p.set_defaults(run=cmd_wait)

    p = sub.add_parser("travel", help="send him to a point and watch")
    p.add_argument("x", type=float)
    p.add_argument("y", type=float)
    p.add_argument("--timeout", type=int, default=420)
    p.set_defaults(run=cmd_travel)

    p = sub.add_parser("ready", help="where the session stands and what to do next")
    p.set_defaults(run=cmd_ready)

    p = sub.add_parser("status", help="what the journey thinks it is doing")
    p.set_defaults(run=cmd_status)

    p = sub.add_parser("stop", help="stop and stand movement down")
    p.set_defaults(run=cmd_stop)

    p = sub.add_parser("quit", help="close the game")
    p.set_defaults(run=cmd_quit)

    p = sub.add_parser("log", help="the mod's log, filtered")
    p.add_argument("--tail", type=int, default=60)
    p.add_argument("--grep", default=None)
    p.add_argument("--since", default=None, help="HH:MM:SS")
    p.add_argument("--input-lines", action="store_true",
                   help="keep the per-quarter-second input lines")
    p.set_defaults(run=cmd_log)

    args = parser.parse_args()
    try:
        return args.run(args)
    except NotRunning as e:
        say(str(e))
        return 1


if __name__ == "__main__":
    sys.exit(main())
