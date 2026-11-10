#!/bin/sh
# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
#
# Makes the folder to publish, out/release/<item id>, from item/, and checks what a stranger
# would get. Uploads nothing. Run build.sh and tools/make_buildings.py first.
#   tools/make_release.sh <workshop item id>
set -e
cd "$(dirname "$0")/.."
ID="$1"
case "$ID" in
    ''|*[!0-9]*) echo "usage: $0 <workshop item id>"; exit 2 ;;
esac

OUT="out/release/$ID"
rm -rf "$OUT"
mkdir -p "out/release"
cp -r item "$OUT"
sed -i "s/^\$ITEM_ID .*/\$ITEM_ID $ID/" "$OUT/workshopconfig.ini"
[ -f previewimage.png ] && cp previewimage.png "$OUT/"

BAD=0
fail() { echo "FAIL: $1"; BAD=1; }

grep -q "^\$ITEM_ID $ID" "$OUT/workshopconfig.ini" || fail "item id not written"
grep -q '^\$ITEM_NAME "WRSR Mail"$' "$OUT/workshopconfig.ini" || fail "item name"
grep -q '^resources = 1' "$OUT/plugins/mail.ini" || fail "resources must be 1 in mail.ini"
# The game reads $ITEM_DESC into a stack buffer of about 9000 bytes and dies with 0xC0000409
# when it is longer (seen 2026-11-08 at 10082 bytes, opening the items in development).
# Cyrillic takes two bytes a letter in UTF-8, so count bytes, not characters.
DESC_BYTES=$(python -I -c "import sys;s=open(sys.argv[1],encoding='utf-8').read();i=s.index('\$ITEM_DESC \"')+12;print(len(s[i:s.index(chr(34),i)].encode()))" "$OUT/workshopconfig.ini")
[ "$DESC_BYTES" -le 8000 ] || fail "the description is $DESC_BYTES bytes, keep it under 8000"
echo "description: $DESC_BYTES bytes"
[ -f "$OUT/previewimage.png" ] || fail "no previewimage.png in the project folder"
[ -f "$OUT/plugins/mail.dll" ] || fail "no plugin"

# Every building the item names has a folder, and every folder is named.
for o in $(grep '^\$OBJECT_BUILDING' "$OUT/workshopconfig.ini" | tr -d '\r' | cut -d' ' -f2); do
    [ -f "$OUT/$o/building.ini" ] || fail "$o is named but has no building.ini"
done
for d in "$OUT"/*/; do
    n=$(basename "$d")
    case "$n" in plugins|vfs) continue ;; esac
    grep -q "^\$OBJECT_BUILDING $n" "$OUT/workshopconfig.ini" || fail "$n is not named in the config"
    f="$d/building.ini"
    grep -q '^\$NAME_STR "\[MAIL\] ' "$f" || fail "$n has no [MAIL] in its name"
    grep -qE '^\$(PARTICLE|POLLUTION_)' "$f" && fail "$n pollutes"
    # Every material it uses is there, ours or next to it.
    for m in $(tr -d '\r' < "$d/renderconfig.ini" | grep -E '^ *(MATERIAL|MATERIALEMISSIVE|MODEL) ' | awk '{print $2}'); do
        [ -f "$d/$m" ] || fail "$n: $m is missing"
    done
done

# Nothing of this machine or of the work on it.
if grep -rIlE 'C:\\|G:/|/c/Projects|workshop/content|\(dev\)' "$OUT" --include='*.ini' --include='*.mtl' >/tmp/mail_release_paths 2>/dev/null \
   && [ -s /tmp/mail_release_paths ]; then
    fail "paths or dev text in: $(tr '\n' ' ' < /tmp/mail_release_paths)"
fi
strings -a "$OUT/plugins/mail.dll" 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+-dev' && fail "the plugin says it is a dev build"

echo "size: $(du -sh "$OUT" | cut -f1), buildings: $(grep -c '^\$OBJECT_BUILDING' "$OUT/workshopconfig.ini")"
[ "$BAD" = 0 ] && echo "release folder ok: $OUT" || { echo "not ready"; exit 1; }
