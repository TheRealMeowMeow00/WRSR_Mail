#!/bin/sh
# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
#
# Puts the build into the live item, the folder the game loads and the one that is published
# from: media_soviet/workshop_wip/<item id>. Close the game first.
#   tools/install_test.sh install            copies item/ over it, keeps a dated backup
#   tools/install_test.sh restore <backup>   puts a backup folder back
#
# The item's own workshopconfig.ini and previewimage.png are left alone: they carry the real
# item id and the published text. After an install the folder holds an untested build, so do
# not upload from it before the build has been seen in the game.
set -e
ID="${MAIL_ITEM_ID:-3835656082}"
WIP="/g/SteamLibrary/steamapps/common/SovietRepublic/media_soviet/workshop_wip"
LIVE="$WIP/$ID"
cd "$(dirname "$0")/.."

if tasklist //FI "IMAGENAME eq SOVIET64.exe" 2>/dev/null | grep -q SOVIET64; then
    echo "the game is running, close it first"; exit 1
fi

case "$1" in
install)
    [ -d "$LIVE" ] || { echo "no live item at $LIVE"; exit 1; }
    [ -f item/plugins/mail.dll ] || { echo "no build in item/, run build.sh first"; exit 1; }
    BACKUP="/c/Projects/wrsr-mail-backup-wip-$ID-$(date +%Y-%m-%d-%H%M)"
    cp -r "$LIVE" "$BACKUP"
    echo "backup: $BACKUP"
    for f in item/*; do
        case "$(basename "$f")" in workshopconfig.ini) continue ;; esac
        cp -r "$f" "$LIVE/"
    done
    sed -i 's/^resources = .*/resources = 1/' "$LIVE/plugins/mail.ini"
    grep -n '^resources' "$LIVE/plugins/mail.ini"
    cmp item/plugins/mail.dll "$LIVE/plugins/mail.dll" && echo "installed into $LIVE"
    ;;
restore)
    [ -d "$2" ] && [ -f "$2/workshopconfig.ini" ] || { echo "usage: $0 restore <backup folder>"; exit 2; }
    rm -rf "$LIVE"
    cp -r "$2" "$LIVE"
    echo "restored $LIVE from $2"
    ;;
*)
    echo "usage: $0 install | restore <backup folder>"; exit 2
    ;;
esac
