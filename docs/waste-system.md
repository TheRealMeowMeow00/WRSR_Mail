# The waste system of Workers & Resources: Soviet Republic

Reverse-engineering notes for SOVIET64.exe 1.1.1.9, made for WRSR Mail, which copies
this system's structure without touching it. Everything here comes from static
reading of data files and decompiled code unless marked otherwise. Addresses include
the image base 0x140000000. They identify this build only: runtime code must locate
everything by signature.

Confidence: **read** means the code was read. **partial** means only part of it was
read. **inferred** means the role is deduced from context, not proven.

## Resources

- Mixed waste, `waste_mixed`, is a standalone record at `game+0xC3B8`, outside the
  resource vector. **read**
- Fractions, by pointer in the game object: `+0xC3C8` gravel, `+0xC3D0` steel,
  `+0xC3D8` aluminium, `+0xC3E0` plastic, `+0xC3E8` bio, `+0xC3F0` fertiliser,
  `+0xC3F8` burnable, `+0xC400` toxic, `+0xC408` other, `+0xC410` ash. **inferred**
  from the field order, the office source ids and the bio to fertiliser decay.
- Sorted list used by storages: `game+0x12078`, stride 0x10. **read**
- Mixed is really a composition: the record field `+0x2EC` is its share per fraction,
  and `FUN_1401917C0` builds a storage's slot list from it. **read**

## Sources

Every source ends in `FUN_1401990C0(game, building, list, factor)`. The list is
`{resource*, amount}`, stride 0x10, and the function frees it. 7 direct callers.

| Source | Where | Amount from |
|---|---|---|
| Citizen at home | `FUN_14083A6D0`, gated by `game+0x5B8 > 0` | What was consumed, through composition tables |
| Customer leaving | `FUN_140822FD0` | Type list `+0x700`, `$WASTE_CUSTOMERS` |
| Worker leaving | `FUN_14017D4E0`, `FUN_14017E770` | Type list `+0x718`, `$WASTE_WORKERS` |
| Production | `FUN_140176FF0` from `FUN_1401B3700`, then `FUN_14019A3B0`, `FUN_140190B70` | `$WASTE_PRODUCTION`, `$WASTE_RESOURCE_RATIO`. **partial** |

- Composition tables: `game+0x12720`, `+0x12738`, `+0x12750`, `+0x12768`, `+0x12780`,
  filled by `FUN_1401942D0` from `FUN_140194A30`. `FUN_1401986E0` applies them. **read**
- Factor scale: `FUN_140197310` depends on the person, happiness among other things. **partial**
- Parser (`FUN_14010E1F0`, too large to decompile, read as disassembly): the
  `$WASTE_WORKERS` branch keeps a resource only if it is `waste_mixed` or in the sorted
  list. An ini cannot name another resource there. **read**
- Disable tokens: `$WASTE_DISABLE`, `$WASTE_PRODUCTION_DISABLE`,
  `$WASTE_WORKERS_DISABLE`, `$WASTE_CUSTOMERS_DISABLE`. **read** as tokens.

## Storage

Building storages: `building+0x970`, stride 0xE0.

| Offset | Meaning |
|---|---|
| `+0x00` | Vector of `{resource*, amount}` |
| `+0x18` | Vector of per-resource settings, stride 0x10: resource, share at `+8`, enabled at `+0xC` |
| `+0x84` / `+0x85` | Import / export |
| `+0x88` | Kind: -1 default, 5 small bin, 6 big bin, 8 and 9 special, 10 carplant, 0x32 fuel |
| `+0x8C` | Capacity |
| `+0x90` | Transport class, 17 is waste |
| `+0xC8` | Vector of containers on a stand, stride 0xBC. **inferred** |

- Waste storages sum all contents to get their fill, unlike other classes. **read**
- Default bin: at the end of the building parser, when the type makes waste and has no
  waste storage. Class 17, kind 5 or 6, sized in steps of 0.12 t or 6.5 t. Added with
  `FUN_1400CC2F0` around 0x14011B05D. **read**
- Copied to the building with the same `FUN_1400CC2F0` into `building+0x970`, near the
  end of `FUN_1401F0BE0`. **read**, the save side was **not** traced.
- `FUN_14019A3B0` and `FUN_14019DE90` find where a resource fits. The latter accepts
  classes 1, 2 and 17 as unload targets. **partial**

## Container stands

`$TYPE_TRASH_CONTAINER`, type 0x69. Small 0.72 or 2.16 t, big 52 or 156 t.
`$STORAGE_SPECIAL_TRASH_*` with capacity 0 declares the sortable fractions.

- A building's reachable stands are at `building+0x1140`. **read**
- `FUN_1401990C0` routes waste there, and falls back to the building's own bin. **read**
- Stand sorting: `FUN_14019D1A0`, `FUN_14019D3B0`, `FUN_14019D8B0`, `FUN_14019DD00`
  move container counts between a stand's fractions. `FUN_1404292D0` reads what a
  storage is set to, `FUN_14019CD70` decides what it actually holds. **read**
- Road and pedestrian variants exist, `_pede`. **read** as files.

## Vehicles

- `$RESOURCE_TRANSPORT_TYPE RESOURCE_TRANSPORT_WASTE` plus `$SKILL_WASTE_CONTAINER_SMALL`
  (39) or `_BIG` (38). Capacity 4 to 10 t. Vehicle type: class `+0x8600`, capacity
  `+0x8604`, skills `+0x85E0`, name `+0x200`. **read**
- Tags: `FUN_1403E9480` fills `type+0x96B8` with category ids by string, `FUN_14025C7B0`
  resolves them from `game+0x127E8`, stride 0x44. Water, sewage, waste, snowplows,
  horse and others. **read**. How a purchase window chooses tags: **not found**.
- Animation tokens: `$GARBAGE_LOADING_ANIM`, `$GARBAGE_UNLOADING_ANIM`,
  `$GARBAGE_ANIM_BIGCONTAINERS`, with speeds.

## The technical office

`$TYPE_GARBAGE_OFFICE`, type 0x31. Tick `FUN_1401C3030`, called from the building tick
`FUN_140139A70`. **read**

1. Walks its vehicles at `office+0xC70` and collects the idle ones by class 15, 16, 17,
   and snow ones by skill 0x23. Each vehicle has a random cooldown at `+0x508`.
2. Every 10 s with no targets or 30 s otherwise (`office+0x1028`) `FUN_14017BDC0`
   rebuilds targets at `office+0xF90`, by range, keeping building types that make waste.
3. Runs `FUN_1401C3CE0` (snow), `FUN_1401C4A00` (water), `FUN_1401C4BF0` (sewage),
   `FUN_1401C4E20` (waste). Requires `game+0x5A4 > 0`.

`office+0xF78` is a vector of `{building*, source id}`: the destinations the player
sets, one slot for each `$RESOURCE_SOURCE_*`. Ids 0x16 gravel, 0x17 steel, 0x18
aluminium, 0x19 plastic, 0x1A bio, 0x1B toxic, 0x1C general. **read**

### Waste assigner, `FUN_1401C4E20`

- Builds the list of waste types that have a destination with room, `FUN_14019DE90`.
- For each vehicle:
  - Empty and at base: `FUN_14019E330` picks the best target: class 17, kind matching the
    truck's size, and load at least the truck capacity or fill above a threshold.
  - On a route with room: `FUN_14019E750` searches nearby for the next stop of the same
    fraction, by path distance. That makes rounds of several stops.
  - Loaded: goes to the destination.
  - No work: 15 s cooldown.
- `FUN_14019F180(game, office, destination, source, vehicle, resource)` builds the route.

### Route

`FUN_14067CB30` clears, `FUN_14067D5D0` adds a stop, `FUN_1406B8EB0` starts, on failure
`FUN_1406B8C80` then clear. A stop has stride 0x198: `+0` load flag, `+4` load share,
`+8` load resources, `+0xC8` unload flag, `+0xCC` unload share, `+0xD0` unload resources.
Same shape in water `FUN_14017C000` and sewage `FUN_14017C260`. **read**

### Delivery, water

`FUN_14017CA40` picks the lowest fill below a threshold from the target list, and the
route is source, target, base. The only delivery logic in the system, and the model for
the return half of mail. **read**

## Stops

`FUN_14068B330`, the vehicle tick, 7843 lines. **partial**

- A storage matches when its class equals the vehicle class, generically. For an office
  vehicle, class 15 and 16 start from the type's special index (`+0x838`, `+0x83C`).
- Waste branch: `FUN_140197500` gets the load, `FUN_14019CD30` and `FUN_14019CD50` scale
  the speed by kind, `FUN_140420D50` and `FUN_140421350` transfer. Stats `FUN_1401945C0`.

## Landfill, sorting, burning

- Landfill and transfer stations are plain `$TYPE_STORAGE` with `$STORAGE
  RESOURCE_TRANSPORT_WASTE`, 72 to 360 t. Nothing special in the ini. **read**
- Separation: factories with `$WASTE_EXTRACTION <fraction> <rate>`. Treatment and
  burning: `$WASTE_CONSUMPTION`, `$WASTE_CONSUMPTION_RATIO`, `$WASTE_PRODUCTION_ASH`.
  Runtime **not** read.
- Periodic storage tick: `FUN_140198E50`, each 10 s per building, on class 17 storages
  only, calls `FUN_140199C70`. **read**
  - Decay: bio to fertiliser, mixed and toxic shrink, ash.
  - Pollution, see below.
  - Above a fill threshold: excess removed through `FUN_140421A30`, with pollution.
- `FUN_140198930` adds bio waste from livestock storages. **partial**

Nothing decays or pollutes unless the storage is class 17.

## Effect on citizens

- `FUN_140199C70` calls `FUN_1404D57A0`, which adds to the pollution map `game+0x120A8`
  (setting `game+0x5D4`). Contributors: toxic, bio, burnable, other. **read**
- `FUN_1404D5F50` copies the map to housing `+0x11B0` and `+0x11B4`. **read**
- `FUN_14083A6D0`: health drops by pollution x 0.015 x difficulty, and illness odds rise.
  **read**
- No direct write from waste to happiness was found. `WARNING_MESSAGE_GARBAGE_FULL` is a
  setting for a warning.

## Interface

- `FUN_1406F4B40`, 66 callers: common window sections, in order `FUN_140772400`,
  `FUN_1407752A0`, `FUN_140775000`, `FUN_140702730`, `FUN_1406F4DB0`. **read**
- `FUN_140775000(game, x*, y*, window)`: the container section, kinds 5 and 6, title
  text id 0x10A8, skipped for stands. `FUN_140706C00` draws one storage. **read**
- `FUN_1404A12E0` with `FUN_1401978C0`: the map overlay of worst fill. **read**
- Text ids: `FUN_1402A1B20(class, kind)`. External language files were not found.

## Categories in the interface

### Vehicle tags

- Vehicle types: `game+0x12840`, stride 0x9F08. **read**
- Tag table: `game+0x127E8`, stride 0x44: name, 64 bytes, then the caption text id at
  `+0x40`. Filled once by hardcoded calls in `FUN_1403E69F0`, through `FUN_140448700`,
  called from 0x14028EE53. No data file. **read**
- `FUN_1403E9480` gives each vehicle type its tags, from its skills and class. **read**

### Purchase window

- Opened from the window code (`FUN_1406F8CC0`), which calls
  `FUN_1403DDA40(game, mode, building, ...)`: mode 1 USD, 2 RUB, 7 vehicles from a
  depot. It fills the candidates at `game+0x12858`. **read**
- Its final test is `FUN_1403E2900(game, building, vehicleType, 1, 0)`, which decides if
  a vehicle fits a building, by type and skills. 13 callers. For office type 0x31 this
  is where garbage, water, sewage and snow are allowed. **partial**
- `FUN_14080CC60` draws the window. It builds the filter from the tags of the
  candidates, `FUN_140815760` draws the dropdown. So a category exists when a listed
  vehicle has the tag. **read**
- `FUN_1403E3F40` is a count of free places, not a suitability test. **read**

### Build menu

- Tabs: table at `game+0xC608`, stride 0x68, filled in `FUN_1402E6BC0`. **read**
- `workshopbuildings` and `editorbuildings` are both `FUN_140388140(game, editor)`, chosen
  in `FUN_140317DB0`. **read**
- Building records: `game+0x11B20`, stride 0xBE8. **read**
- **The category is the building type number at `+0x360`.** The function collects the
  distinct numbers among listed buildings, and filters on `game+0xC6A8`. One exception:
  type 2 subtype 1 shows as 0x35. **read**
- The dropdown is `FUN_140815760`. How it captions a building type was **not** traced.

### Office destinations section

`FUN_140770A30`, called from 3 windows (0x14074E6CE, 0x14077A2E6, 0x14078117B), draws the
"utilities management" rows. One row per entry of `office+0xF78`, `{building*, id}`. The id
picks both captions by a hardcoded table: water 0x14, sewage 0x15, gravel 0x16, steel 0x17,
aluminium 0x18, plastic 0x19, bio 0x1A, toxic 0x1B, general 0x1C. Clicking a row starts the
`building_addsource` tool, command `AddCOBuilding`. The choice is handled in `FUN_1401D51A0`,
**not** read. Rows should follow the `$RESOURCE_SOURCE_*` tokens of the type, **inferred**.

## Not read

Runtime of `$WASTE_EXTRACTION` and `$WASTE_CONSUMPTION`, production waste in full, the
save format of storages, the office's window, how a building joins `building+0x1140`,
the caption of a building category.
