# Vehicle classes in Workers & Resources: Soviet Republic

Research from the WRSR Mail project, by TheRealMeowMeow00. SOVIET64.exe 1.1.1.9. Offsets belong to
this version. Everything here comes from the disassembly and the game's data files. **Nothing here
was tried in the game**: the experiment at the end is what would settle it.

The question: can a mod add a vehicle class of its own, the way covered, cooler or tanker are
classes?

**Short answer: a truly new number no, reusing a slot maybe, and a mod-side "virtual class" gives
most of the benefit at almost no risk.**

## What a class is

`$RESOURCE_TRANSPORT_TYPE <name>` in a vehicle's `script.ini` sets a number at `vehicle type+0x8600`.
Storages have the same numbers at `storage+0x90`. A resource has a table of `0x20` byte blocks from
`resource+0xCC`, one per class, with the factor between units and tonnes first in the block.

| Number | Name | Vehicles in the base game and DLC folders |
|---:|---|---|
| 0 | COVERED | yes |
| 1 | OPEN | yes |
| 2 | GRAVEL | yes |
| 3 | OIL | yes |
| 4 | CEMENT | yes |
| 5 | COOLER | yes |
| 6 | LIVESTOCK | yes |
| 7 | PASSANGER | yes |
| 8 | CONCRETE | yes |
| 9 | ELETRIC | no, buildings only |
| 10 | VEHICLES | no in `vehicles/`, buildings use it |
| 11 | GENERAL | yes, planes, ships, helicopters |
| 12, 13 | NUCLEAR1, NUCLEAR2 | containers and buildings |
| 14 | HEATING | no data file names it |
| 15 | WATER | yes |
| 16 | SEWAGE | yes |
| 17 | WASTE | yes |
| 18 | none, see below | the default |

Parser: `FUN_1402A1610`, name to number. Reverse: `FUN_1400E4CB0`, which answers `some_error` for
anything unknown.

## Why not number 19

- The table in the resource record has no room. Slot 18 ends at `0xCC + 19 * 0x20 = 0x32C`. Slot 19
  would end at `0x34C`, and the record is `0x340` long, so it would write over the start of the next
  resource, which is its name.
- Growing the record is not realistic. Its size, `0x340`, is written into the code of every walk over
  the resource list that I read.
- The class field is read 425 times in 161 functions (counted around `0x8600(`, so a little
  approximate). `FUN_14068B330` alone reads it 46 times and `FUN_1407D5460` 27 times. Each would need
  checking for a new number.

## Why not number 18

An unknown name does parse, to 18: the last comparison in the parser ends in `0x11 + (no match)`. So
a typo or an invented name loads without complaint. Workshop item 2757425461 has
`RESOURCE_TRANSPORT_PRISONER` and this is what it gets.

But 18 is taken. The vehicle type's constructor sets it as the default (`1403E9BC0`), so it means
"this vehicle has no class", and 29 places compare the field directly with it. It is not free.

## Reusing a slot: 9 or 14

Electricity and heat go through wires and pipes, so no vehicle uses these two. In the disassembly
**no comparison of the vehicle class with 9 or 14 turned up** near any read of the field, while most
classes in use have them. My search only sees plain comparisons, not jump tables, so this is strong
evidence and not proof. Their table slots exist too. On the vehicle side either one looks free.

The problem is the storage side, where they are very much in use: comparisons of `storage+0x90` with
9 turned up 73 times and with 14 54 times. A storage of class 14 is treated as heating and one of
class 9 as electricity, in those places. If loading requires the storage class to equal the vehicle
class, which I have not read, then mail in a class 14 storage gets heating behaviour.

So a reused slot needs one of:

1. Storages that keep their own class, with a hook on the load check that pairs them with the vehicle.
2. Finding out what those 54 places do, and whether they apply to a storage with no heating connection.

And there are costs whichever way it goes:

- Every other carrier stops working unless handled: planes, wagons, ships and customs carry mail today
  because it is class 0.
- `FUN_1401B9A40` and friends index the table with the storage's class, so the slot for mail must be
  filled in.
- A UI that names classes has no text for it.
- Other mods may make the same choice and collide.

## The virtual class

What a mod usually wants from a class is "these vehicles belong to my service, and my rules apply to
them". That needs no number in the game:

- Vehicles stay class 0, so they load, unload and travel as covered trucks everywhere.
- A list says which ones count: a vehicle name list in the plugin's ini, read at start. In WRSR Mail
  that is `mail_vehicles`, and empty means every covered truck.
- The hook on "does this vehicle fit this office" (`FUN` found by `kSigVehicleFits` in `mail.cpp`)
  accepts only those for the mail service, which also fixes the purchase list.
- Rules can follow the owner and the list, as the road search patch below does.

It gives no separate column in the game's tables and no new cargo look by itself.

## Related: what lets a vehicle reach a building with no road

`FUN_14067E8F0`, the road search. Two things set it apart, and this is not one of the `$SKILL_` tokens
alone:

- Skills, in a vector at `vehicle type+0x85E0`, each entry `0x10` long. Numbers are the order in the
  parser: 0x13 groundworks, 0x15 crane, 0x1E ambulance, 0x1F fire truck, 0x20 fire ladder, 0x21
  personal, 0x22 forklift, 0x23 snow plow, 0x24 police car, 0x25 police bus, 0x26 and 0x27 waste
  containers.
- A "service" flag at `rsp+32h`: the owner's type is 0x49, 0x4D or 0x6A, or it is 0x31 and the class
  is 1, 2 or 15 to 17. Water and sewage tankers have no skill in their config and rely on this.

With the flag, a building with fewer than two road bays gets its own search mode. The flag also turns
off the check of road signs in `FUN_14057AFB0`. WRSR Mail adds class 0 to this list at `14067EB5C`.
Whether that is all a truck needs is not yet confirmed in the game.

## An experiment that would settle slot 14

Needs a new map and a throwaway mod.

1. Copy a covered truck, set `$RESOURCE_TRANSPORT_TYPE RESOURCE_TRANSPORT_HEATING`.
2. Does the game load it, and does the vehicle window open? (A crash here ends it.)
3. Add a building with `$STORAGE RESOURCE_TRANSPORT_HEATING 10` for an ordinary resource whose factor
   for slot 14 is set by a plugin. Does the truck load there? Does the building act like it has heating?
4. Does a plane or a wagon still carry that resource?

If you try it, I would like to hear how it went.
