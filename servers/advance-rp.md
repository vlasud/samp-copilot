# Advance RolePlay — how to play here

What this server expects, written down as it is found out. Everything here is
server-specific: none of it belongs in the module's code, and all of it is the
sort of thing a player learns once and then just knows.

`185.169.134.239:7777` · launcher `Advance.exe` · client `omp-client.dll`

## The keys

The server's interaction key is **Left Alt** — controller action **17**, which
`get_bindings` reads off the player's own table. Every prompt that says
"Нажмите Alt" means that key. Under a Russian keyboard layout the game sees no
letter keys at all, so the module asks its own window for English before
anything else.

## Who is who, by skin

The module reports each ped's skin and never interprets it; what a skin means
is written here.

| Skin | Who |
|---|---|
| 70, 274, 275, 276 | medics — the duty doctor at the hospital reception is one of these |

`get_npcs skins:[70,274,275,276]` finds the doctor without guessing at "the
nearest ped that is not a player", which picks up any passer-by.

## Talking to an NPC behind a counter

Stand **in front of his face**, not beside him. `get_npcs` reads the game's own
ped pool and gives each one's heading and the point to stand on; standing at
his shoulder gets nothing at all.

The dialog re-arms only after leaving his range and coming back. Pressing Alt
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

1. **Take a free bed first.** A label reads "Койка свободна / Нажмите Alt
   чтобы занять её"; stand about a metre from it and press Alt. The server
   answers "Вы заняли койку" and "Чтобы выписаться подойдите к врачу или
   выйдите за пределы больницы".
2. **Then the doctor**, found by skin (70, 274, 275, 276). A player medic if
   one is on duty — the label over the
   reception says "Дежурный врач: Свободен" when the post is vacant, and a
   working doctor advertises in the chat with a telephone number. With no
   player medic, the NPC at the counter: stand in front of his face and press
   Alt. He offers a list, "Что Вас беспокоит?", of fifteen ailments.
3. **Then the exit arrow.**

Treatment does not happen by standing about: health sits where it is - it was
watched at 13.1 for four minutes with the connection alive.

Ways that do **not** work: `/healme` needs a first aid kit ("У Вас нет с собой
аптечек") and kits are sold outside; the staff health point on the lab floor
at (2672.5, 1527.0) answers "Вы не можете пользоваться этим пунктом
восстановления здоровья"; the roof exit at (2624.6, 1527.0) does nothing for a
patient; the reception pickup is an information box only.

## Commands worth knowing

`/menu` opens the player menu — Статистика, Задания, Список команд, Личные
настройки, Настройки безопасности, Связь с администрацией, Улучшения, Battle
Pass, Правила сервера, Изменить имя, Промокод, Донат. `follow_dialog` walks it
by naming rows rather than counting them.

From Список команд → Мин. здравоохранения, the medical commands are a medic's,
not a patient's: `/out` discharges a patient, `/medhelp` treats one for money,
`/heal` treats in an ambulance. The patient's own is `/healme`, which uses a
kit from his inventory.

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
