# WRSR Mail

A postal service for Workers & Resources: Soviet Republic, by TheRealMeowMeow00. Version 1.1.0.
GPL-3.0-only, see `LICENSE`. Needs a plugin loader and a new map. The mod is about the mechanics:
its buildings are placeholders on models from the game and the Early Start DLC, all tagged `[MAIL]`.

Buildings make raw mail beside their waste. Mail trucks owned by a mail service collect it
and bring it to a warehouse or straight to the main post office, a factory that processes
it. Trucks bring the processed mail to the homes. Mail in a house lifts its residents'
happiness, a covered house without mail lowers it. Raw mail can be bought abroad, processed
and sold back for a modest profit. The mod's buildings have their own "Mail" tab in the build menu.

See `docs/balance.md` for how the numbers work (in Russian), `docs/modding.md` to make content for
it, `docs/waste-system.md`, `docs/vehicle-classes.md` and `docs/build-menu.md` for what was learned
about the game.

## Building

This repository holds no game files. The buildings are made from your own copy of the game.

```bash
MAIL_TOOLCHAIN=/path/to/llvm-mingw/bin MAIL_GAME_EXE=/path/to/SOVIET64.exe sh build.sh
python tools/make_buildings.py "/path/to/SovietRepublic"
sh tools/make_release.sh <workshop item id>
```

`build.sh` makes `mail.dll` and runs the offline tests against the real game executable. The toolchain
is llvm-mingw, UCRT, x86_64. `src/tesmio_*.h` are the plugin API headers.

## Building on WRSR Mail

Learning from the code and the findings is welcome. If you port, rewrite or re-upload this project or a
part of it: credit WRSR Mail and link this repository, keep the notices, publish your source under
GPL v3, use your own name and say what changed.

## Credits

- The way resources are added comes from `plugins/resources/resources.cpp` in
  MaxLegend/TesmioLoader, GPL-3.0.
- `src/tesmio_*.h` are TesmioLoader's plugin API headers, unchanged.
- That a mod can add a tab to the build menu was learned from the Frontline mod by SAYONE (Steam
  Workshop 3835680832). None of its code is used; see `docs/build-menu.md`.
- All models come from the base game and the Early Start DLC, no other mods. Modders who
  want to add buildings or vehicles: see `docs/modding.md`.
