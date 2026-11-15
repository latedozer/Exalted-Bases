#!/bin/bash
# Checks that the generator writes exactly what a reference version of it writes: every
# adjusted file byte for byte, the codes, the summary line and the log lines. Used after a
# change that must not alter the result.
#
# The family lists handed to the picker hooks are not compared with the reference: since 0.9.1
# they leave out unique and set rows the game can never pick, which 0.9.0 wrongly counted (and
# so the "with unique drops" / "with set drops" counts in the output differ too). The lists
# are checked against the tables instead, by test/check_families.py.
#
#   test/same_output.sh <reference source dir> [mod data dir] [vanilla data dir]
#
# Nothing outside build/ is written.
set -u
cd "$(dirname "$0")/.."
ROOT="$(cd ../.. && pwd)"
REF="${1:?usage: same_output.sh <reference source dir> [mod data dir] [vanilla data dir]}"
MOD="${2:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected/mods/D2RMM/D2RMM.mpq/data}"
VAN="${3:-$ROOT/vanilla/data}"
N=build/native
W="$N/same"
mkdir -p "$N"
rm -rf "$W"
mkdir -p "$W"
g++ -std=c++20 -O2 -o "$N/gen_ref" "$REF/test/gen_test.cpp" "$REF/exalted_core.cpp" || { echo "the reference generator does not build"; exit 1; }
g++ -std=c++20 -O2 -Wall -Wextra -o "$N/gen_test" test/gen_test.cpp exalted_core.cpp || { echo "the generator does not build"; exit 1; }

FAILED=0
CASES=0
FILES=0
FAMILIES=0
# one <name> <data dir> [key=value ...]
one() {
	local name="$1" data="$2"
	shift 2
	mkdir -p "$W/ref-$name" "$W/new-$name"
	"$N/gen_ref" "$data" "$VAN" "$W/ref-$name" "$@" families="$W/ref-$name.families" savecodes="$W/ref-$name.codes" > "$W/ref-$name.out" 2>&1
	local refExit=$?
	"$N/gen_test" "$data" "$VAN" "$W/new-$name" "$@" downgrade=off upgradereq=9 families="$W/new-$name.families" savecodes="$W/new-$name.codes" > "$W/new-$name.out" 2>&1
	local newExit=$?
	CASES=$((CASES + 1))
	FILES=$((FILES + $(find "$W/ref-$name" -type f | wc -l)))
	local what=""
	[ "$refExit" -eq "$newExit" ] || what="$what exit-code"
	diff -r "$W/ref-$name" "$W/new-$name" > /dev/null || what="$what files"
	if [ -f "$W/ref-$name.codes" ] || [ -f "$W/new-$name.codes" ]; then
		cmp -s "$W/ref-$name.codes" "$W/new-$name.codes" || what="$what codes"
	fi
	# Output and log lines, without the counts of bases that have unique or set drops.
	local mask='s/ downgrades=[0-9]*//; s/ zone=[0-9]* zonerows=[0-9]*//; s/uniques=[0-9]* sets=[0-9]*/uniques=N sets=N/; s/[0-9]* with unique drops, [0-9]* with set drops/N with unique drops, N with set drops/'
	[ "$(sed "$mask" "$W/ref-$name.out")" = "$(sed "$mask" "$W/new-$name.out")" ] || what="$what output"
	# The family lists against the tables (only for runs on the mod's own, unadjusted files).
	if [ "$data" = "$MOD" ] && [ -s "$W/new-$name.families" ]; then
		local uniques=7 sets=7 arg
		for arg in "$@"; do
			case "$arg" in uniques=*) uniques="${arg#uniques=}" ;; sets=*) sets="${arg#sets=}" ;; esac
		done
		python3 test/check_families.py "$data" "$W/new-$name.families" "$W/new-$name.codes" "$uniques" "$sets" > "$W/new-$name.famcheck" 2>&1 || what="$what family-lists"
		FAMILIES=$((FAMILIES + 1))
	fi
	if [ -n "$what" ]; then
		echo "  DIFFERENT [$name]:$what"
		FAILED=$((FAILED + 1))
	fi
}

for max in 96 93 90 87 60 30; do one "max$max" "$MOD" "max=$max"; done
for mask in 4 2 1 0; do one "tiers$mask" "$MOD" "uniques=$mask" "sets=$mask"; done
one "tiers-mixed" "$MOD" uniques=6 sets=3
for step in 0 50 300; do one "step$step" "$MOD" "step=$step"; done
one "off" "$MOD" enabled=0
# Saved codes: one kept, one that clashes with another item, one belonging to a base that is away.
if [ -f "$W/ref-max96.codes" ]; then
	sed 's/^uap=.*/uap=zz9/' "$W/ref-max96.codes" > "$W/codes-keep.txt"
	one "codes-keep" "$MOD" "codes=$W/codes-keep.txt"
	sed 's/^uap=.*/uap=cap/' "$W/ref-max96.codes" > "$W/codes-clash.txt"
	one "codes-clash" "$MOD" "codes=$W/codes-clash.txt"
	(grep -v "^uap=" "$W/ref-max96.codes"; echo "zzz=b8p") > "$W/codes-away.txt"
	one "codes-away" "$MOD" "codes=$W/codes-away.txt"
fi
# A second set of input files: the game's own tables used as the mod (one data bank, no names, no models).
if [ -d "$VAN/global/excel" ]; then
	one "vanilla" "$VAN" max=96
	one "vanilla-87" "$VAN" max=87 uniques=4
fi
# What a run on the adjusted copies of an earlier run gives (generated rows in the input).
if [ -d "$W/ref-max96/global/excel" ]; then
	rm -rf "$W/again-in"
	mkdir -p "$W/again-in/global" "$W/again-in/local/lng" "$W/again-in/hd/items"
	cp -r "$MOD/global/excel" "$W/again-in/global/"
	cp -r "$MOD/local/lng/strings" "$W/again-in/local/lng/" 2>/dev/null
	cp "$MOD/hd/items/items.json" "$W/again-in/hd/items/" 2>/dev/null
	cp -r "$W/ref-max96/." "$W/again-in/"
	one "regenerate" "$W/again-in" max=93
	rm -rf "$W/again-in" # a copy of the mod's own tables: not something the generator wrote
fi

echo "  $CASES cases, $FILES files compared with $(basename "$REF"); family lists checked against the tables in $FAMILIES cases"
[ -f "$W/new-max96.famcheck" ] && cat "$W/new-max96.famcheck"
[ "$FAILED" -eq 0 ] && echo "  same output: ok" || echo "  same output: $FAILED case(s) DIFFER"
exit "$FAILED"
