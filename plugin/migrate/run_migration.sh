#!/bin/bash
# The one-time stat migration on THIS install (the user starts it; the game must be closed).
# Dry run by default; add --apply to do it. See migrate.cpp for every step.
set -e
cd "$(dirname "$0")"
GAME="${GAME:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
SAVES="${SAVES:-$HOME/FI/SteamLibrary/steamapps/compatdata/2536520/pfx/drive_c/users/steamuser/Saved Games/Diablo II Resurrected/mods/D2RMM}"
STORE="$GAME/d2rloader/exalted-items/D2RMM"
./build.sh > /dev/null
exec ../build/native/exalted_migrate --saves "$SAVES" --tables "$STORE/data/global/excel" --stat-ids "$STORE/stat-ids.txt" --codes "$STORE/codes.txt" "$@"
