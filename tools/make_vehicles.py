# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the mail truck pairs in item_vehicles/: the In truck only carries mail_raw
# (collection), the Out only mail_done (delivery). Every variant is one era; the model
# comes from the base game or the Early Start DLC, never from another mod.
#
# Not installed since a mail service buys any covered truck: tools/install_test.sh leaves
# this item out, and the plugin does not need it. Kept for a set of mail liveries.
#
# The scripts name mail_raw and mail_done: install them only together with the plugin
# and resources = 1.

import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "item_vehicles")
BASE = r"G:/SteamLibrary/steamapps/common/SovietRepublic/media_soviet/vehicles"

# (in name, out name, caption stem, source, first year, last year)
# source: ("base", folder) or ("dlc3", folder)
ERAS = [
    ("mail_truck_in_horse",  "mail_truck_out_horse",  "Mail cart",        ("dlc3", "h_covered"),                 1850, 1937),
    ("mail_truck_in_early",  "mail_truck_out_early",  "Mail truck 1913",  ("dlc3", "covered_russobalt_t40_65"),  1913, 1925),
    ("mail_truck_in_amo",    "mail_truck_out_amo",    "Mail truck 1924",  ("dlc3", "_Covered_AMO15"),            1924, 1937),
    ("mail_truck_in_1930",   "mail_truck_out_1930",   "Mail truck 1930s", ("dlc3", "covered_zis5"),              1933, 1958),
    ("mail_truck_in_1940",   "mail_truck_out_1940",   "Mail truck 1940s", ("base", "covered_pan_movic"),         1946, 1965),
    ("mail_truck_in_1960",   "mail_truck_out_1960",   "Mail truck 1960s", ("base", "covered_skd_706rt"),         1958, 1990),
    ("mail_truck_in_1970",   "mail_truck_out_1970",   "Mail truck 1970s", ("base", "covered_kmz_5320"),          1976, 3000),
]


def donor_path(source, base):
    if source[0] == "dlc3":
        return os.path.join(os.path.dirname(base), "dlc3", "vehicles", source[1])
    return os.path.join(base, source[1])


def build_script(text, caption, resource, first, last):
    text, n = re.subn(r'^\$NAME_STR .*$', '$NAME_STR "{}"'.format(caption), text, flags=re.M)
    if n == 0:
        # Base game scripts carry a language id instead of a literal name.
        text, n = re.subn(r'^\$NAME \d+.*$', '$NAME_STR "{}"'.format(caption), text, flags=re.M)
    if n != 1:
        raise SystemExit("expected one name line in the donor script")
    # The horse cart is GENERAL in its own script; every mail variant must end up
    # COVERED because the mail storages are class 0.
    text, n = re.subn(r'^\$RESOURCE_TRANSPORT_TYPE .*$',
                      '$RESOURCE_TRANSPORT_TYPE RESOURCE_TRANSPORT_COVERED', text, flags=re.M)
    if n != 1:
        raise SystemExit("expected one transport type line in the donor script")
    # Drop the donor's own cargo restrictions and skills, the mail variant decides both.
    text = re.sub(r'^\$RESOURCE_ALLOW_ONLY [^\r\n]*\r?\n?', '', text, flags=re.M)
    text = re.sub(r'^\$SKILL_[A-Z_]+ [^\r\n]*\r?\n?', '', text, flags=re.M)
    text, n = re.subn(r'^(\$RESOURCE_TRANSPORT_TYPE RESOURCE_TRANSPORT_COVERED[ \t]*\r?)$',
                      lambda m: m.group(1) + "\n$RESOURCE_ALLOW_ONLY " + resource +
                                "\n$SKILL_AMBULANCE 1", text, flags=re.M)
    if n != 1:
        raise SystemExit("expected one covered transport line in the donor script")
    text, n = re.subn(r'^\$AVAILABLE .*$', '$AVAILABLE {} {}'.format(first, last), text, flags=re.M)
    if n == 0:
        text = text.rstrip("\r\n") + "\n$AVAILABLE {} {}\n".format(first, last)
    return text


def fix_materials(src, dst):
    """Donor materials name textures relative to the donor's folder, often in a sibling
    (../open_pan_movic/diffuse_3.dds). Copy each into the truck's folder and point the
    material at the copy. A texture the donor itself does not have gets the game's blank
    for that slot, and the build says so."""
    blanks = {"0": "blankdiffuse.dds", "1": "blankspecular.dds", "2": "blankbump.dds"}

    def one(m):
        slot, ref = m.group(1), m.group(2)
        origin = os.path.normpath(os.path.join(src, ref))
        if not os.path.isfile(origin):
            print("  missing in donor, using a blank:", ref)
            return "$TEXTURE {} {}".format(slot, blanks.get(slot, "blankdiffuse.dds"))
        if os.path.dirname(ref):
            local = "ref_" + re.sub(r"[^A-Za-z0-9_.]+", "_", ref)
            shutil.copyfile(origin, os.path.join(dst, local))
            return "$TEXTURE_MTL {} {}".format(slot, local)
        return m.group(0)

    for f in os.listdir(dst):
        if f.lower().endswith(".mtl"):
            path = os.path.join(dst, f)
            text = open(path, encoding="latin1", newline="").read()
            text = re.sub(r"\$TEXTURE_MTL[ \t]+(\d+)[ \t]+([^\s]+)", one, text)
            open(path, "w", encoding="latin1", newline="").write(text)


def localize_models(text, src, dst, media):
    """Point every model line at a file in the truck's own folder. A path like
    dlc3/vehicles/x/alpha.nmf drew an empty mesh and crashed the engine in C3D_MESH::Render
    on 2026-11-06, the one truck with such an $ALPHA line. A model that exists nowhere loses
    its line, and its $LODDISTANCE with it: same empty mesh, only later, from far away."""
    out = []
    dropped = []
    for line in text.splitlines(True):
        m = re.match(r"^(\$[A-Z0-9_]+[ \t]+)([^\s]+\.nmf)\b(.*)$", line, re.S)
        if not m:
            out.append(line)
            continue
        token, ref, rest = m.groups()
        name = os.path.basename(ref)
        local = os.path.join(dst, name)
        if not os.path.isfile(local):
            origin = next((p for p in (os.path.join(src, ref), os.path.join(media, ref))
                           if os.path.isfile(p)), None)
            if origin is None:
                print("  no such model, line dropped:", token.strip(), ref)
                dropped.append(token.strip())
                continue
            shutil.copyfile(origin, local)
        out.append(token + name + rest)
    text = "".join(out)
    for token in dropped:
        lod = re.match(r"^\$LOD(\d+)$", token)
        if lod:
            text = re.sub(r"^\$LODDISTANCE" + lod.group(1) + r"\b[^\r\n]*\r?\n?", "", text, flags=re.M)
    return text


def main(base):
    names = []
    for in_name, out_name, stem, source, first, last in ERAS:
        src = donor_path(source, base)
        if not os.path.isdir(src):
            raise SystemExit("donor not found: " + src)
        for name, side, resource, caption in (
            (in_name,  "In",  "mail_raw",  "[MAIL] " + stem + " In (collection)"),
            (out_name, "Out", "mail_done", "[MAIL] " + stem + " Out (delivery)"),
        ):
            dst = os.path.join(OUT, name)
            # A clean folder: files of an earlier donor must not stay behind.
            # The contents and not the folder: Windows refuses to remove a folder some shell
            # stands in, and a half built set must not pass for a finished one.
            os.makedirs(dst, exist_ok=True)
            for old in os.listdir(dst):
                os.remove(os.path.join(dst, old))
            for f in os.listdir(src):
                if f != "script.ini":
                    shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))
            fix_materials(src, dst)
            text = open(os.path.join(src, "script.ini"), encoding="latin1", newline="").read()
            with open(os.path.join(dst, "script.ini"), "w", encoding="latin1", newline="") as f:
                f.write(localize_models(build_script(text, caption, resource, first, last),
                                        src, dst, os.path.dirname(base)))
            print("built", name, "carries only", resource, "ages", first, last)
            names.append(name)

    objs = "".join("$OBJECT_VEHICLE {}\n".format(t) for t in names)
    with open(os.path.join(OUT, "workshopconfig.ini"), "w", newline="\n") as f:
        f.write('$ITEM_ID 9000000002\n\n$OWNER_ID 76561198005448678\n\n'
                '$ITEM_TYPE WORKSHOP_ITEMTYPE_VEHICLE\n\n$VISIBILITY 2\n\n' + objs +
                '\n$ITEM_NAME "WRSR Mail trucks (dev)"\n\n'
                '$ITEM_DESC "Local development item, unpublished."\n\n$END\n')


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else BASE)


