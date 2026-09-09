# Advance RolePlay — how to play here

What this server expects, written down as it is found out. Everything here is
server-specific: none of it belongs in the module's code, and all of it is the
sort of thing a player learns once and then just knows.

`185.169.134.239:7777` · launcher `Advance.exe` · client `omp-client.dll`

## The world says which key

Nothing here names the key for an action, because the server already does. It
writes the instruction on the thing itself — a label over a bed reads "Койка
свободна / Нажмите Alt / чтобы занять её" — and on the screen, where its own
prompts appear beside whatever the character is standing in front of. Both
reach the state page. Read the prompt in front of you and press the key it
names; a key written down here would be one more thing to go stale the day
the server changes it.

Under a Russian keyboard layout the game sees no letter keys at all, so the
module asks its own window for English before anything else.

## Getting into a building

Doors are mostly scenery. What lets a character inside is a pickup on the
ground at the entrance: walk onto it and the server takes it from there —
sometimes straight in, sometimes with a dialog first. So a building that
looks shut is usually a pickup not yet stood on, not a door to be pushed. The
state page lists the pickups near him with their places; the nearest one at a
doorway is the way in.

## Who is who, by skin

The module reports each ped's skin and never interprets it; what a skin means
is written here.

| Skin | Who |
|---|---|
| 70, 274, 275, 276 | medics — the duty doctor at the hospital reception, and the nurses standing about the wards |
| 76 | the social workers by the Los Santos train station |

`get_npcs skins:[70,274,275,276]` finds the medics without guessing at "the
nearest ped that is not a player", which picks up any passer-by. The nearest
medic is usually not the one wanted: the wards are full of nurses in the same
skin, and the one who talks is the one standing at the reception counter, so
pick by distance to the counter rather than to the character.

## Talking to an NPC behind a counter

Stand **in front of his face**, not beside him. `get_npcs` reads the game's own
ped pool and gives each one's heading and the point to stand on; standing at
his shoulder gets nothing at all.

The dialog re-arms only after leaving his range and coming back. Pressing the key
twice in a row where you stand gets silence the second time — walk away ten
metres and return.

## The hospital

The city hospital interior sits at about `x 1335..1375, y -860..-800, z 1013`.

- Reception counter, information pickup: **(1356.0, -827.7)**
- Duty doctor NPC: **(1356.7, -834.8)**, facing due east
- Exit arrow, under the EXIT sign: **(1374.0, -834.7)**
- Stairs up: **(1340.6, -842.7)** and **(1337.1, -842.7)** → lab floor at
  about `(2626, 1504, 1016)`; "На 1 этаж" there at `(2663.1, 1523.6)` comes
  back to the lobby side

**Getting out while hurt.** The exit arrow answers "Медперсонал не может
отпустить Вас в таком состоянии. Отправляйтесь на лечение" and does nothing.
The order matters:

1. **Take a free bed first.** A label over each one says what to press;
   stand about a metre from it and press what it says. The server
   answers "Вы заняли койку" and "Чтобы выписаться подойдите к врачу или
   выйдите за пределы больницы".
2. **Then the doctor**, found by skin (70, 274, 275, 276). A player medic if
   one is on duty — the label over the
   reception says "Дежурный врач: Свободен" when the post is vacant, and a
   working doctor advertises in the chat with a telephone number. With no
   player medic, the NPC at the counter: stand in front of his face and press
   the key his own prompt names. He offers a list, "Что Вас беспокоит?", of fifteen ailments.
3. **Then the exit arrow.**

Taking the bed is what starts the treatment, and it then runs by itself
wherever he goes - about a point of health every four seconds, from fifteen to
ninety-five in a few minutes. Standing about without having taken a bed does
nothing at all: health was watched at 13.1 for four minutes with the
connection alive and did not move.

The duty doctor answers "Дежурный врач занят. Ожидайте очереди" when somebody
else is being seen. He is not needed for the exit: once the treatment has run,
the arrow lets him out on its own - the character walked out at 95 health and
landed on the street at about (1189, -1302, 13.6).

Ways that do **not** work: `/healme` needs a first aid kit ("У Вас нет с собой
аптечек") and kits are sold outside; the staff health point on the lab floor
at (2672.5, 1527.0) answers "Вы не можете пользоваться этим пунктом
восстановления здоровья"; the roof exit at (2624.6, 1527.0) does nothing for a
patient; the reception pickup is an information box only.

## A dialog takes the keyboard

While any dialog is on screen nothing else works: no walking, no key, no chat
line. The character stands there and every action is swallowed. So a dialog is
always the first thing to deal with, and `act` refuses to run any chain whose
first step is not `answer` while one is up - it stops at once with
`stopped_by: dialog` rather than reporting steps the game never saw.

## Commands worth knowing

`/menu` opens the player menu — Статистика, Задания, Список команд, Личные
настройки, Настройки безопасности, Связь с администрацией, Улучшения, Battle
Pass, Правила сервера, Изменить имя, Промокод, Донат. `follow_dialog` walks it
by naming rows rather than counting them.

From Список команд → Мин. здравоохранения, the medical commands are a medic's,
not a patient's: `/out` discharges a patient, `/medhelp` treats one for money,
`/heal` treats in an ambulance. The patient's own is `/healme`, which uses a
kit from his inventory.

## Work

`/menu` → Список команд → Работы lists the professions: Пожарный, Развозчик,
Автомеханик, Уличный торговец, and more below them.

Getting hired is not done from the station. `/fire` and `/tasks` both answer
that the character has to be an employee already, the checkpoint standing on
the station square does nothing when walked into, and the social workers there
- skin **76**, not the medics' skins - do not open a dialog at all.
Each profession appears to have its own place of hiring: the fire station for
a fireman, and so on. Go there rather than trying to be hired where he stands.

## Checkpoints

The server marks where it wants somebody to go with a red checkpoint, not a
pickup: a job's delivery point, the next corner of a route, a spot to park.
`get_checkpoint` reports the one being shown and, separately, a racing
checkpoint with the position of the one after it — `look` carries both too.
Walk to `at` with `travel_to` or a `go` step.

Only one of each exists at a time: SA-MP keeps them in its own CGame rather
than in a pool, because a player is only ever shown one.

Seen live at the Los Santos train station: a checkpoint at (1763.7, -1885.8),
radius 2.1, sitting on the social worker's spot.

## Pickups and labels

Pickups are streamed: a scan finds only what is near, so walk the floor and
scan again rather than concluding a place has none. The 3D labels carry the
prompts - which key, and what it does - and are worth reading before pressing
anything.

## The anticheat

The server disconnects this client with "Обнаружено использование чит-программ
(код 27)", anywhere from two seconds to twenty minutes into a session. What it
looks like from inside, and what is worth knowing before drawing any
conclusion: the chat stops updating, the position stops changing, health
freezes at whatever it was, and the walker reports the server has frozen the
player. Everything measured after that point is measured on a disconnected
client and means nothing. Take a screenshot before believing anything.
