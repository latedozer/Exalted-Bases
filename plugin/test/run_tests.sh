#!/bin/bash
# Runs every check that does not need the game. Nothing outside build/ is written.
#
#   test/run_tests.sh [mod data dir] [vanilla data dir] [game image dump]
#
# Defaults are this machine's paths. The DLL part needs wine and a built DLL (./build.sh).
set -u
cd "$(dirname "$0")/.."
ROOT="$(cd ../.. && pwd)"
MOD="${1:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected/mods/D2RMM/D2RMM.mpq/data}"
VAN="${2:-$ROOT/vanilla/data}"
DUMP="${3:-$ROOT/bin/D2RLoader_6ab3782c_dump.exe}"
N=build/native
FAILED=0
note() { printf '\n== %s ==\n' "$1"; }
bad() { echo "FAILED: $1"; FAILED=$((FAILED + 1)); }

mkdir -p "$N"
g++ -std=c++20 -O2 -Wall -Wextra -o "$N/gen_test" test/gen_test.cpp exalted_core.cpp || { echo "the generator does not build"; exit 1; }
g++ -std=c++20 -O2 -Wall -Wextra -o "$N/scan_test" test/scan_test.cpp || { echo "the picker search does not build"; exit 1; }
g++ -std=c++20 -O2 -Wall -Wextra -o "$N/match_test" test/match_test.cpp || { echo "the path compare test does not build"; exit 1; }
g++ -std=c++20 -O2 -Wall -Wextra -DEXALTED_SELF_TEST -o "$N/core_test" test/core_test.cpp exalted_core.cpp || { echo "the table reading test does not build"; exit 1; }
gen() { local out="$1"; shift; rm -rf "$N/$out"; "$N/gen_test" "$MOD" "$VAN" "$N/$out" "$@"; }

REF="${REF:-../exalted-plugin-0.9.0-backup}"
note "generator: the same output as $(basename "$REF"), byte for byte"
if [ -f "$REF/exalted_core.cpp" ]; then
	test/same_output.sh "$REF" "$MOD" "$VAN" || bad "output differs from $(basename "$REF")"
else
	echo "  skipped: no reference sources at $REF"
fi

note "family lists for the picker hooks: exactly the rows the game can pick"
gen famcheck families="$N/famcheck.families" savecodes="$N/famcheck.codes" >/dev/null
python3 test/check_families.py "$MOD" "$N/famcheck.families" "$N/famcheck.codes" || bad "family lists"
rm -rf "$N/famcheck"

note "table and JSON reading, quick ways against the plain way"
"$N/core_test" "$MOD" "$VAN" | sed 's/^/  /' | tail -5
"$N/core_test" "$MOD" "$VAN" >/dev/null || bad "table and JSON reading"

note "path compare of the file redirect"
"$N/match_test" | sed 's/^/  /'
"$N/match_test" >/dev/null || bad "path compare"

note "generator: tables for a range of highest levels"
for max in 96 93 90 87 60 30; do
	gen "max$max" "max=$max" | tail -1 | grep -q "ok=1" || bad "generator run, max=$max"
	python3 test/check_tables.py "$MOD" "$N/max$max" | grep -E "FAIL|RESULT|exalted bases:|loot rows:|not written" | sed "s/^/  [max $max] /"
	python3 test/check_tables.py "$MOD" "$N/max$max" >/dev/null || bad "table checks, max=$max"
done

note "only in terror zones: the condition on the exalted rows, and nothing else changed"
for max in 96 90 87; do
	gen "zone$max" "max=$max" zone=1 | tail -1 | grep -q "ok=1 .* zone=1" || bad "generator run with the terror zone setting, max=$max"
	python3 test/check_zone.py "$N/max$max" "$N/zone$max" | sed "s/^/  [max $max]/"
	python3 test/check_zone.py "$N/max$max" "$N/zone$max" >/dev/null || bad "terror zone checks, max=$max"
	python3 test/check_tables.py "$MOD" "$N/zone$max" >/dev/null || bad "table checks with the terror zone setting, max=$max"
done

note "generator: same input, same output; disabled writes nothing"
gen again max=96 >/dev/null
diff -r "$N/max96" "$N/again" >/dev/null && echo "  two runs identical" || bad "two runs differ"
gen off enabled=0 >/dev/null
[ -z "$(find "$N/off" -type f 2>/dev/null)" ] && echo "  disabled: no files" || bad "files written while disabled"

note "generator: unique and set tables are never written"
[ -z "$(find "$N" -iname 'uniqueitems.txt' -o -iname 'setitems.txt' -o -iname 'sets.txt')" ] && echo "  none" || bad "a unique or set table was written"

note "item code legend: every base and material once, with its elite code and English name"
gen legend legend="$N/legend.txt" savecodes="$N/legend.codes" >/dev/null
python3 test/check_legend.py "$N/legend.txt" "$N/legend.codes" "$N/legend/local/lng/strings/item-names.json" | tail -4 | sed 's/^/  /'
python3 test/check_legend.py "$N/legend.txt" "$N/legend.codes" "$N/legend/local/lng/strings/item-names.json" >/dev/null || bad "legend"

note "loot filter: the Exalted box in the rule panel's layout"
FL=global/ui/layouts/lootfilteritempanelhd.json
if [ -f "$VAN/$FL" ]; then
	python3 test/check_filter.py "$VAN/$FL" "$N/max96/$FL" | sed 's/^/  /'
	python3 test/check_filter.py "$VAN/$FL" "$N/max96/$FL" >/dev/null || bad "the Exalted box in the layout"
	gen nobox filterbox=0 >/dev/null
	[ ! -e "$N/nobox/$FL" ] && echo "  setting off: the layout is not written" || bad "the layout was written with the box turned off"
	gen twice >/dev/null
	mkdir -p "$N/twicemod/global/ui/layouts" && cp "$N/twice/$FL" "$N/twicemod/global/ui/layouts/"
	rm -rf "$N/twiceout"; "$N/gen_test" "$N/twicemod" "$VAN" "$N/twiceout" >/dev/null 2>&1
	[ ! -e "$N/twiceout/$FL" ] && echo "  a layout that already has the box is left as it is" || bad "a layout that has the box was written again"
	rm -rf "$N/nobox" "$N/twice" "$N/twicemod" "$N/twiceout"
else
	echo "  skipped: no $FL in $VAN"
fi

note "exalted base codes"
gen c1 savecodes="$N/codes1.txt" >/dev/null
gen c2 codes="$N/codes1.txt" savecodes="$N/codes2.txt" >/dev/null
diff -r "$N/c1" "$N/c2" >/dev/null && diff "$N/codes1.txt" "$N/codes2.txt" >/dev/null && echo "  codes survive a restart" || bad "codes changed between runs"
code_of() { awk -F'\t' -v n="$2" 'NR==1{for(i=1;i<=NF;i++) h[$i]=i} $1==n {print $(h["code"])}' "$1"; }
sed 's/^uap=.*/uap=zz9/' "$N/codes1.txt" > "$N/codes_keep.txt"
gen c3 codes="$N/codes_keep.txt" >/dev/null
[ "$(code_of "$N/c3/global/excel/armor.txt" '[xlt] Shako')" = "zz9" ] && echo "  a saved code is kept" || bad "a saved code was not kept"
(grep -v "^uap=" "$N/codes1.txt"; echo "zzz=b8p") > "$N/codes_reserved.txt"
gen c5 codes="$N/codes_reserved.txt" >/dev/null
[ "$(code_of "$N/c5/global/excel/armor.txt" '[xlt] Shako')" != "b8p" ] && echo "  the code of a base that is away is not given out" || bad "an absent base's code was reused"

note "Restoration Crystal: the item, the restore recipes, the boss loot rows, the picture"
CRY="python3 test/check_crystal.py"
gen cry0 >/dev/null
gen cry1 crystal=1 shard=0 | tail -3 | grep -q "ok=1" || bad "generator run with the crystal"
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry1" | sed 's/^/  /'
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry1" >/dev/null || bad "crystal checks"
gen cry2 crystal=1 shard=0 >/dev/null
diff -r "$N/cry1" "$N/cry2" >/dev/null && echo "  two runs identical" || bad "two runs with the crystal differ"
gen cry3 crystal=1 shard=0 restoreoff=belts.maximum_mana restoreoff=gloves.attack_speed restoreoff=gloves.attack_rating restoreoff=gloves.faster_hit_recovery restoreoff=gloves.requirements >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry3" off=belts.maximum_mana,gloves.attack_speed,gloves.attack_rating,gloves.faster_hit_recovery,gloves.requirements >/dev/null \
	&& echo "  attributes turned off are left out; a kind with all of them off has no restore recipe" || bad "attributes turned off"
gen cry4 crystal=1 shard=0 crystaldrop=50 >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry4" drop=50 >/dev/null && echo "  drop weight 50%: half the essence's weight" || bad "drop weight setting"
gen cry5 crystal=1 shard=0 crystaldrop=0 restore=0 >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry5" drop=0 restore=0 >/dev/null && echo "  drops off and restoring off: no loot row and no stat table touched" || bad "drops and restoring off"
gen cry6 crystal=1 shard=0 restoreline=0 >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry6" line=0 >/dev/null && echo "  without the \"Restored\" line: the marker has no text" || bad "restored line off"
gen cry6b crystal=1 shard=0 crystalcolor=none >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry6b" color=none >/dev/null && echo "  name colour: orange like the game's materials by default; none leaves the name plain" || bad "name colour"
gen cry6c crystal=1 shard=0 crystalslot=0 >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry6c" slot=0 >/dev/null && [ -z "$(find "$N/cry6c/global/ui" -iname "bankexpansion*" 2>/dev/null)" ] && echo "  stash slot off: an ordinary item, no stash layout written" || bad "stash slot off"
# The crystal's slot in the stash's stacking tabs: in the mod's layouts, and in the game's own
# where a mod has none (a stand-in mod folder without its ui files).
LAY=global/ui/layouts
SLOTCODE="$(gen cry1s crystal=1 shard=0 | sed -n "s/.*crystal='\([a-z0-9]*\)'.*/\1/p")"
for layout in "$LAY/bankexpansionlayouthd.json" "$LAY/controller/bankexpansionlayouthd.json"; do
	if [ -f "$MOD/$layout" ]; then
		python3 test/check_slot.py "$MOD/$layout" "$N/cry1s/$layout" "$SLOTCODE" place=400:300 | sed 's/^/  mod layout: /'
		python3 test/check_slot.py "$MOD/$layout" "$N/cry1s/$layout" "$SLOTCODE" place=400:300 >/dev/null 2>&1 || bad "stash slot in the mod's $layout"
	fi
done
if [ -f "$MOD/hd/global/ui/panel/stash/advancedstash_expanded_cube.sprite" ]; then
	python3 - "$MOD/hd/global/ui" "$N/cry1s/hd/global/ui" <<'PY' && echo "  slot frame: painted into copies of the stash background (mouse and controller, both sizes), nothing else changed" || bad "slot frame in the stash background"
import struct, sys
for name, box in (("panel/stash/advancedstash_expanded_cube.sprite", 100), ("panel/stash/advancedstash_expanded_cube.lowend.sprite", 50),
                  ("controller/panel/stash/v2/advancedstash_expanded_cube_controller.sprite", 100), ("controller/panel/stash/v2/advancedstash_expanded_cube_controller.lowend.sprite", 50)):
    a = open(sys.argv[1] + "/" + name, "rb").read(); b = open(sys.argv[2] + "/" + name, "rb").read()
    assert len(a) == len(b) and a[:40] == b[:40], name
    w, h = struct.unpack_from("<II", a, 8)
    xs = set(); ys = set()
    for y in range(h):
        ra = a[40 + y * w * 4:40 + (y + 1) * w * 4]; rb = b[40 + y * w * 4:40 + (y + 1) * w * 4]
        if ra != rb:
            ys.add(y)
            xs.update(x for x in range(w) if ra[x * 4:x * 4 + 4] != rb[x * 4:x * 4 + 4])
    assert xs and max(xs) - min(xs) < box and max(ys) - min(ys) < box, name + ": changed area %s" % ((min(xs), min(ys), max(xs), max(ys)) if xs else None,)
    # the painted frame has a border line on its left edge
    x0, y0 = min(xs), min(ys)
    edge = [b[40 + (y * w + x) * 4:40 + (y * w + x) * 4 + 3] for y in range(y0 + 4, y0 + box - 8) for x in range(x0, x0 + 4)]
    assert max(max(p) for p in edge) > 60, name + ": no frame line"
PY
fi
if [ -f "$VAN/$LAY/bankexpansionlayouthd.json" ]; then
	rm -rf "$N/noui" && mkdir -p "$N/noui/global"
	for d in "$MOD"/*; do [ "$(basename "$d")" != global ] && ln -s "$d" "$N/noui/"; done
	for d in "$MOD"/global/*; do [ "$(basename "$d")" != ui ] && ln -s "$d" "$N/noui/global/"; done
	rm -rf "$N/cry1v" && "$N/gen_test" "$N/noui" "$VAN" "$N/cry1v" crystal=1 shard=0 >/dev/null
	for layout in "$LAY/bankexpansionlayouthd.json" "$LAY/controller/bankexpansionlayouthd.json"; do
		python3 test/check_slot.py "$VAN/$layout" "$N/cry1v/$layout" "$SLOTCODE" | sed 's/^/  game layout: /'
		python3 test/check_slot.py "$VAN/$layout" "$N/cry1v/$layout" "$SLOTCODE" >/dev/null 2>&1 || bad "stash slot in the game's own $layout"
	done
	rm -rf "$N/noui"
else
	echo "  stash slot in the game's own layout: skipped (no layout in the test data)"
fi
gen cry6d crystal=1 shard=0 strip=0 >/dev/null
$CRY "$MOD" "$VAN" "$N/cry0" "$N/cry6d" strip=0 >/dev/null && echo "  strip on downgrade off: no roll stats, no twins, recipes as before 0.11" || bad "strip on downgrade off"
g++ -std=c++20 -O2 -Wall -Wextra -o "$N/strip_test" test/strip_test.cpp || bad "strip test build"
for excel in global/excel global/excel/base; do "$N/strip_test" "$N/cry1/$excel" || bad "taking a restore off ($excel)"; done
gen cry7 crystal=1 shard=0 recipes=crystal savecodes="$N/crycodes1.txt" >/dev/null
gen cry7r recipes=crystal 2>&1 | grep -c "names the crystal" | grep -q "^12$" && echo "  crystal off: the six recipes that name it are left out (and said so) in both table sets" || bad "recipes naming a crystal that is off"
CODE="$(sed -n 's/^\*crystal=//p' "$N/crycodes1.txt")"
[ -n "$CODE" ] && [ "$(awk -F'\t' -v c="$CODE" '$1 ~ /Elite Unique Weapon -> Exalted/ {n += ($13 == c && $11 == 2)} END {print n + 0}' "$N/cry7/global/excel/cubemain.txt")" = "1" ] \
	&& echo "  the word crystal in an upgrade recipe becomes its item code ($CODE)" || bad "crystal as an upgrade material"
sed "s/^\*crystal=.*/*crystal=zz8/" "$N/crycodes1.txt" > "$N/crycodes_keep.txt"
gen cry8 crystal=1 shard=0 codes="$N/crycodes_keep.txt" | grep -q "crystal='zz8'" && echo "  the crystal keeps a saved code" || bad "the crystal's saved code was not kept"
# Another mod adds a stat: the marker must keep its number.
P="$N/modplus"
rm -rf "$P" && mkdir -p "$P/global/excel"
for entry in "$MOD"/*; do [ "$(basename "$entry")" = global ] || ln -s "$entry" "$P/"; done
for entry in "$MOD"/global/*; do [ "$(basename "$entry")" = excel ] || ln -s "$entry" "$P/global/"; done
for entry in "$MOD"/global/excel/*; do [ "$(basename "$entry")" = itemstatcost.txt ] || ln -s "$entry" "$P/global/excel/"; done
python3 - "$MOD/global/excel/itemstatcost.txt" "$P/global/excel/itemstatcost.txt" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
eol = b"\r\n" if b"\r\n" in data else b"\n"
lines = data.split(eol)
last = [l for l in lines if l][-1].split(b"\t")
last[0] = b"another_mods_stat"
open(sys.argv[2], "wb").write(data.rstrip(b"\r\n") + eol + b"\t".join(last) + eol)
PY
rm -rf "$N/cry9" && "$N/gen_test" "$P" "$VAN" "$N/cry9" crystal=1 shard=0 >/dev/null
[ "$(awk -F'\t' '$1 == "xlt_restored" {print NR - 2}' "$N/cry9/global/excel/itemstatcost.txt")" = "500" ] && [ "$(awk -F'\t' '$1 == "xlt_restored" {print NR - 2}' "$N/cry1/global/excel/itemstatcost.txt")" = "500" ] \
	&& grep -q "another_mods_stat" "$N/cry9/global/excel/itemstatcost.txt" && echo "  another mod's new stat does not move the marker: still stat number 500" || bad "the marker stat moved"
rm -rf "$P" # (links to the mod's own tables: not output, and not to be mistaken for it)

note "Shard of Desecration: a second item made like the crystal; both stash slots"
gen sh1 crystal=1 savecodes="$N/shcodes.txt" | tail -3 | grep -q "ok=1" || bad "generator run with the shard"
python3 test/check_shard.py "$MOD" "$N/cry1" "$N/sh1" "$N/shcodes.txt" | sed 's/^/  /'
python3 test/check_shard.py "$MOD" "$N/cry1" "$N/sh1" "$N/shcodes.txt" >/dev/null || bad "shard checks"
LAY=global/ui/layouts
SHARDCODE="$(sed -n 's/^\*desecration=//p' "$N/shcodes.txt")"; CRYSTALCODE="$(sed -n 's/^\*crystal=//p' "$N/shcodes.txt")"
for layout in "$LAY/bankexpansionlayouthd.json" "$LAY/controller/bankexpansionlayouthd.json"; do
	if [ -f "$MOD/$layout" ]; then
		python3 test/check_slot.py "$MOD/$layout" "$N/sh1/$layout" "$CRYSTALCODE,$SHARDCODE" place=400:300,300:300 beside | sed 's/^/  mod layout: /'
		python3 test/check_slot.py "$MOD/$layout" "$N/sh1/$layout" "$CRYSTALCODE,$SHARDCODE" place=400:300,300:300 beside >/dev/null 2>&1 || bad "the two stash slots in the mod's $layout"
	fi
done
gen sh5 crystal=1 >/dev/null; diff -r "$N/sh1" "$N/sh5" >/dev/null && echo "  two runs identical" || bad "two runs with the shard differ"

gen sh2 crystal=1 sharddrop=0 >/dev/null
python3 test/check_shard.py "$MOD" "$N/cry1" "$N/sh2" "" drop=0 >/dev/null && echo "  shard drop weight 0: no loot row carries it" || bad "shard drop weight 0"
gen sh3 crystal=1 downgrade=r01 >/dev/null
python3 test/check_shard.py "$MOD" "$N/cry1" "$N/sh3" "" downgrade=r01 >/dev/null && echo "  another downgrade material: the shard is still made, the recipe uses the material named" || bad "downgrade material"
gen sh4 shard=1 > "$N/sh4.log" 2>&1; grep -q "Shard of Desecration '[a-z0-9]*', listed in 43 boss loot rows" "$N/sh4.log" && [ "$(grep -c "stash slot for the Shard of Desecration at" "$N/sh4.log")" = 2 ] && ! grep -q "Restoration Crystal" "$N/sh4.log" && echo "  the shard without the crystal: made, dropped and stacked on its own" || bad "shard without the crystal"

note "tier choice against one merged list"
gen pick picktest=40000 | grep "pick test" | sed 's/^/  /' | grep -q " ok" && gen pick picktest=40000 | grep "pick test" | sed 's/^/  /' || bad "tier choice"
for mask in 4 1 0; do gen "tiers$mask" uniques=$mask sets=$mask | tail -1 | grep -o "uniques=[0-9]* sets=[0-9]*" | sed "s/^/  tiers mask $mask: /"; done

note "picker search in the game image"
if [ -f "$DUMP" ]; then
	"$N/scan_test" "$DUMP" 58c2f0 443910 314110 36ccf0 | sed 's/^/  /' | tail -8
	"$N/scan_test" "$DUMP" 58c2f0 443910 314110 36ccf0 >/dev/null || bad "picker search"
else
	echo "  skipped: no image dump at $DUMP"
fi

note "the DLL in a stand-in loader (wine)"
if command -v wine >/dev/null && command -v x86_64-w64-mingw32-g++ >/dev/null && [ -f build/d2rl-exalted-items.dll ] && [ -f "$DUMP" ]; then
	SDK=""
	for candidate in ../PluginSDK-master/include ../../PluginSDK-master/include; do [ -f "$candidate/D2RLPlugin/api.h" ] && SDK="$candidate" && break; done
	x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -Wno-missing-field-initializers -static -static-libgcc -static-libstdc++ -I "$SDK" -o build/fake_host.exe test/fake_host.cpp || bad "stand-in loader build"
	D="build/fakegame/mods/TestMod/TestMod.mpq/data"
	rm -rf build/fakegame
	mkdir -p "$D/global" "$D/local/lng" "$D/hd/items"
	cp -r "$MOD/global/excel" "$D/global/" && cp -r "$MOD/local/lng/strings" "$D/local/lng/" && cp "$MOD/hd/items/items.json" "$D/hd/items/"
	# The stash layouts (the mod's, or else the game's own): the crystal's stash slot goes into them.
	for from in "$MOD" "$VAN"; do
		if [ -f "$from/global/ui/layouts/bankexpansionlayouthd.json" ]; then
			mkdir -p "$D/global/ui/layouts/controller"
			cp "$from/global/ui/layouts/bankexpansionlayouthd.json" "$D/global/ui/layouts/" && cp "$from/global/ui/layouts/controller/bankexpansionlayouthd.json" "$D/global/ui/layouts/controller/"
			break
		fi
	done
	# The loot filter's rule panel (in the game it comes out of the game's own archive).
	for from in "$MOD" "$VAN"; do
		if [ -f "$from/global/ui/layouts/lootfilteritempanelhd.json" ]; then
			mkdir -p "$D/global/ui/layouts" && cp "$from/global/ui/layouts/lootfilteritempanelhd.json" "$D/global/ui/layouts/"
			break
		fi
	done
	# The Worldstone Shard's picture and model description, where the test data has them (in the
	# game they come out of the game's own archive): the crystal's purple picture is made from them.
	SHARD=hd/global/ui/items/misc/shard
	if [ -f "$VAN/$SHARD/mote_of_anguish.sprite" ] && [ -f "$VAN/hd/items/misc/shard/mote_of_anguish.json" ]; then
		mkdir -p "$D/$SHARD" "$D/hd/items/misc/shard"
		cp "$VAN/$SHARD"/mote_of_anguish*.sprite "$D/$SHARD/" && cp "$VAN/hd/items/misc/shard/mote_of_anguish.json" "$D/hd/items/misc/shard/"
	fi
	export WINEDEBUG=-all
	host() { timeout 150 wine build/fake_host.exe "$(winepath -w "$PWD/build/d2rl-exalted-items.dll")" "$(winepath -w "$PWD/build/fakegame")" TestMod "$(winepath -w "$PWD/$1")" "$2" 2>&1; }
	host exalted-items.toml "$(winepath -w "$DUMP")" > "$N/host1.log"
	grep -E "Exalted Items:|picker saw|FAIL|stand-in loader" "$N/host1.log" | cut -c1-260 | sed 's/^/  /'
	grep -q "stand-in loader: ok" "$N/host1.log" || bad "stand-in loader, default settings"
	grep -q "hooks installed: 3" "$N/host1.log" || bad "the three hooks installed"
	grep "^loot filter:" "$N/host1.log" | cut -c1-200 | sed 's/^/  /'
	[ "$(grep -c "^loot filter:" "$N/host1.log")" = 5 ] || bad "the loot filter's Exalted box in the DLL"
	grep -q "back to elite: the hook took 50 restores off" "$N/host1.log" && echo "  back to elite: the DLL's hook takes a restore off, the item is exactly as before" || bad "the DLL taking a restore off"
	grep -q "confirmed where they are known to be, no search" "$N/host1.log" && echo "  known game image: pickers confirmed without a search" || bad "the known game image was searched"
	grep -o "start-up took.*" "$N/host1.log" | sed 's/^/  first start (under wine): /'
	if [ -f "$D/$SHARD/mote_of_anguish.sprite" ]; then
		grep -q "Restoration Crystal (item code [a-z0-9]*, purple picture, stacks in the stash) in 43 boss loot rows, 169 restore recipes" "$N/host1.log" \
			&& grep -q '"itemCode":"[a-z0-9]*"}},' "build/fakegame/mods/TestMod/d2rloader/exalted-items/data/global/ui/layouts/bankexpansionlayouthd.json" \
			&& [ -f "build/fakegame/mods/TestMod/d2rloader/exalted-items/data/$SHARD/xlt_restoration_crystal.sprite" ] \
			&& grep -q "^\*crystal=" build/fakegame/mods/TestMod/d2rloader/exalted-items/codes.txt \
			&& echo "  Restoration Crystal: made, purple picture stored, code saved" || bad "the crystal in the DLL"
	else
		echo "  Restoration Crystal picture: skipped (no shard sprite in the test data)"
	fi
	host exalted-items.toml "search:$(winepath -w "$DUMP")" > "$N/host6.log"
	grep -q "MB searched in" "$N/host6.log" && grep -q "hooks installed: 2" "$N/host6.log" && grep -q "stand-in loader: ok" "$N/host6.log" \
		&& echo "  another game image: pickers found by search, both hooks" || bad "search on an unknown game image"
	grep -o "game code (.* ms)" "$N/host6.log" | sed 's/^/  /'
	grep -o "start-up took.*" "$N/host6.log" | sed 's/^/  start with a search (under wine): /' 
	host exalted-items.toml "$(winepath -w "$DUMP")" > "$N/host2.log"
	grep -q " 0 rebuilt this start" "$N/host2.log" && echo "  second start: nothing rebuilt" || bad "second start rebuilt files"
	if [ -f "$D/$SHARD/mote_of_anguish.sprite" ]; then
		grep -q "purple picture" "$N/host2.log" && grep -q "xlt_restoration_crystal.sprite: read" "$N/host2.log" && echo "  second start: the stored picture is used again" || bad "the crystal's picture on a second start"
	fi
	grep -o "start-up took.*" "$N/host2.log" | sed 's/^/  second start (under wine): /' 
	sed -e 's/^unique_drops = .*/unique_drops = "elite"/' -e 's/^set_drops = .*/set_drops = "off"/' exalted-items.toml > build/test-elite.toml
	host build/test-elite.toml "$(winepath -w "$DUMP")" > "$N/host3.log"
	grep -q "unique drops: elite" "$N/host3.log" && grep -q "set drops: off" "$N/host3.log" && grep -q "hooks installed: 2" "$N/host3.log" && grep -q "stand-in loader: ok" "$N/host3.log" \
		&& echo "  elite uniques only, sets off: one hook" || bad "tier settings in the DLL"
	host exalted-items.toml none > "$N/host4.log"
	grep -q "hooks installed: 0" "$N/host4.log" && grep -q "stand-in loader: ok" "$N/host4.log" && echo "  image without the pickers: no hooks, no failure" || bad "missing pickers"
	sed -e 's/^terror_zone_only = .*/terror_zone_only = true/' exalted-items.toml > build/test-zone.toml
	host build/test-zone.toml "$(winepath -w "$DUMP")" > "$N/host7.log"
	grep -q "they drop only in terror zones" "$N/host7.log" && grep -q "hooks installed: 3" "$N/host7.log" && grep -q "stand-in loader: ok" "$N/host7.log" \
		&& [ "$(grep -c "picker saw" "$N/host7.log")" = "$(grep -c "picker saw" "$N/host1.log")" ] \
		&& grep -q "cond('Desecrated')" build/fakegame/mods/TestMod/d2rloader/exalted-items/data/global/excel/armor.txt \
		&& echo "  only in terror zones: condition in the stored copies, both hooks working as before" || bad "terror zone setting in the DLL"
	host exalted-items.toml "$(winepath -w "$DUMP")" > "$N/host8.log"
	grep -q "they drop everywhere" "$N/host8.log" && ! grep -q "cond('Desecrated')" build/fakegame/mods/TestMod/d2rloader/exalted-items/data/global/excel/armor.txt \
		&& echo "  setting turned off again: condition gone from the stored copies" || bad "terror zone setting turned off"
	# Where the plugin keeps its files: beside the loader folder it is installed in.
	MS=build/fakegame/mods/TestMod/d2rloader/exalted-items
	GS=build/fakegame/d2rloader/exalted-items/TestMod
	cp "$MS/codes.txt" "$N/codes.before"
	FAKE_SCOPE=global host exalted-items.toml "$(winepath -w "$DUMP")" > "$N/host9.log"
	grep -q "stand-in loader: ok" "$N/host9.log" && grep -q "hooks installed: 3" "$N/host9.log" && grep -q "installed for every mod" "$N/host9.log" \
		&& cmp -s "$N/codes.before" "$GS/codes.txt" && [ -f "$GS/data/global/excel/armor.txt" ] && [ -z "$(find "$MS/data" -type f 2>/dev/null)" ] \
		&& echo "  installed for every mod: files in <game>/d2rloader/exalted-items/<mod>, codes taken over, old copies removed" || bad "global install"
	FAKE_SCOPE=global host exalted-items.toml "$(winepath -w "$DUMP")" > "$N/host10.log"
	grep -q " 0 rebuilt this start" "$N/host10.log" && grep -q "stand-in loader: ok" "$N/host10.log" && echo "  installed for every mod, second start: nothing rebuilt" || bad "global install, second start"
	host exalted-items.toml "$(winepath -w "$DUMP")" > "$N/host11.log"
	grep -q "stand-in loader: ok" "$N/host11.log" && grep -q "installed for this mod" "$N/host11.log" && cmp -s "$N/codes.before" "$MS/codes.txt" \
		&& [ -f "$MS/data/global/excel/armor.txt" ] && [ -z "$(find "$GS/data" -type f 2>/dev/null)" ] \
		&& echo "  moved back to the mod's folder: same codes, copies follow" || bad "move back to the mod folder"
	# A mod with a name of its own, and one whose .mpq folder is named differently again.
	O="build/fakegame/mods/Some Other Mod"
	mkdir -p "$O" && cp -r build/fakegame/mods/TestMod/TestMod.mpq "$O/Some Other Mod.mpq"
	timeout 150 wine build/fake_host.exe "$(winepath -w "$PWD/build/d2rl-exalted-items.dll")" "$(winepath -w "$PWD/build/fakegame")" "Some Other Mod" "$(winepath -w "$PWD/exalted-items.toml")" "$(winepath -w "$DUMP")" > "$N/host12.log" 2>&1
	grep -q "stand-in loader: ok" "$N/host12.log" && grep -q "hooks installed: 3" "$N/host12.log" && [ -f "$O/d2rloader/exalted-items/codes.txt" ] \
		&& echo "  a mod with another name (with a space in it): works" || bad "mod with another name"
	mv "$O/Some Other Mod.mpq" "$O/Whatever.mpq"
	FAKE_MPQ=Whatever.mpq FAKE_SCOPE=global timeout 150 wine build/fake_host.exe "$(winepath -w "$PWD/build/d2rl-exalted-items.dll")" "$(winepath -w "$PWD/build/fakegame")" "Some Other Mod" "$(winepath -w "$PWD/exalted-items.toml")" "$(winepath -w "$DUMP")" > "$N/host13.log" 2>&1
	grep -q "stand-in loader: ok" "$N/host13.log" && grep -q "hooks installed: 3" "$N/host13.log" && [ -f "build/fakegame/d2rloader/exalted-items/Some Other Mod/data/global/excel/armor.txt" ] \
		&& echo "  .mpq folder named differently from the mod, installed for every mod: works" || bad ".mpq folder with another name"
	sed -e 's/^enabled = .*/enabled = false/' exalted-items.toml > build/test-off.toml
	host build/test-off.toml "$(winepath -w "$DUMP")" > "$N/host5.log"
	grep -q "disabled in exalted-items.toml" "$N/host5.log" && [ -z "$(find build/fakegame/mods/TestMod/d2rloader/exalted-items/data -type f 2>/dev/null)" ] && echo "  disabled: copies removed" || bad "disabled state"
	[ -f build/fakegame/mods/TestMod/d2rloader/exalted-items/codes.txt ] && echo "  codes.txt kept while disabled" || bad "codes.txt lost while disabled"
else
	echo "  skipped: needs wine, mingw-w64, a built DLL and the image dump"
fi

echo
[ "$FAILED" -eq 0 ] && echo "ALL CHECKS PASSED" || echo "$FAILED CHECK(S) FAILED"
exit "$FAILED"
