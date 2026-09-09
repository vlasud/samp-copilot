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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_http import Client
import step as digest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "https://api.aitunnel.ru/v1/"

RULES = """Ты — мозг бота, играющего в GTA San Andreas на русском ролевом сервере
Advance RolePlay. Мод — тело: он сообщает мир и выполняет команды.

ЧЕСТНОСТЬ, важнее всего остального. Ты бот. Никогда не выдавай себя за
человека. Если администратор спросит, бот ли ты, не отрицай — ответь честно и
поставь "stop": true. Никогда не пытайся обойти античит.

Каждый ход тебе дают короткую страницу с состоянием мира. Ответь ОДНИМ
объектом JSON и ничем больше. Никаких рассуждений, пояснений, разметки и
текста до или после — только сам объект, короткий. Длинный ответ обрывается
на полуслове, не разбирается, и персонаж от этого просто стоит на месте.

{"summary": "одно короткое русское предложение, что делаешь",
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

Инструменты мода для do:
  travel_to {x,y}            идти через город
  move_to {x,y}              короткий отрезок
  act {steps:[...]}          цепочка; шаг это {"go":{"x","y","stop_within"}},
                             {"press":"alt"}, {"say":"текст"}, {"wait":мс},
                             {"answer":{"item":N}} или {"answer":{"button":1}}
  act_stop {}                отменить цепочку
  press_key {key,ms}         одна клавиша; клавиша взаимодействия — alt
  answer_dialog {item|button|text}   ответ в открытом диалоге
  send_chat {text}           сказать в чат или ввести команду сервера
  use_vehicle {}, drive_to {x,y}     машины

Правила, которые важнее дотошности:
- Пока страница пишет "DIALOG IS OPEN", работает только answer_dialog.
- В чате антифлуд: между своими сообщениями не меньше 30 секунд.
- Если с персонажем заговорили — отвечай по-русски, в роли. Имя Lo_Vlasuddd.
- Решай быстро и коротко. Медленный цикл — это провал.
"""


def read_key():
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


def only_json(text):
    """The object out of whatever the model wrapped it in."""
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
         "set_standing", "follow_dialog", "find_text", "stop")


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
    body = Client(timeout=120)
    body.handshake("brain")
    body.tool("set_movement", {"on": True})
    print("игра снова на связи")
    return body


def main():
    p = argparse.ArgumentParser()
    p.add_argument("task", nargs="?",
                   default="Осмотрись и веди себя как обычный игрок.")
    p.add_argument("--model", default="deepseek-v4-flash-0731")
    p.add_argument("--minutes", type=float, default=20.0)
    p.add_argument("--turns", type=int, default=400)
    p.add_argument("--base", default=BASE)
    p.add_argument("--models", action="store_true",
                   help="list what the service offers")
    args = p.parse_args()

    from openai import OpenAI
    client = OpenAI(api_key=read_key(), base_url=args.base)

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

    while turn < args.turns and time.time() - began < args.minutes * 60:
        turn += 1
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

        asked = time.time()
        messages = [{"role": "system", "content": system}]
        for was, did in recent[-4:]:
            messages.append({"role": "user", "content": was})
            messages.append({"role": "assistant", "content": did})
        messages.append({
            "role": "user",
            "content": "Задача: %s\n\nСостояние:\n%s\n\nОтветь одним JSON."
                       % (args.task, page)})
        def ask(extra=None):
            said = list(messages)
            if extra:
                said.append({"role": "user", "content": extra})
            got = client.chat.completions.create(
                model=args.model, messages=said, temperature=0.4,
                max_tokens=4000).choices[0]
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
            recent.append((page[:400], answer[:200]))
            continue

        summary = str(order.get("summary", "") or "").strip()[:120]
        if not summary:
            steps = order.get("steps") or []
            summary = (str(steps[0])[:60] if steps else "(мозг не сказал, что делает)")
        print("%3d  %4.1f c  %s" % (turn, thought, summary))

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

        recent.append((page[:400], json.dumps(order, ensure_ascii=False)[:300]))
        if order.get("stop"):
            # Not while the world is still coming back. The client had just
            # been restarted, the page said the connection was gone, and the
            # brain took that for the end of the road and stopped for good.
            if turn <= just_revived + 2:
                print("       (не останавливаюсь: игра только что вернулась)")
            else:
                print("мозг говорит: закончили")
                break
        time.sleep(max(1.0, min(5.0, float(order.get("wait", 2)))))

    print("ходов %d, минут %.1f" % (turn, (time.time() - began) / 60.0))


if __name__ == "__main__":
    main()
