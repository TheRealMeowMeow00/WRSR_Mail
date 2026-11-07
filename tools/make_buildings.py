# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the mail buildings in item/: the normal set from base game donors, whose
# materials point at the game's own files, and the Early Start set from the DLC's
# buildings. Nothing comes from other mods. Geometry, connections and construction stay
# the donor's, the economy is rewritten. A factory here is clean production: no smoke
# and no pollution, because the game adds up pollution from the $PARTICLE factory_ lines.
#
# These inis name mail_raw and mail_done, and an unknown resource name can crash the
# parser. Install them only together with the plugin and resources = 1.
#
#
#   python tools/make_buildings.py "G:/SteamLibrary/steamapps/common/SovietRepublic"

import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ITEM = os.path.join(HERE, "..", "item")

# Direct delivery to the post office works, the warehouse is the efficient way: 105 t on
# screen against 7 t and five times the loading speed, at the price of its own truck line
# to the office and of mail going out of date there.
#
# Tokens the rules below replace. Nothing written here may contain a dollar sign in a
# comment: building.ini has no comment syntax.
ECONOMY = re.compile(
    r"^\$(NAME|NAME_STR|PRODUCTION|CONSUMPTION|CONSUMPTION_PER_SECOND|WORKERS_NEEDED"
    r"|CONSUMPTION_WATER_REQUIRED_QUALITY|PRODUCTION_SEWAGE_POLLUTION"
    r"|WASTE_[A-Z_]+|RESOURCE_SOURCE_[A-Z_]+|STORAGE[A-Z_]*|POLLUTION_[A-Z]+|PARTICLE"
    r"|VEHICLE_LOADING_FACTOR|VEHICLE_UNLOADING_FACTOR)\b.*$"
)



def sorting(name, workers, per_worker, eletric, storage):
    """A sorting centre. Output is workers times per_worker, in tonnes a day: 0.025 is the manual
    40 worker days a tonne the trade price rests on, 0.1 an automated centre's 10."""
    return [
        '$NAME_STR "{}"'.format(name),
        "$WORKERS_NEEDED {}".format(workers),
        "$PRODUCTION mail_done {}".format(per_worker),
        "$CONSUMPTION mail_raw {}".format(per_worker),
        "$CONSUMPTION_PER_SECOND eletric {}".format(eletric),
        "$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED {} mail_raw".format(storage),
        "$STORAGE_EXPORT_SPECIAL RESOURCE_TRANSPORT_COVERED {} mail_done".format(storage),
        "$WASTE_PRODUCTION_DISABLE",
    ]


BUILDINGS = {
    "MailService": {
        "donor": "technical_services",
        "head": [
            '$NAME_STR "Mail service"',
            # The two rows of the window: where collected mail goes, where delivered
            # mail comes from.
            "$RESOURCE_SOURCE_WASTE_PLASTIC",
            "$RESOURCE_SOURCE_WASTE_TOXIC",
            "$STORAGE_FUEL RESOURCE_TRANSPORT_OIL 30",
        ],
    },
    "MailWarehouse": {
        "donor": "warehouse_covered_combined",
        "head": [
            '$NAME_STR "Mail warehouse"',
            "$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 300 mail_raw",
            "$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 300 mail_done",
            "$VEHICLE_LOADING_FACTOR 5.0",
            "$VEHICLE_UNLOADING_FACTOR 5.0",
        ],
    },
    "SortingOffice": {
        "donor": "waste_plasticrecycling",
        "head": [
            '$NAME_STR "Main post office"',
            "$WORKERS_NEEDED 40",
            "$PRODUCTION mail_done 0.025",
            "$CONSUMPTION mail_raw 0.025",
            "$CONSUMPTION_PER_SECOND eletric 0.07",
            "$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 mail_raw",
            "$STORAGE_EXPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 mail_done",
            "$WASTE_PRODUCTION_DISABLE",
        ],
    },
    # Four times the small one, workers per tonne unchanged: the trade price rests on 40
    # worker days a tonne, so a cheaper tonne here would push the small office out. What it
    # buys is one loading point and less time in a warehouse, see docs/balance.md.
    "SortingOfficeLarge": {
        "donor": "food_factory",
        "head": [
            '$NAME_STR "Main post office (large)"',
            "$WORKERS_NEEDED 160",
            "$PRODUCTION mail_done 0.025",
            "$CONSUMPTION mail_raw 0.025",
            "$CONSUMPTION_PER_SECOND eletric 0.28",
            "$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 40 mail_raw",
            "$STORAGE_EXPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 40 mail_done",
            "$WASTE_PRODUCTION_DISABLE",
        ],
    },
    # 2 and 8 t a day, the same 40 worker days a tonne.
    "SortingOfficeMedium": {
        "donor": "fabric_factory",
        "head": sorting("Main post office (medium)", 80, 0.025, 0.14, 30),
    },
    "SortingOfficeHuge": {
        "donor": "eletronic_components_factory",
        "head": sorting("Main post office (huge)", 320, 0.025, 0.56, 60),
    },
    # Few workers and a lot of power. A tonne takes 10 worker days, not 40, and 0.25 of power
    # a day, not 0.07, so they earn more per tonne while the grid can carry them.
    "SortingCentreAuto": {
        "donor": "plastics_factory",
        "head": sorting("Automated sorting centre", 20, 0.1, 0.5, 30),
    },
    # The car plant's body, as a plain factory: what only a production line has is dropped.
    # Its cargo pictures name storages by number, and this building has just two.
    "SortingCentreAutoLarge": {
        "donor": "production_vehicle",
        "as_factory": True,
        "head": sorting("Automated sorting centre (large)", 120, 0.1, 3.0, 100),
    },
}

# Early Start: the DLC's own buildings from media_soviet/dlc3/buildings. Model, materials,
# connections and construction stay the DLC's, the economy is rewritten. These folders copy
# DLC files. The author's call (2026-11-07): that may ship, since a player without the DLC
# cannot start in the years they belong to. Whether a player without the DLC sees them in
# the menu on a later start was not checked.
EARLY = {
    "MailServiceEarly": {
        "donor": "technical_services_medium",
        "head": [
            '$NAME_STR "Mail service (early)"',
            "$RESOURCE_SOURCE_WASTE_PLASTIC",
            "$RESOURCE_SOURCE_WASTE_TOXIC",
            "$STORAGE_FUEL RESOURCE_TRANSPORT_OIL 20",
        ],
    },
    "MailWarehouseEarly": {
        "donor": "warehouse1",
        "head": [
            '$NAME_STR "Mail warehouse (early)"',
            "$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 300 mail_raw",
            "$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 300 mail_done",
            "$VEHICLE_LOADING_FACTOR 5.0",
            "$VEHICLE_UNLOADING_FACTOR 5.0",
        ],
    },
    "SortingOfficeEarly": {
        "donor": "clothing_factory",
        "head": [
            '$NAME_STR "Main post office (early)"',
            "$WORKERS_NEEDED 40",
            "$PRODUCTION mail_done 0.025",
            "$CONSUMPTION mail_raw 0.025",
            "$CONSUMPTION_PER_SECOND eletric 0.07",
            "$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 mail_raw",
            "$STORAGE_EXPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 mail_done",
            "$WASTE_PRODUCTION_DISABLE",
        ],
    },
    "SortingOfficeLargeEarly": {
        "donor": "food_factory_old",
        "head": [
            '$NAME_STR "Main post office (large, early)"',
            "$WORKERS_NEEDED 160",
            "$PRODUCTION mail_done 0.025",
            "$CONSUMPTION mail_raw 0.025",
            "$CONSUMPTION_PER_SECOND eletric 0.28",
            "$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 40 mail_raw",
            "$STORAGE_EXPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 40 mail_done",
            "$WASTE_PRODUCTION_DISABLE",
        ],
    },
    "SortingOfficeMediumEarly": {
        "donor": "distillery_old",
        "head": sorting("Main post office (medium, early)", 80, 0.025, 0.14, 30),
    },
    "SortingOfficeHugeEarly": {
        "donor": "electronics_factory",
        "head": sorting("Main post office (huge, early)", 320, 0.025, 0.56, 60),
    },
}

RENDER ="""$TYPE_WORKSHOP
 MODEL model.nmf
 MATERIAL ../{mtl}
{emissive} LIFE 3000.000000
 EXPLOSION_GROUP 0
 DERBIS_FALLING_FX buildingfall1 1.000000
 DERBIS_FALLED_FX buildingfall2 1.400000
 DERBIS_FALLED_SFX collapse
 DERBIS_NUM 20
 DERBIS_FALLING_FX_MAXTIME 3.000000
 DERBIS_SCALE 1.000000
 DERBIS_MESH buildings/buildingwreck1.nmf buildings/buildingwreck.mtl
 DERBIS_MESH buildings/buildingwreck2.nmf buildings/buildingwreck.mtl
 END
"""


def donor_materials(media, donor):
    """MATERIAL and MATERIALEMISSIVE of a donor, from the game's own table. The file
    names do not follow the model's: technical_services_small uses distribution_office."""
    text = open(os.path.join(media, "buildings", "buildingtypes.ini"), encoding="latin1").read()
    m = re.search(r"^\$TYPE\s+" + re.escape(donor) + r"\s*$(.*?)^\s*END\s*$", text, re.M | re.S)
    if not m:
        raise SystemExit("no entry in buildingtypes.ini for " + donor)
    mat = re.search(r"^\s*MATERIAL\s+(\S+)", m.group(1), re.M)
    emi = re.search(r"^\s*MATERIALEMISSIVE\s+(\S+)", m.group(1), re.M)
    if not mat:
        raise SystemExit("no MATERIAL for " + donor)
    return mat.group(1), emi.group(1) if emi else None


# Tokens of a production line that a factory has no use for.
LINE_ONLY = re.compile(
    r"^\$(SUBTYPE_[A-Z_]+|PROFESORS_NEEDED|ELETRIC_CONSUMPTION_LIVING_WORKER_FACTOR"
    r"|VEHICLE_PARKING|PARKING_NOT_BLOCK|PARKING_NOT_BLOCK_DETOUR_POINT_PID)\b.*$"
)


def build_ini(donor_path, head, as_factory=False):
    # Every name starts with the tag, so that all of it is found by searching for [MAIL].
    out = [l.replace('$NAME_STR "', '$NAME_STR "[MAIL] ', 1) for l in head]
    seen_type = False
    in_picture = False
    for raw in open(donor_path, encoding="latin1").read().splitlines():
        line = raw.rstrip()
        if ECONOMY.match(line.strip()):
            in_picture = False
            continue
        if as_factory:
            # A cargo picture runs over several lines, up to the next token.
            if line.startswith("$"):
                in_picture = line.startswith("$RESOURCE_VISUALIZATION")
            if in_picture or LINE_ONLY.match(line.strip()):
                continue
            if line.startswith("$TYPE_PRODUCTION_LINE"):
                line = "$TYPE_FACTORY"
        if line.startswith("$TYPE_"):
            seen_type = True
        out.append(line)
    if not seen_type:
        raise SystemExit("no type line in " + donor_path)
    return "\n".join(out) + "\n"


def localize_siblings(src, folder):
    """A DLC building can use a sibling's materials, technical_services_medium uses those of
    technical_services_small. Copy them in, with their textures, and point the config at them.
    The DLC's config also names variants its sibling does not have: those lines go, and the
    build says so."""
    path = os.path.join(folder, "renderconfig.ini")
    text = open(path, encoding="latin1", newline="").read()

    def one(m):
        head, sibling, name, rest = m.groups()
        origin = os.path.join(src, "..", sibling)
        if not os.path.isfile(os.path.join(origin, name)):
            print("  no such file in the DLC, line dropped:", sibling + "/" + name)
            return ""
        shutil.copyfile(os.path.join(origin, name), os.path.join(folder, name))
        if name.lower().endswith(".mtl"):
            mtl = open(os.path.join(origin, name), encoding="latin1").read()
            for tex in re.findall(r"\$TEXTURE_MTL[ \t]+\d+[ \t]+([^\s/\\]+)", mtl):
                shutil.copyfile(os.path.join(origin, tex), os.path.join(folder, tex))
        return head + name + rest

    text = re.sub(r"(?m)^([ \t]*[A-Z_0-9]+[ \t]+)\.\./([^/\s]+)/([^\s]+)([^\n]*\n?)", one, text)
    open(path, "w", encoding="latin1", newline="").write(text)


def build_early(media):
    """A whole DLC building folder, only building.ini rewritten."""
    for obj, spec in EARLY.items():
        src = os.path.join(media, "dlc3", "buildings", spec["donor"])
        if not os.path.isdir(src):
            raise SystemExit("Early Start donor not found: " + src)
        folder = os.path.join(ITEM, obj)
        shutil.rmtree(folder, ignore_errors=True)
        shutil.copytree(src, folder)
        localize_siblings(src, folder)
        with open(os.path.join(folder, "building.ini"), "w", encoding="latin1", newline="\n") as f:
            f.write(build_ini(os.path.join(src, "building.ini"), spec["head"]))
        # Left by the previous donor-based version of this building.
        for stale in (obj + ".mtl", obj + "_e.mtl"):
            path = os.path.join(ITEM, stale)
            if os.path.exists(path):
                os.remove(path)
        print("built", obj, "from dlc3", spec["donor"])


def item_description():
    """The Workshop text. The ini's string cannot hold a double quote."""
    text = open(os.path.join(HERE, "..", "docs", "workshop-description.txt"), encoding="utf-8").read().strip()
    if '"' in text:
        raise SystemExit("workshop-description.txt must not contain a double quote")
    return text


def main(game):
    media = os.path.join(game, "media_soviet")
    types = os.path.join(media, "buildings_types")
    meshes = os.path.join(media, "buildings")
    tools = os.path.join(media, "editor")
    os.makedirs(ITEM, exist_ok=True)
    for obj, spec in BUILDINGS.items():
        d = spec["donor"]
        folder = os.path.join(ITEM, obj)
        os.makedirs(folder, exist_ok=True)
        # The table says which mesh: a variant can share its sibling's, as the pedestrian stand does.
        table = open(os.path.join(media, "buildings", "buildingtypes.ini"), encoding="latin1").read()
        entry = re.search(r"^\$TYPE\s+" + re.escape(d) + r"\s*$(.*?)^\s*END\s*$", table, re.M | re.S)
        model = re.search(r"^\s*MODEL\s+(\S+)", entry.group(1), re.M) if entry else None
        mesh = os.path.join(media, model.group(1)) if model else os.path.join(meshes, d + ".nmf")
        shutil.copyfile(mesh, os.path.join(folder, "model.nmf"))
        for ext in ("bbox", "fire"):
            shutil.copyfile(os.path.join(types, d + "." + ext), os.path.join(folder, "building." + ext))
        shutil.copyfile(os.path.join(tools, "tool_" + d + ".png"), os.path.join(folder, "imagegui.png"))

        # Copied unchanged: their texture paths already name the game's files.
        mat, emi = donor_materials(media, d)
        mtl = obj + ".mtl"
        shutil.copyfile(os.path.join(media, mat), os.path.join(ITEM, mtl))
        emissive = ""
        if emi:
            e_name = obj + "_e.mtl"
            shutil.copyfile(os.path.join(media, emi), os.path.join(ITEM, e_name))
            emissive = " MATERIALEMISSIVE ../{}\n".format(e_name)
        with open(os.path.join(folder, "renderconfig.ini"), "w", newline="\n") as f:
            f.write(RENDER.format(mtl=mtl, emissive=emissive))
        with open(os.path.join(folder, "building.ini"), "w", encoding="latin1", newline="\n") as f:
            f.write(build_ini(os.path.join(types, d + ".ini"), spec["head"], spec.get("as_factory", False)))
        print("built", obj, "from", d, "emissive" if emissive else "")

    build_early(media)

    objs = "".join("$OBJECT_BUILDING {}\n".format(o) for o in list(BUILDINGS) + list(EARLY))
    # 9000000001 is the local test folder. tools/make_release.sh writes the real id,
    # 3835656082. Visibility and tags are what the game's upload window wrote for that item.
    with open(os.path.join(ITEM, "workshopconfig.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write('$ITEM_ID 9000000001\n\n$OWNER_ID 76561198005448678\n\n'
                '$ITEM_TYPE WORKSHOP_ITEMTYPE_BUILDING\n\n$VISIBILITY 0\n\n'
                '$TAGS 3\n$TAGS 5\n$TAGS 7\n\n' + objs + '\n$ITEM_NAME "WRSR Mail"\n\n'
                '$ITEM_DESC "' + item_description() + '"\n\n$END\n')


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"G:/SteamLibrary/steamapps/common/SovietRepublic")

