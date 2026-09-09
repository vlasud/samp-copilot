"""The brain, with nothing between it and the game.

An agent turn costs a system prompt, a tool listing and a transcript that
grows all day; a decision took a minute and a half, and the panel showed it.
This is the same loop with none of that: a small fixed prompt, the world as
a short page, one model call, and the answer handed straight to the mod.

    python tools/brain.py --models
    python tools/brain.py --minutes 30 "Устройся на работу и отработай смену"

The key is never in this file and never in the repository. It goes in
`bot.brain-key` beside it, which git ignores, or in BRAIN_API_KEY.
"""
import argparse
import json
import os
import re
import sys
import time
import subprocess
import urllib.request
import io

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_http import Client
import step as digest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What the person wants, said once.
#
# Somebody watching leans over and says "go and buy a car"; the character
# hears it and gets on with it. It is not kept: an errand that came back on
# every turn for the rest of the session stopped being a thing he was asked
# to do and became a thing he could never finish, and he could never move on
# to anything else afterwards.
#
# So it is taken and the file is emptied in the same breath. The brain sets
# its own goals; this only ever nudges them.
TASK_FILE = os.path.join("D:" + os.sep + "SAMP", "bot.task")


def asked_of_him():
    try:
        with io.open(TASK_FILE, encoding="utf-8-sig") as f:
            said = f.read().strip()[:600]
    except Exception:
        return ""
    if said:
        try:
            with io.open(TASK_FILE, "w", encoding="utf-8") as f:
                f.write("")
        except Exception:
            pass          # unreadable is one thing; unwritable is not fatal
    return said
BASE = "https://api.aitunnel.ru/v1/"

# The contract, and nothing else.
#
# What the answer must look like and what each command does - the part that
# would have to change if the mod changed. Everything about how to play, how
# to talk and what this server expects lives in servers/advance-rp.md, which
# is loaded whole beside this, so that knowledge can be edited without
# touching the script.
RULES = """Ответь ОДНИМ объектом JSON и ничем больше — без рассуждений,
пояснений, разметки и текста до или после. Длинный ответ обрывается на
полуслове, не разбирается, и персонаж от этого просто стоит на месте.

{"summary": "одно короткое предложение на русском, что делаешь",
 "steps": ["шаг", "шаг"], "doing": 0,
 "do": [{"tool": "travel_to", "args": {"x": 1189, "y": -1302}}],
 "wait": 2, "stop": false}

- summary и steps рисуются на экранной панели игры, по ним за тобой следит
  человек. Пиши их по-русски.
- do — список вызовов мода по порядку. Пустой список допустим.
- wait — сколько секунд подождать перед следующим осмотром, от 1 до 5.
- stop — true, только когда задача выполнена или дальше действительно
  нельзя. Оборванная связь, перезаход на сервер, пустая страница — это не
  повод останавливаться: подожди и осмотрись снова.

Инструменты мода для do. Все команды движения и цепочки ВОЗВРАЩАЮТСЯ СРАЗУ и
работают в фоне — ты отдал команду, тело её выполняет, а ты на следующих
ходах видишь на странице, как идёт. Не повторяй команду, которая уже идёт.

  travel_to {x,y,stop_within}  идти к точке, как угодно далеко. Перепланирует
                             по дороге. stop_within — на каком расстоянии
                             считать, что пришёл: 2.5 по умолчанию, 0.8 чтобы
                             встать на пикап или вплотную к стойке.
                             Страница: walking: ... N m left / arrived / gave up.
                             Отменить: stop.
  move_to {x,y}              короткий отрезок рядом, без перепланирования.
                             Отменить: stop.
  stop {}                    прекратить любое движение, включая машину.
  act {steps:[...]}          цепочка шагов, тело выполняет сама и САМА
                             ОСТАНАВЛИВАЕТСЯ, когда случилось важное: открылся
                             диалог, кто-то назвал тебя по имени, тебя ранили,
                             шаг застрял. Шаги: {"go":{"x","y","stop_within"}}
                             {"press":"walk"} (клавиша взаимодействия, Alt)
                             {"say":"текст"} {"wait":мс}
                             {"answer":{"choose":"текст строки"}} или
                             {"answer":{"item":N}} / {"answer":{"button":1}}.
                             Страница: doing: step N of M, потом stopped_by:
                             done | dialog | spoken_to | hurt | blocked | cancelled.
                             Отменить: act_stop. При открытом диалоге цепочка
                             запускается, только если первый шаг — answer.
  act_stop {}                отменить цепочку немедленно.
  press_key {key,ms}         одна клавиша, зажать на ms (200 по умолчанию).
                             Имена: walk (=Alt, взаимодействие), enter_exit,
                             horn, jump, sprint, handbrake, accelerate, brake,
                             или один символ ("y", "2"), или alt, enter, esc,
                             space, tab, up, down, f1..f12.
  answer_dialog {choose|item|text|button}
                             единственное, что работает при открытом диалоге.
                             choose: строка по её тексту (ПРЕДПОЧТИТЕЛЬНО —
                             сервер перенумеровывает меню); item: номер строки
                             с нуля; text: ввод в поле; button: 1 = первая
                             кнопка (Enter), 2 = вторая (Esc). По умолчанию 1.
  follow_dialog {path:[...], open_with}
                             пройти цепочку меню одним вызовом, по ТЕКСТУ строк:
                             {"open_with":"/gps","path":["Госучреждения","Мэрия"]}.
                             Сам ждёт каждое следующее меню. Страница/статус:
                             dialog_path_status. Это главный способ ходить по
                             /mn и /gps.
  send_chat {text}           сказать или ввести команду (до 128 символов).
                             Буквы печатаются ~12 в секунду; пока на странице
                             STILL TYPING — новую реплику не слать.
  use_vehicle {}             нажать «сесть/выйти»: рядом с машиной — сядет,
                             внутри — выйдет. Потом проверь in_vehicle.
  drive_to {x,y}             ехать на машине, в которой уже сидишь, по дорогам.
                             Отменить: stop.
  set_standing {name,standing,why}   пометить: friend | neutral | wary | enemy.
Читающие, ничего не меняют:
  look {radius,chat}         полное состояние, если страницы мало
  get_dialog {}              что сейчас в открытом диалоге, все строки
  get_labels {} get_pickups {} get_npcs {} get_checkpoint {}
  get_textdraws {}           что сервер написал на экране
  get_chat {lines}           последние строки чата
"""


def is_local(base):
    """A model running on this machine, which nobody has to pay or trust."""
    return "localhost" in base or "127.0.0.1" in base or "::1" in base


def read_key(base=""):
    # Ollama and the rest want no key at all, but the client library insists
    # on a string, so it gets one and nothing is sent anywhere.
    if is_local(base):
        return "local"
    from_env = os.environ.get("BRAIN_API_KEY")
    if from_env and from_env.strip():
        return from_env.strip()
    path = os.path.join(HERE, "bot.brain-key")
    if os.path.exists(path):
        # utf-8-sig, because PowerShell's Set-Content puts a byte order mark
        # at the front and a mark in an HTTP header is not a header.
        with open(path, encoding="utf-8-sig") as f:
            text = f.read().strip().lstrip("﻿")
        if text:
            return text
    print("No key. Put it in %s (git ignores it) or in BRAIN_API_KEY." % path)
    sys.exit(2)


def ask_ollama(base, model, system, page, think, budget):
    """A local model, taken by the collar.

    A thinking model decides for itself how long to think, and this one
    thought for eleven seconds on one turn and fifty-seven on the next - the
    same question, the same settings. Neither ollama nor the OpenAI wrapper
    will bound that: `num_predict` bounds tokens, and a long think then eats
    the budget and leaves the object unfinished, which is worse than slow.

    So the answer is read as it arrives and cut off at the first of two
    things: the object is complete, or the time is up. Whatever has arrived
    by then is what gets parsed - and a thinking model that has been thinking
    for twelve seconds has usually written the object already and is
    admiring it.

    Returns (text, why_it_stopped).
    """
    body = json.dumps({
        "model": model, "system": system, "prompt": page, "stream": True,
        "think": think,
        # One shape of request, always: changing the options between calls
        # makes ollama reconsider the model and costs seconds.
        "options": {"temperature": 0.4, "num_predict": 1500},
        "keep_alive": "30m"}).encode()
    where = base.rstrip("/")
    if where.endswith("/v1"):
        where = where[:-3]
    request = urllib.request.Request(where + "/api/generate", body,
                                     {"Content-Type": "application/json"})
    said, thought = [], 0
    deadline = time.time() + budget
    # The socket's own patience is not the budget. Nothing arrives at all
    # until the model starts writing, and it can spend a long time before
    # that on a cold cache - a minute has been seen. The budget is counted
    # here, chunk by chunk; the socket only has to outlast the wait for the
    # first of them.
    try:
        stream = urllib.request.urlopen(request, timeout=max(90.0, budget * 6))
    except Exception as e:
        return "", "не дождался: %s" % str(e)[:60]
    try:
        for line in stream:
            if not line.strip():
                continue
            try:
                piece = json.loads(line.decode("utf-8"))
            except ValueError:
                continue
            said.append(piece.get("response") or "")
            thought += len(piece.get("thinking") or "")
            if piece.get("done"):
                break
            whole = "".join(said)
            if only_json(whole) is not None:
                return whole, "объект собран"
            if time.time() > deadline:
                return whole, "вышло время"
    finally:
        stream.close()
    return "".join(said), "договорил"


def only_json(text):
    """The object out of whatever the model wrapped it in."""
    # Thinking out loud, kept out of the way. A local model reasons in the
    # answer itself rather than in a field of its own, and that reasoning is
    # full of braces - the first "{" in the reply belongs to the thinking,
    # not to the order.
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
    text = re.sub(r"^.*?</think>", "", text, flags=re.S)   # opened, never closed
    text = text.strip()
    if text.startswith("```"):
        text = re.sub(r"^```[a-zA-Z]*\n?|```$", "", text).strip()
    start = text.find("{")
    if start < 0:
        return None
    depth, in_string, escaped = 0, False, False
    for i in range(start, len(text)):
        ch = text[i]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                try:
                    return json.loads(text[start:i + 1])
                except ValueError:
                    return None
    return None


# The names the mod actually answers to, and the shorter words a model
# reaches for instead. A brain that says {"travel_to": {...}} means the same
# thing as one that says {"tool": "travel_to", "args": {...}}, and refusing
# the first only means the character stands still while the answer looks
# perfectly sensible in the log.
KNOWN = ("travel_to", "move_to", "act", "act_stop", "press_key",
         "answer_dialog", "send_chat", "use_vehicle", "drive_to", "look",
         "get_chat", "get_npcs", "get_labels", "get_pickups", "get_checkpoint",
         "set_standing", "follow_dialog", "find_text", "stop",
         "get_textdraws", "get_dialog", "get_trail", "get_people",
         "get_objects", "get_object_texts", "check_point", "walk_status",
         "set_standing",
         "travel_status", "drive_status", "act_status", "probe_heights")


def as_call(one):
    """One entry of `do`, in whatever shape it arrived, as (tool, args)."""
    if not isinstance(one, dict):
        return None, None
    if one.get("tool"):
        args = one.get("args")
        return one["tool"], args if isinstance(args, dict) else {}
    if len(one) != 1:
        return None, None
    name, what = next(iter(one.items()))
    if name in KNOWN:
        return name, what if isinstance(what, dict) else {}
    # The chain's own words, used loose at the top level.
    if name == "press":
        return "press_key", {"key": str(what)}
    if name == "say":
        return "send_chat", {"text": str(what)}
    if name == "go" and isinstance(what, dict):
        return "move_to", {k: v for k, v in what.items() if k in ("x", "y", "z")}
    if name == "answer" and isinstance(what, dict):
        return "answer_dialog", what
    return None, None


def in_a_line(page):
    """The past, as one honest line rather than half a page.

    The history used to carry the first four hundred characters of each old
    state, which cut off in the middle of a word - "NPCs near (server char" -
    and told the brain a maimed version of a world that had moved on anyway.
    What is worth remembering about a moment is where he was and what he was
    doing; the whole truth is in the page for the moment he is in now.
    """
    lines = page.split(chr(10))
    keep = [l for l in lines if l.startswith(("me:", "doing:", "dialog:"))]
    return " | ".join(keep) if keep else lines[0][:120]


def revive():
    """Start the game again and wait for it to be in the world.

    The client goes away for all sorts of reasons - the server drops it, it
    falls over, somebody closes the window - and why is not this loop's
    business. A brain that stops for good the first time that happens is not
    living anybody's life, so the game is started again and the loop carries
    on.
    """
    print("клиента нет - поднимаю игру заново")
    for what in (["launch"], ["wait", "--timeout", "200"]):
        try:
            subprocess.run([sys.executable, os.path.join(HERE, "tools", "testrun.py")] + what,
                           cwd=HERE, timeout=300,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as e:
            print("!! %s: %s" % (" ".join(what), e))
            return None
    # The launch can fail, or the game can come up and not answer yet, and
    # a rescue that throws is worse than no rescue at all - it takes the
    # loop down with it.
    try:
        body = Client(timeout=120)
        body.handshake("brain")
        body.tool("set_movement", {"on": True})
    except Exception as e:
        print("!! игра не отозвалась: %s" % e)
        return None
    print("игра снова на связи")
    return body


def main():
    p = argparse.ArgumentParser()
    p.add_argument("task", nargs="?", default="")
    # Left unset, the model and the effort follow from where the brain is
    # running: the measured best of each. Naming either overrides it.
    p.add_argument("--model", default=None)
    # How often a question goes out, counted from the last one rather than
    # from the answer. Thinking already takes several seconds; resting a
    # fixed amount on top of it would make the loop slower the harder the
    # brain thought, which is backwards.
    # How much thinking to pay for. Off is fastest and answers well enough for
    # "walk there, press that"; a little is worth having when the choice is
    # which of nineteen jobs to take. Asked for in the several ways providers
    # spell it, and dropped altogether if the endpoint will not have it.
    p.add_argument("--reasoning", default=None,
                   choices=["off", "low", "medium", "high"])
    p.add_argument("--gap", type=float, default=5.0)
    p.add_argument("--minutes", type=float, default=20.0)
    p.add_argument("--turns", type=int, default=400)
    p.add_argument("--base", default=BASE)
    # A model on this machine: no key, no bill, no network. Ollama's
    # OpenAI-shaped door is on 11434.
    p.add_argument("--local", action="store_true",
                   help="брать модель из ollama на этой машине")
    # How long a local model may think before it is cut off mid-thought.
    p.add_argument("--budget", type=float, default=12.0)
    p.add_argument("--models", action="store_true",
                   help="list what the service offers")
    args = p.parse_args()

    if args.local and args.base == BASE:
        args.base = "http://localhost:11434/v1/"

    # On this machine: gpt-oss at medium. Measured against the alternatives
    # on the same page - four turns in 5.9, 5.4, 4.2 and 3.4 seconds, every
    # one choosing the pickup at the hospital door rather than the interior
    # coordinates written in the notes. On low it is a second faster and
    # picks the wrong one; on high it never closes the object.
    here = is_local(args.base)
    if args.model is None:
        args.model = "gpt-oss:20b" if here else "qwen3.8-flash"
    if args.reasoning is None:
        args.reasoning = "medium" if here else "off"

    from openai import OpenAI
    client = OpenAI(api_key=read_key(args.base), base_url=args.base)
    if is_local(args.base):
        print("модель локальная: %s на %s" % (args.model, args.base))

    if args.models:
        for m in client.models.list().data:
            print(m.id)
        return

    body = Client(timeout=120)
    body.handshake("brain")
    body.tool("set_movement", {"on": True})

    server = ""
    md = os.path.join(HERE, "servers", "advance-rp.md")
    if os.path.exists(md):
        with open(md, encoding="utf-8") as f:
            server = f.read()

    system = RULES + "\n\nПравила этого сервера:\n\n" + server
    recent = []          # a short rolling memory, not the whole day
    began = time.time()
    turn = 0
    just_revived = 0     # the turn the game last came back on
    last_typed = None
    no_reasoning_refused = [False]
    args_task_holder = [""]
    # How many turns running it has said the same thing, and how many it has
    # asked the body for nothing at all.
    stuck = [0]
    idle = [0]
    said_before = [""]

    while turn < args.turns and time.time() - began < args.minutes * 60:
        turn += 1

        # Nothing is decided while the body is still talking.
        #
        # A line of chat is not said the moment it is asked for - the letters
        # go into the game one at a time, like a hand. Deciding again in the
        # middle of that got the same sentence said twice, because from the
        # brain's side nothing had happened yet. So the turn waits for the
        # keys to finish, up to a few seconds, and only then looks.
        held = 0.0
        while held < 6.0:
            try:
                if not (body.tool("act_status") or {}).get("typing"):
                    break
            except Exception:
                break
            time.sleep(0.4)
            held += 0.4
        if held >= 0.4:
            print("      (подождал %.1f с, пока допечаталось)" % held)

        try:
            page = digest.report(body)
        except Exception as e:                       # the client went away
            print("!! %s" % e)
            again = revive()
            if again is None:
                time.sleep(10.0)
                continue
            body = again
            just_revived = turn
            continue

        # Going round in circles, said out loud.
        #
        # It would answer "иду в магазин" with an empty list of things to do,
        # and answer it again, and again - a plan that was never carried out
        # looks from the inside exactly like one that is going fine. The loop
        # can see what the brain cannot: that nothing has been asked of the
        # body and nothing has changed, several turns running.
        if stuck[0] >= 2:
            page += ("\n\nВНИМАНИЕ: ты %d хода подряд говоришь примерно одно и "
                     "то же" % stuck[0])
            if idle[0] >= 2:
                page += " и при этом не отдал моду ни одной команды"
            page += (". То, что ты пробуешь, не работает. Смени подход: "
                     "спроси у игрока, посмотри вывески вокруг, пойди в другое "
                     "место или займись другой целью.")

        asked = time.time()
        messages = [{"role": "system", "content": system}]
        for was, did in recent[-4:]:
            messages.append({"role": "user", "content": was})
            messages.append({"role": "assistant", "content": did})
        # With no task given, none is invented: the brain decides
        # for itself what it wants, and a line reading "Задача:"
        # with nothing after it is just an invitation to make one up.
        typed = asked_of_him()
        if typed != last_typed:
            last_typed = typed
            print("   человек просит: %s" % (typed or "(ничего)"))
        wanted = args.task.strip()
        if typed:
            wanted = (wanted + " " + typed).strip() if wanted else typed
        args_task_holder[0] = wanted
        asked_for = ("Задача от человека: %s\n\n" % wanted) if wanted else ""
        messages.append({
            "role": "user",
            "content": "%sСостояние:\n%s\n\nОтветь одним JSON."
                       % (asked_for, page)})
        def ask(extra=None):
            if is_local(args.base):
                # Straight to ollama, so the answer can be read as it comes
                # and stopped when it is done or when the time is up.
                whole = page if not extra else page + "\n\n" + extra
                # The level, not merely yes or no. A model built with
                # low/medium/high of its own answers in three seconds on low
                # and fourteen on high, and the difference is not subtle.
                think = False if args.reasoning == "off" else args.reasoning
                text, why = ask_ollama(args.base, args.model, system, whole,
                                       think, args.budget)
                # Cut off in the middle of a thought there is no object, and
                # a turn spent musing is a turn the character stood still.
                # So it is asked again with the thinking off, which on this
                # model costs a second and a half and answers straight away.
                if think and only_json(text) is None:
                    print("      (%s - переспрашиваю без размышлений)" % why)
                    text, why = ask_ollama(args.base, args.model, system, whole,
                                           False, args.budget)
                elif why != "объект собран":
                    print("      (%s)" % why)
                return text, why
            said = list(messages)
            if extra:
                said.append({"role": "user", "content": extra})
            spoken = dict(model=args.model, messages=said, temperature=0.4,
                          max_tokens=4000)
            # Ollama has a switch of its own, and it is the only one that
            # works on a local thinking model: the OpenAI-shaped ones are
            # ignored, and the reasoning then arrives inside the answer -
            # "We need answer user with only JSON object" and half a minute
            # gone. With its own switch the object comes back on its own,
            # and a third of the time with it.
            if is_local(args.base):
                spoken["extra_body"] = {"think": args.reasoning != "off"}
            elif not no_reasoning_refused[0]:
                if args.reasoning == "off":
                    spoken["extra_body"] = {"reasoning": {"enabled": False,
                                                          "exclude": True},
                                            "reasoning_effort": "none",
                                            "thinking": {"type": "disabled"}}
                else:
                    spoken["extra_body"] = {
                        "reasoning": {"enabled": True, "effort": args.reasoning,
                                      "exclude": True},
                        "reasoning_effort": args.reasoning}
            try:
                got = client.chat.completions.create(**spoken).choices[0]
            except Exception as e:
                if "extra_body" not in spoken or no_reasoning_refused[0]:
                    raise
                # The endpoint will not take the switch: say so once and go
                # on without it rather than stopping.
                no_reasoning_refused[0] = True
                print("   (модель не приняла настройку рассуждений: %s)"
                      % str(e)[:90])
                spoken.pop("extra_body")
                got = client.chat.completions.create(**spoken).choices[0]
            return (got.message.content or ""), got.finish_reason

        try:
            answer, why_stopped = ask()
        except Exception as e:
            print("!! модель: %s" % e)
            time.sleep(5.0)
            continue

        order = only_json(answer)
        if order is None:
            # Once more, briefly. An answer that ran out of room is the usual
            # reason: the model spent its words explaining itself and never
            # closed the object.
            try:
                answer, why_stopped = ask(
                    "Предыдущий ответ не разобрался. Пришли только объект JSON, "
                    "без единого слова вокруг, и короче.")
                order = only_json(answer)
            except Exception as e:
                print("!! модель: %s" % e)
        thought = time.time() - asked

        if order is None:
            print("%3d  %4.1f c  не разобрал (%s): %s"
                  % (turn, thought, why_stopped, answer.strip()[:80] or "пусто"))
            recent.append((in_a_line(page), answer[:200]))
            continue

        summary = str(order.get("summary", "") or "").strip()[:120]
        if not summary:
            steps = order.get("steps") or []
            summary = (str(steps[0])[:60] if steps else "(мозг не сказал, что делает)")
        print("%3d  %4.1f c  %s" % (turn, thought, summary))

        # Two summaries count as the same when they start alike - "иду в
        # магазин напротив" and "иду к магазину, чтобы купить телефон" are
        # one plan restated, not two plans.
        head = " ".join(summary.lower().split())[:28]
        stuck[0] = stuck[0] + 1 if head and head == said_before[0] else 0
        said_before[0] = head
        idle[0] = idle[0] + 1 if not (order.get("do") or []) else 0

        try:
            body.tool("set_plan", {
                "summary": summary,
                "steps": [str(x)[:60] for x in (order.get("steps") or [])][:8],
                "doing": int(order.get("doing", 0) or 0)})
        except Exception:
            again = revive()
            if again is not None:
                body = again
                just_revived = turn
            continue
        for one in (order.get("do") or [])[:6]:
            name, call_args = as_call(one)
            if not name:
                print("       ? не понял команду: %s"
                      % json.dumps(one, ensure_ascii=False)[:90])
                continue
            try:
                got = body.tool(name, call_args or {})
                print("       %s -> %s"
                      % (name, json.dumps(got, ensure_ascii=False)[:120]))
            except Exception as e:
                print("       %s !! %s" % (name, e))

        recent.append((in_a_line(page), json.dumps(order, ensure_ascii=False)[:300]))
        if order.get("stop"):
            # Not while the world is still coming back. The client had just
            # been restarted, the page said the connection was gone, and the
            # brain took that for the end of the road and stopped for good.
            if turn <= just_revived + 2:
                print("       (не останавливаюсь: игра только что вернулась)")
            else:
                print("мозг говорит: закончили")
                break
        # Ten seconds is the most it may ask for. It had been asking for
        # fifteen and asking again the next turn, standing in the street
        # waiting for somebody to answer a line of chat - and nobody owes it
        # an answer. A reply, when it comes, arrives on the page like
        # anything else; there is nothing to hold still for.
        wanted = min(10.0, float(order.get("wait", 0) or 0))
        rest = max(args.gap - (time.time() - asked), wanted)
        if rest > 0:
            time.sleep(rest)

    print("ходов %d, минут %.1f" % (turn, (time.time() - began) / 60.0))


if __name__ == "__main__":
    main()
