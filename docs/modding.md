# Making content for WRSR Mail

For modders who want to add post offices, warehouses, mail services or trucks. WRSR Mail is by
TheRealMeowMeow00, GPL-3.0-only. Game version: SOVIET64.exe 1.1.1.9.

Each rule below is marked:

- **[game]** seen working in the game.
- **[code]** read in the game's or the plugin's code, not separately tried.
- **[untested]** expected to work, nobody has tried it yet. Please report what you find.

## What the mod needs from you

WRSR Mail is about the mechanics. Its buildings are placeholders on models from the base game and
the Early Start DLC, there to make the mechanics work, and it has no models of its own. Content that
is actually made for mail is what it lacks most:

| Wanted | Role | Sizes in the mod today |
|---|---|---|
| Post offices, sorting centres | factory | 40 to 320 workers, automated 20 and 120 |
| Mail depots | warehouse | any size, see below |
| Mail service, garage with a post office front | service | 8 vehicles |
| Mail trucks, vans, liveries | covered truck | any capacity |

Every era is welcome, 1920s to modern. You do not need the plugin's code, only ini files and
your models.

**Please start the name of everything you make with `[MAIL]`**, as all of the mod's own do, for
example `$NAME_STR "[MAIL] District post office"`. Players then find all mail content, whoever made
it, by searching for one word. It is a recommendation, the plugin does not read names.

## How mail flows

Two resources, both added by the plugin:

- `mail_raw`, unprocessed. Buildings produce it, a mail truck collects it.
- `mail_done`, processed. A mail truck brings it to houses, residents use it.

Path: building, truck, warehouse, truck, post office, truck, warehouse, truck, house. Going
without a warehouse also works. Raw mail can be bought abroad and processed mail sold abroad.

## Your item depends on the plugin

Your building names `mail_raw` and `mail_done`, and those exist only while the plugin runs with
`resources = 1`. **An unknown resource name can crash the game while it loads** [code]. So:

- Mark WRSR Mail as required on your Workshop page.
- Tell players it needs the mod to be running, not only subscribed.
- The resource count is part of a save. Saves from before the mod was installed do not load
  with it, and buildings on older maps have no mail storages, so test on a new map [game].

## Buildings

Start from a game building of a similar shape. Copy the folder structure from any building in
`media_soviet/dlc3/buildings/`: `building.ini`, `building.bbox`, `building.fire`,
`renderconfig.ini`, `imagegui.png`, the model and materials. List it in your `workshopconfig.ini`
with `$OBJECT_BUILDING <folder>`. The mod's own builds are in `tools/make_buildings.py` of its
repository, if you want to see how a donor becomes a mail building.

### Post office: a factory

```
$NAME_STR "[MAIL] Your post office"
$TYPE_FACTORY
$WORKERS_NEEDED 40
$PRODUCTION mail_done 0.025
$CONSUMPTION mail_raw 0.025
$CONSUMPTION_PER_SECOND eletric 0.07
$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 mail_raw
$STORAGE_EXPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 mail_done
$WASTE_PRODUCTION_DISABLE
```

Plus your geometry, connections and construction lines. [game]

Rates are per worker per day. Output is workers times rate, so 40 × 0.025 = 1 t a day.

| Kind | Rate | Worker days a tonne | Power per tonne a day |
|---|---|---|---|
| Manual | 0.025 | 40 | 0.07 |
| Automated | 0.1 | 10 | 0.25 |

**Please keep to these two.** The trade price of mail is worked out from 40 worker days a tonne,
and it follows the wage over the years, so a much cheaper tonne makes every other post office
pointless.

Storage numbers are in the game's units. On screen they are 0.35 times that: 20 shows as 7 t.

**A mail factory is clean production: no pollution, no smoke.** [code]

- No `$POLLUTION_SMALL`, `$POLLUTION_MEDIUM` or `$POLLUTION_HIGH`.
- No `$PARTICLE factory_*` lines. This one is easy to miss: the game adds pollution for each of
  them, a model's chimney smoke is what pollutes:

| Particle | big | medium | small |
|---|---|---|---|
| `factory_*_black` | see below | 7 | 4 |
| `factory_*_gray` | 6 | 4.2 | 2.4 |
| `factory_*_white` | see below | 2.1 | 1.2 |

  (The values of `big_black` and `big_white` were not decoded. Use none of them anyway.)
- A car plant style building can be used. Change `$TYPE_PRODUCTION_LINE` to `$TYPE_FACTORY` and
  remove `$SUBTYPE_*`, `$PROFESORS_NEEDED`, `$ELETRIC_CONSUMPTION_LIVING_WORKER_FACTOR`, the
  `$VEHICLE_PARKING` and `$PARKING_NOT_BLOCK*` lines and any `$RESOURCE_VISUALIZATION` block.
  [untested in play]

### Warehouse

```
$NAME_STR "[MAIL] Your mail depot"
$TYPE_STORAGE
$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 300 mail_raw
$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 300 mail_done
$VEHICLE_LOADING_FACTOR 5.0
$VEHICLE_UNLOADING_FACTOR 5.0
```

[game]

- **The size is yours to choose.** 300 here is only the mod's own depot, not a target: fit the
  number to your building. The window shows it times 0.35, the factor mail has for covered
  storage, so 300 shows as 105 t.
- **Use `$STORAGE_SPECIAL`, one line per mail.** A plain `$STORAGE RESOURCE_TRANSPORT_COVERED` has
  the game's own fixed list of goods, and mail is not on it [code; seen on customs houses]. The
  plugin only treats a store as mail when mail is its single entry.
- **Capacity 5 or more in game units.** Below that the plugin takes the store for the small mail
  store every building has: it is not a destination and its mail does not age [code].
- Stock in a warehouse slowly goes out of date, a post office's does not. That is meant.

### Mail service

```
$NAME_STR "[MAIL] Your mail service"
$TYPE_GARBAGE_OFFICE
$RESOURCE_SOURCE_WASTE_PLASTIC
$RESOURCE_SOURCE_WASTE_TOXIC
$STORAGE_FUEL RESOURCE_TRANSPORT_OIL 30
$WORKING_VEHICLES_NEEDED 8
```

Plus `$VEHICLE_STATION` lines and the rest of the donor's. [game]

- **Exactly those two `$RESOURCE_SOURCE_` lines.** That is how the plugin knows a mail service: a
  technical office whose type names exactly two sources. One more or less and it is an ordinary
  technical office [code]. In the window, these rows read as the two mails.
- `$WORKING_VEHICLES_NEEDED` sets how many vehicles it runs. The mod's own has 8.
- Use a motor service, not `$HORSE_WORKPLACE`. Carts cannot carry mail, see below.

### Other buildings

Every other building that makes waste gets two small mail stores automatically, you add nothing.
**A building that declares its own waste bin in the ini gets none** [code], so it neither sends
nor receives mail. Container stands are left out this way on purpose.

Houses take their mail storage from their size, 1.5 kg per resident, from `$STORAGE
RESOURCE_TRANSPORT_PASSANGER` or `$STORAGE_LIVING_AUTO`. [game]

A truck reaches a house with only a pedestrian entrance, the way a garbage truck does. [game]

### The "Mail" tab of the build menu

The plugin adds a tab "Mail" to the bottom build menu, after the industry tabs, with the mod's
own buildings in it [game]. The section for other mods is [untested]. Your buildings can be listed there too, in a section "Other mail
buildings". The player adds them to `mail.ini`:

```
mail_tab_extra = 1234567890/MyPostOffice 1234567890/MyMailDepot
```

Each entry is `<your Workshop item id>/<building folder>`, the folder being the one named in your
`$OBJECT_BUILDING` line. A name the game does not know is skipped, so a player who is not
subscribed to your item loses nothing. Put the line for your item on your Workshop page so that
players can copy it. How the tab works is in `build-menu.md`.

## Vehicles

**There is no mail vehicle class, and there does not need to be.** A mail service buys any covered
road truck, and the truck carries what its route asks for. So a mail truck is a covered truck:

```
$RESOURCE_TRANSPORT_TYPE RESOURCE_TRANSPORT_COVERED
```

- Leave out `$RESOURCE_ALLOW_ONLY`. One truck carries both mails, in turns. Restricting it
  to one resource should keep it from the other [untested].
- The plugin does not look at the name. Start the name shown in the game with `[MAIL]`, as for
  buildings.
- Road vehicles only. A cart with `RESOURCE_TRANSPORT_GENERAL` does not appear in the purchase
  list [code].
- Players can limit what a service buys with `mail_vehicles` in `mail.ini`, a list of names or parts
  of names [untested in the window]. Picking a clear name for your truck, such as `mail_van_1975`,
  lets them list all of it with `mail_`.
- Why no new class: see `docs/vehicle-classes.md`.

Trains, planes and ships need nothing special. They carry mail as they carry any covered cargo and
trade it through customs. [game]

## Testing your content

1. Install the plugin with `resources = 1`, start a **new map**.
2. Place your building, supply it, watch it for some days.
3. The log is `rml/logs/rml-runtime.log` in the RML folder. Lines of the plugin start with `mail  `:
   - `mail  storage dump`: what a building's stores really are.
   - `mail  state`: how many buildings the service sees and how full they are.
   - `mail  stuck`: a truck was recalled after doing nothing for 90 s. If your depot or service
     causes this often, its parking or connections are worth a look.

Check that trucks reach all your loading points and that the window names both mails.

## Credit and license

WRSR Mail is GPL-3.0-only. Your models and textures stay yours.

- Please name WRSR Mail as required and link it on your page.
- If you take code from the plugin, the GPL applies to that code: keep the notices and publish the
  source under the same license.
- If you use another author's model, get their permission and credit them. The mod itself ships
  nothing from other mods, and it can take your work into its recommended list only on the same
  terms.

Questions and finds about anything marked **[untested]** are welcome.
