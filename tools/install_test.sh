#!/bin/sh
# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
#
# Load test install under RML. Close the game first.
#   tools/install_test.sh install    copies the item, resources = 1, keeps a backup
#   tools/install_test.sh remove     removes the item, restores the backup
#
# While installed: existing saves do not load (the resource count is in the save), and
# starting the game WITHOUT RML can crash, since the buildings name resources only the
# plugin provides. Remove it to go back to normal play.
set -e
WIP="/g/SteamLibrary/steamapps/common/SovietRepublic/media_soviet/workshop_wip"
BACKUP="/c/Projects/wrsr-mail-backup-before-load-test"
cd "$(dirname "$0")/.."

if tasklist //FI "IMAGENAME eq SOVIET64.exe" 2>/dev/null | grep -q SOVIET64; then
    echo "the game is running, close it first"; exit 1
fi

case "$1" in
install)
    if [ -d "$WIP/9000000001" ] && [ ! -d "$BACKUP" ]; then
        cp -r "$WIP/9000000001" "$BACKUP"
        echo "backup: $BACKUP"
    fi
    rm -rf "$WIP/9000000001" "$WIP/9000000002"
    cp -r item "$WIP/9000000001"
    sed -i 's/^resources = .*/resources = 1/' "$WIP/9000000001/plugins/mail.ini"
    grep -n '^resources' "$WIP/9000000001/plugins/mail.ini"
    echo "installed, start a NEW map under RML"
    ;;
remove)
    rm -rf "$WIP/9000000001" "$WIP/9000000002"
    if [ -d "$BACKUP" ]; then cp -r "$BACKUP" "$WIP/9000000001"; echo "restored from $BACKUP"; fi
    echo "removed"
    ;;
*)
    echo "usage: $0 install|remove"; exit 2
    ;;
esac
