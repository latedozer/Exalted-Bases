#!/bin/bash
# The stat migration, on COPIES of the user's saves and files (made here, in build/native/mig;
# the live files are only read). Never commit build/.
#   test/check_migrate.sh [game folder] [saves folder]
set -u
cd "$(dirname "$0")/.."
ROOT="$PWD"; while [ "$ROOT" != / ] && [ ! -d "$ROOT/vanilla/data" ]; do ROOT="$(dirname "$ROOT")"; done
GAME="${1:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
SAVES="${2:-$HOME/FI/SteamLibrary/steamapps/compatdata/2536520/pfx/drive_c/users/steamuser/Saved Games/Diablo II Resurrected/mods/D2RMM}"
MOD="$GAME/mods/D2RMM/D2RMM.mpq/data"
STORE="$GAME/d2rloader/exalted-items/D2RMM"
VAN="$ROOT/vanilla/data"
N=build/native
M=$N/mig
FAILED=0
bad() { echo "FAILED: $1"; FAILED=$((FAILED + 1)); }
migrate/build.sh > /dev/null || { echo "the tool does not build"; exit 1; }
g++ -std=c++20 -O2 -Wall -Wextra -o "$N/scan_saves" test/scan_saves.cpp save_reader.cpp || { echo "the save lister does not build"; exit 1; }
g++ -std=c++20 -O2 -o "$N/gen_test" test/gen_test.cpp exalted_core.cpp || exit 1

# The saves in the 1.2.0 layout. Once this install has been migrated (its stat-ids.txt says
# packed), they are the copies in the migration's own backup folder (read only), under their
# own names in build/native/mig-src; the tables are then those 1.3.0 makes for that layout.
SRC="$SAVES"
LEGACY_TABLES="$STORE/data/global/excel"
if tr -d '\r' < "$STORE/stat-ids.txt" 2>/dev/null | grep -qx "layout=packed"; then
	B="$(ls -d "$SAVES"/exalted-migration-backup-* 2>/dev/null | sort | head -1)"
	[ -n "$B" ] || { echo "this install is migrated and has no migration backup with the 1.2.0 saves: nothing to test on"; exit 1; }
	SRC=$N/mig-src
	rm -rf "$SRC" && mkdir -p "$SRC"
	for f in "$B"/*; do n="$(basename "$f")"; cp "$f" "$SRC/${n#*-}"; done
	LEGACY_TABLES=""
	echo "== this install is migrated: the 1.2.0 saves from $(basename "$B")"
fi
fresh() {
	rm -rf "$M" && mkdir -p "$M/saves" "$M/store"
	cp "$SRC"/*.d2s "$SRC"/*.d2i "$M/saves/"
	if [ "$SRC" = "$SAVES" ]; then cp "$STORE/codes.txt" "$M/store/"; else cp "$SRC/codes.txt" "$M/store/"; fi
	# the map 1.3.0 writes at its first start on this install (codes, no map: the 1.2.0 layout)
	"$N/gen_test" "$MOD" "$VAN" "$M/gen-before" crystal=1 codes="$M/store/codes.txt" > /dev/null
	cp "$M/gen-before.stat-ids.txt" "$M/store/stat-ids.txt"
	if [ -n "$LEGACY_TABLES" ]; then cp -r "$LEGACY_TABLES" "$M/tables"; else cp -r "$M/gen-before/global/excel" "$M/tables"; fi
}
run() { EXALTED_MIGRATE_NO_GAME_CHECK=1 "$N/exalted_migrate" --saves "$M/saves" --tables "$M/tables" --stat-ids "$M/store/stat-ids.txt" --codes "$M/store/codes.txt" "$@"; }
sums() { (cd "$M" && find saves store -type f -not -path '*backup*' -print0 | sort -z | xargs -0 md5sum); }

echo "== the 1.3.0 tables of this install (1.2.0 layout) = what the saves were written with"
cmp -s "$M/gen-before/global/excel/itemstatcost.txt" "$STORE/data/global/excel/itemstatcost.txt" 2>/dev/null \
	|| { fresh; cmp -s "$M/gen-before/global/excel/itemstatcost.txt" "$M/tables/itemstatcost.txt" && echo "  same itemstatcost.txt as the stored copy" || echo "  note: the stored copy differs from a 1.3.0 run (made with an older mod state); the tool reads the stored copy"; }

echo "== dry run: nothing written"
fresh
before="$(sums)"
run --out "$M/dry" | sed 's/^/  /'
[ "$before" = "$(sums)" ] && [ -z "$(ls -d "$M"/saves/exalted-migration-backup-* 2>/dev/null)" ] && echo "  dry run changed nothing" || bad "the dry run changed a file"

echo "== refused while the game runs, refused without stat-ids.txt"
( exec -a "D2R.exe" sleep 5 ) & sleeper=$!
sleep 0.2
"$N/exalted_migrate" --saves "$M/saves" --tables "$M/tables" --stat-ids "$M/store/stat-ids.txt" --codes "$M/store/codes.txt" --apply | grep -q "The game is running" && echo "  refused while a D2R.exe runs" || bad "it ran while the game was running"
kill $sleeper 2>/dev/null; wait $sleeper 2>/dev/null
mv "$M/store/stat-ids.txt" "$M/store/x" && run --apply | grep -q "start the game once with Exalted Items 1.3.0" && echo "  refused without the map 1.3.0 writes" || bad "it ran without stat-ids.txt"
mv "$M/store/x" "$M/store/stat-ids.txt"
[ "$before" = "$(sums)" ] || bad "a refused run changed a file"

echo "== a failed read-back puts everything back"
EXALTED_MIGRATE_TEST_FAIL=1 run --apply > "$M/fail.out"
grep -q "every file put back" "$M/fail.out" && [ "$before" = "$(sums)" ] && echo "  every file as before, backup folder kept" || { cat "$M/fail.out"; bad "putting back after a failure"; }

echo "== the real run, on the copies"
fresh
run --apply | sed 's/^/  /'
backup="$(ls -d "$M"/saves/exalted-migration-backup-* | head -1)"
[ -n "$backup" ] && [ "$(ls "$backup" | wc -l)" -ge 16 ] && echo "  backup folder with every save + stat-ids.txt + codes.txt" || bad "backup folder"
for f in "$M"/saves/*.d2s "$M"/saves/*.d2i; do
	n="$(basename "$f")"; orig="$SRC/$n"
	if ! cmp -s "$f" "$orig"; then
		case "$n" in Hellpal.d2s) ;; *) bad "$n changed although it holds none of the stats";; esac
	fi
done
cmp -s "$M/saves/Hellpal.d2s" "$SRC/Hellpal.d2s" && bad "Hellpal.d2s was not changed"
[ "$(stat -c %s "$M/saves/Hellpal.d2s")" = "$(stat -c %s "$SRC/Hellpal.d2s")" ] && echo "  only Hellpal.d2s changed, same size" || bad "Hellpal.d2s size"
# the bytes that differ: only inside the moved numbers and the checksum
python3 - "$SRC/Hellpal.d2s" "$M/saves/Hellpal.d2s" <<'PY'
import sys
a, b = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
bits = [i for i in range(len(a) * 8) if (a[i >> 3] >> (i & 7) & 1) != (b[i >> 3] >> (i & 7) & 1)]
outside = [i for i in bits if not 96 <= i < 128]
print("  %d bits differ: %d in the checksum, %d elsewhere (a 9-bit number changes at most 9)" % (len(bits), len(bits) - len(outside), len(outside)))
PY
echo "  stat-ids.txt now:"; tr -d '\r' < "$M/store/stat-ids.txt" | grep -v '^#' | tr '\n' ' ' | fold -w 110 | sed 's/^/    /'; echo
echo "== the next start: 1.3.0 makes its tables from the new map, and every save reads with them"
"$N/gen_test" "$MOD" "$VAN" "$M/gen-after" crystal=1 codes="$M/store/codes.txt" statids="$M/store/stat-ids.txt" > "$M/gen-after.out"
grep -q "changed=0" "$M/gen-after.out" && cmp -s "$M/gen-after.stat-ids.txt" "$M/store/stat-ids.txt" && echo "  the plugin keeps the migrated map as it is" || bad "the plugin changed the migrated map"
rows="$(($(wc -l < "$M/gen-after/global/excel/itemstatcost.txt") - 1))"
grep -c "xlt_unused_" "$M/gen-after/global/excel/itemstatcost.txt" | grep -qx 0 && echo "  itemstatcost.txt: $rows stats, no xlt_unused_ filler rows" || bad "filler rows are still there"
cp "$M/gen-after/global/excel/itemstatcost.txt" "$M/tables-after.txt"
"$N/scan_saves" "$M/saves" "$M/tables" "$M/gen-after/global/excel/itemstatcost.txt" 375 > "$M/after.txt"
"$N/scan_saves" "$SRC" "$M/tables" "$M/tables/itemstatcost.txt" 375 > "$M/before.txt"
grep -q UNREADABLE "$M/after.txt" && bad "a migrated save does not read with the new tables"
sed -n 's/^stat/  after: stat/p' "$M/after.txt"
sed -n 's/^stat/  before: stat/p' "$M/before.txt"
python3 - "$M/before.txt" "$M/after.txt" "$M/store/stat-ids.txt" <<'PY' || exit 1
import sys
ids = {}
for l in open(sys.argv[3]):
    l = l.strip()
    if l.startswith("marker="): ids[500] = int(l[7:])
    if l.startswith("roll."):
        k, n = l[5:].split("="); ids[499 - int(k)] = int(n)
def items(path):
    out = []
    for l in open(path):
        if l.startswith("   "): out.append(l.split(": stat ")[0] + "|" + l.split(": stat ")[1].strip())
    return out
b, a = items(sys.argv[1]), items(sys.argv[2])
moved = []
for line in b:
    head, stat = line.split("|"); n, v = stat.split(" = ")
    moved.append(head + "|%d = %s" % (ids[int(n)], v))
ok = sorted(moved) == sorted(a)
print("  every saved marker/roll value is on the same item at its new number: %s" % ("yes" if ok else "NO"))
sys.exit(0 if ok else 1)
PY
[ $? = 0 ] || bad "values moved"
echo "== a second run: nothing to do"
run --apply | grep -q "already in the packed layout" && echo "  refused: already migrated" || bad "a second run did something"

[ $FAILED = 0 ] && echo "ALL MIGRATION CHECKS PASSED" || echo "$FAILED MIGRATION CHECK(S) FAILED"
exit $FAILED
