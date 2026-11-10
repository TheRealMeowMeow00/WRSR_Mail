# The build menu tabs in Workers & Resources: Soviet Republic

Research from the WRSR Mail project, by TheRealMeowMeow00. SOVIET64.exe 1.1.1.9. Addresses are
with the image base `0x140000000` and belong to this version.

Marks, as in `modding.md`:

- **[game]** seen working in the game.
- **[code]** read in the game's code, not separately tried.
- **[untested]** expected to work, nobody has tried it yet.

The question: can a mod add a tab of its own to the bottom build menu, next to "Food industry" or
"Waste management", and put its buildings there?

**Short answer: yes. The menu is a plain list that one function fills at start-up, and a tab added
to that list with the game's own functions is drawn, clicked and navigated like any other
[code]. The Frontline mod does it, and WRSR Mail does it since 1.1.0 [game].**

## Credit

That this can be done at all I learned from the Frontline mod by SAYONE (Steam Workshop
3835680832), which adds a "Defence" section this way. Frontline is closed source and none of its
code is used here. What follows was read in the game itself; Frontline told me where to look.

## How the game builds the menu

`FUN_140051E30` builds the whole menu. It has one caller, at `1402F0ECA`, at the end of the
function that creates the game's tools, Workshop buildings included. It clears the tab vector and
adds 26 tabs and 342 items, all with the same few calls [code]:

| What | Function | Notes |
|---|---|---|
| tab vector | `1409E17D8` | member `+0xC8C8` of the game object at `1409D4F10`; records of `0xB0` |
| tabs: count, at | `14008E390`, `14008E380` | |
| tabs: construct, push, destruct | `140079430`, `14008E3C0`, `140079470` | the game zeroes a record on its stack, constructs, pushes, destructs |
| set up a tab | `140051CA0(tab, name, textId, group, color*)` | `color` is 16 bytes read with `movaps`: align it |
| items: count, at | `14008E470`, `14008E460` | on `tab+0x50`, records of `0x30` |
| items: construct, push, destruct | `140079500`, `14008E500`, `140079520` | |
| set up an item | `140051B80(item, tool, 0x947, 0)` | the game passes `0x947` for all but one item |
| tool by name | `14003AAA0(game, name)` | NULL when there is none; tools are `0x2D0` long at `game+0xD280`, name first |
| push an int | `14001DF90(vector, int*)` | for the two section vectors |

The memory belongs to the game's allocator, so a plugin has to call these and not build the
vectors itself.

### A tab record

| Offset | What |
|---:|---|
| `+0x00` | name, a C string. The icon is `media_soviet/editor/bottomtab_<name>.png`, 96x96 RGBA, loaded when the tab is set up |
| `+0x40` | the icon texture |
| `+0x48` | text id of the tooltip, through `C3D_LANGUAGE::GetString` |
| `+0x50` | vector of items |
| `+0x68` | vector of int: text id of each section heading |
| `+0x80` | vector of int: number of items up to the end of each section |
| `+0x98` | group, 0 to 5 |
| `+0x9C` | four floats, a colour; every tab gets the same one |

An item is a tool pointer at `+0`, a vector of tool pointers at `+8` (variants of one building),
the text id at `+0x20`. A tool for a building has the building type at `+0x48`; for a Workshop
building its name is `<workshop item folder>/<building folder>`, for example
`3835656082/MailService` [code, and Frontline uses the same form].

### The tabs of the game

| Group | Tabs, in order | Frame caption id |
|---:|---|---|
| 0 | roads, railways, pedestrianway, ships, airplanes, cableways | 67000 |
| 1 | factorycons, conveyors, pipelines, coverbulk | 0x105B9 |
| 2 | electric, water, heatpipes, waste | 0x10635 |
| 3 | residential, forcitizens, attractions, culture | 0x105BB |
| 4 | foodindustry, miscell, fosilfuels, metalurgy, advindustry, constindustry | 0x105BC |
| 5 | terraintools, editor_civ | none |

## What the menu code expects of a tab

Read in `FUN_14007C400`, which draws the menu, and `FUN_140444210`, the gamepad navigation [code].

- **One row, in vector order.** Tab `i` is drawn at `i` times the tab width. The background bar is
  as wide as the number of tabs, so it grows by itself.
- **The group frames do not grow.** The five captioned frames behind the tabs are drawn at fixed
  places, made for 6, 4, 4, 4 and 6 tabs. A tab inserted in the middle pushes every later tab out
  of its frame. A tab added after the last industry tab moves only the terrain tools, which have
  no frame. That is the one place that costs nothing, and it is where both Frontline and WRSR Mail
  put theirs. Frontline then draws one more piece of frame itself; WRSR Mail leaves its tab
  without a frame.
- **The last tab is hidden outside the editor.** The drawing loop stops one short unless the
  editor flag at `game+0x1090` is set. A tab appended at the very end would be invisible in a
  normal game, and `editor_civ` would show up instead.
- **A tab needs a section and an item.** An open tab reads the first entry of `+0x68` and of
  `+0x80` without checking that there is one. An empty tab crashes when it is clicked.
- **The group number is only used by the gamepad**, to jump from group to group.
- **The game keeps a pointer to the open tab** (`game+0xC8E0`). The builder clears it first, so
  moving records right after the builder is safe; later it is not.

## How WRSR Mail adds its tab

In `plugins/mail/mail.cpp`, "the build menu tab".

1. The builder is found by a signature of its prologue. Every function and both objects in the
   table above are then read from the builder's own first tab and first item, each call checked
   at its place, so no address is written in the plugin. Any mismatch: no tab, a line in the log.
2. An inline hook lets the builder run, then adds the tab `mail` with group 4, items for the
   buildings that exist as tools, and one section per non-empty group of them.
3. The new record is pushed at the end and moved in front of the first tab of group 5.
4. Captions come from the plugin's `GetString` hook, ids `1700010` to `1700013`.
5. The icon `bottomtab_mail.png` is copied once into `media_soviet/editor`. If the mod has none,
   the game's `bottomtab_miscell.png` is copied under that name. No file, no tab.

Settings: `mail_tab = 0` turns it off; `mail_tab_extra` lists buildings of other mods for a
section "Other mail buildings", see `modding.md`.

Log lines start with `mail  tab:`. In a working game there is one `menu builder hooked` at start
and one `added as tab 25 of 27 with N buildings` when the game builds its menu.

## Seen in the game

2026-11-10, WRSR Mail on 1.1.1.9, the author's screenshot: the tab is there after "Industry" with
its icon, the terrain tools moved one place to the right and still work as a row, and the open tab
shows both sections with their headings and all 14 buildings [game]. The tab has no frame and no
caption under it, as expected.

Open: every building picture in the tab carries the small round badge the game puts on a building
that is not available. The author's reading is that it is the badge for "technical maintenance"
being switched off in the game settings, which the game was running with. Not looked into yet:
which check draws the badge and why it applies to these buildings.

## Not known yet

- Building from the tab, the tooltip of the tab, and the tab on a gamepad were not tried.

- Whether a Workshop building in a tab also stays in the Workshop menu. Frontline's do appear in
  its tab; I have not read what the Workshop menu does with them [untested].
- What a missing icon file does. Frontline never lets it happen and neither does WRSR Mail.
- With Frontline installed too, both add tabs at the same place. Frontline calls the builder
  through the address it patched, WRSR Mail hooks the builder itself, so ours is added first and
  Frontline's tabs follow it. Its extra frame is sized for its own tabs only [code, untested].
- Vehicles are bought from a different list, in the window of the depot or service. This research
  does not cover it; see `vehicle-classes.md`.
