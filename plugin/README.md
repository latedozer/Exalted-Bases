# Exalted Items (D2RLoader plugin)

A fourth item tier above elite. Each time the game starts the plugin reads the active mod's
`armor.txt` / `weapons.txt` (or the game's own, when the mod has none) and builds an exalted
version of every elite base as ordinary soft-mod data.

**The mod's files are never changed.** Adjusted copies are kept in a folder of the plugin's
own, and the game is handed a copy whenever it opens the matching file of the mod
(`mods/<mod>/<mod>.mpq/data/...`). You keep launching the same mod.

The mod can have any name: the loader says which mod is active, and its files are read from
`mods/<mod>/<mod>.mpq/data` (if the mod's folder holds a single `.mpq` folder with another
name, that one is used). The `.mpq` must be a folder, not a packed archive.

Where the plugin keeps its files depends on where it is installed:

| Plugin DLL in | Its files (adjusted copies in `data/`, and `codes.txt`) |
| --- | --- |
| `<game>/mods/<mod>/d2rloader/plugins/` (one mod) | `<game>/mods/<mod>/d2rloader/exalted-items/` |
| `<game>/d2rloader/plugins/` (every mod) | `<game>/d2rloader/exalted-items/<mod>/` |

The second has a folder per mod, because the copies and the codes belong to one mod's files.
When the plugin is moved from one place to the other, `codes.txt` is taken along on the next
start and the copies left at the old place are removed.

## What it generates

| File (both `global/excel` and `global/excel/base`) | What is added |
| --- | --- |
| `armor.txt`, `weapons.txt` | one exalted row per elite base; the elite row's `ubercode` points at it |
| `cubemain.txt` | upgrade recipes: elite unique / set / rare + runes + perfect gem -> same item, exalted base |
| `treasureclassex.txt` | only when the tier reaches above item level 87: the loot rows continued upward (see Drops) |
| `local/lng/strings/item-names.json` | "Exalted <name>" in every language |
| `hd/items/items.json` | the elite base's model for each exalted base |

| `misc.txt` | the Restoration Crystal (with `[crystal] enabled`) |
| `itemstatcost.txt`, `properties.txt` | the stat and property a restored item is marked with (with restoring on) |
| `hd/global/ui/items/misc/shard/xlt_restoration_crystal.sprite` (+ `.lowend`), `hd/items/misc/shard/xlt_restoration_crystal.json` | the crystal's picture and model description |

`uniqueitems.txt`, `setitems.txt` and `sets.txt` are never touched.

Generated rows are marked (`[xlt] ` in `name` / `description`, `xlt ` loot rows, `xlt_` string
keys).

## Drops

An exalted base is an ordinary spawnable item. The game itself sorts every base into a loot
table by its item level (`weap3` ... `weap96`, `armo3` ... `armo96`, one table per three
levels), so an exalted base drops wherever items of its level drop: from monsters, bosses and
chests, with the quality bonuses those drops carry. There is no separate roll and no drop
chance setting.

The tier is spread over ten of those tables, three levels apart, counted down from
`drop_level_max` (default 96: item levels 69, 72 ... 96). Each exalted base keeps the place its
elite base has among the elite bases.

The game's own loot rows stop at table 87 (`Act 5 (H) ... C`, monster level 85). When the tier
reaches above that, the top rungs of every ladder of loot rows that leads there (Equip, Melee,
Bow, Chest, H2H, Cast, Miss, Citem, Champ, Uitem, Unique and their terror zone versions) are
repeated that many rungs higher, at monster levels 88, 91 and 94, with their level tables
raised by the same amount. Rows the game itself places at those levels are pointed at the new
rungs: the superunique and terror zone boss rows at 90/93/96, and the fixed rows of bosses whose
own level is that high (Hell Diablo, Baal, Nihlathak). Nothing below the old top changes.

With `drop_level_max` at 87 or lower no loot row is added or changed at all.

### Only in terror zones

With `terror_zone_only = true` every exalted row gets `cond('Desecrated')` in its
`DropConditionCalc` cell. That is the game's own test for a drop made in a terror zone, the one
its loot rows use (`ConditionCalc` of `All Acts Terrorize Consumable`), and both columns are
compiled and evaluated by the same routines. The base stays an ordinary spawnable member of its
level table. When a loot table is rolled, the game first takes out every entry whose condition
fails and lowers the total by its weight, so outside a terror zone a level table gives exactly
what it gives without the exalted bases: no lost drops, no changed odds. The condition is read
nowhere else (not by the unique or set pickers, not by the cube), so unique and set items on
exalted bases and both kinds of cube recipe are unaffected.

A level table above the game's own (90, 93, 96) holds nothing but exalted bases. A new rung
names such a table through a row of its own that carries the same condition
(`xlt weap96 terror zone`), so outside a terror zone the rung is left with the game's tables and
its way down to the rung below instead of landing on an empty table. Those rows are placed in
front of the first ladder that got new rungs, because the game only knows a row it has already
read.

Not known: whether a chest opened inside a terror zone counts. The condition is evaluated for
the unit the drop is made for; monsters are certain (the game's own terror zone rows rely on
it), chests have not been tried.

## The Restoration Crystal

One item, built like an exalted base: a copy of a row the mod has (the Western Worldstone
Shard `xa1`; in tables without it, an essence), with its own code, kept in `codes.txt` as
`*crystal=<code>`. The cells that make the shard usable are cleared.

- **Stash slot.** The stash's stacking tabs hold an item when its row says
  `AdvancedStashStackable` = 1 and the stash layout has an `AdvancedStashSlotWidget` naming
  its code. The plugin adds one such widget to adjusted copies of
  `global/ui/layouts/bankexpansionlayouthd.json` and `controller/bankexpansionlayouthd.json`
  (the mod's, or else the game's own): in the container that holds the `xa1` slot, the same
  size, at a free place among the columns and rows the existing slots use: the one with the
  most slots right beside it, then the nearest (`AddStashSlot`, `FreeSlotPlace`). The game's
  slot frames are painted into the stash background, so the frame is painted too: the model
  slot's frame is copied to the new place in an adjusted copy of that tab's `backgroundFile`
  sprite and its `.lowend` twin (`CopyInSprite`). Stored once (`Io::HasCopy`); delete the
  stored copy to have it made again, for instance after the mod's stash art changed. Do NOT
  put any other widget into a slot container: the game takes every child for a slot and
  crashes when a character is loaded (0.10.4 did that with an `ImageWidget`). Only when a
  layout got the slot is the row flagged. The stored piles are
  ordinary item records in the shared stash file's materials section, so a new code needs
  nothing else. Not yet seen in game. `stash_slot = false` turns it off.

- **Picture.** The shard's sprite is read from the mod or the game's archive, tinted purple
  (hue replaced, brightness and alpha kept; `TintSprite`) and stored under its own name. It is
  made once: later starts find the stored copy and do not open the game's archive. No game art
  is shipped with the plugin. The model description is a plain copy, so on the ground the
  crystal is the shard. If the sprite cannot be read, the crystal shares the shard's asset.
- **Drops.** Every loot row that lists a boss essence (`tes`, `ceh`, `bet`, `fed`) gets the
  crystal as one more entry, with the essence's weight times `drop_weight_percent`. No row is
  named in the code. The other entries of such a row drop that much less often (`Baal (H)`:
  150 added to a total of 9,023).
  Blood Raven, the Countess and Nihlathak get it too, in Hell: their rows are found from
  `monstats.txt` / `superuniques.txt` (the Hell loot row cells, the higher rungs of the same
  ladder, and from there down to the rows that list gold). They hold no essence, so the weight
  there is the row's total times the crystal's share in the first essence row.
- **Upgrade.** The word `crystal` in a recipe's materials becomes the crystal's code.
- **Restoring.** One recipe per exalted base (the cube cannot say "exalted" by type):
  base + crystal, output `useitem`, `mod 1` the marker, `mod 2..4` the attributes of the
  base's kind (`kRestoreKinds` in `exalted_core.h`; a base takes the first kind one of its item
  types belongs to). `op` 18 / `param` 500 / `value` 0 lets the recipe take only an item whose
  marker stat is 0, which is the pattern the Corruption Shards and Runed Unique Items mods use.
- **Marker.** Stat `xlt_restored`, a copy of the game's `item_indesctructible` row (on the
  item or not, one line of text), and property `xlt-restored`. Saved items name stats by
  number, so the marker is always stat 500; the numbers between the mod's last stat and 500
  are filled with empty `xlt_unused_<n>` rows. A table that already has more than 500 stats
  gets no marker and no restoring. (500 is below 511, the most a nine-bit stat number in an
  item save can hold.)

## Unique and set items on exalted bases

The game picks a unique or a set piece for a new item by the item's own code, so an exalted
base would find none. Rows are **not** repeated for it: a repeated unique is a separate unique
to the game (its own saved row number, its own grail entry), and a repeated set row is an extra
member of the set.

Instead the plugin hooks the game's two picking routines. For an exalted base it lets the
routine see a base of the same family for the length of that one call, then puts the exalted
base back. Level check, rarity, the once-per-game rule for uniques and the item's properties
stay the game's own work, and the item is saved as the real row.

`unique_drops` and `set_drops` choose the tiers of the family that may be used: `off`,
`normal`, `exceptional`, `elite`, `all`, or a list such as `"exceptional, elite"`. With more
than one tier, each is weighed by what it can give an item of that level, which gives every
row the same chance as one merged list of the whole family. Only rows the game's picker can
take are counted: a row whose `spawnable` cell is blank or 0, or whose `disabled` cell is set,
never drops (mods use that for uniques that are only made in the cube), so it weighs nothing.
With `off`, or when the family has
nothing for the item's level, the game falls back as it does for any base without such rows
(a unique roll becomes a rare, a set roll a magic item).

The two routines are found by the shape of their code (`game_code.h`), not by fixed
addresses, and the helper addresses and structure offsets the hook needs are read out of the
matched code. If a routine is not recognised in some game build, that kind of drop is off
there and the status line says so; nothing else is affected.

## Exalted base codes

A saved item names its base by item code. The codes handed out are kept in
`codes.txt` in the plugin's folder (see the table at the top) and reused on every start, so adding or removing
other mods cannot move an exalted base to another code. Keep that file.

## Settings

`d2rloader/config/exalted-items.toml`, created on first start; every setting is explained
there. With the "I Hate TOMLs" plugin installed the same settings appear in its in-game menu.
Changes apply on the next game start.

## File redirect

Every Win32 file call ends in one of four ntdll functions (`NtCreateFile`, `NtOpenFile`,
`NtQueryAttributesFile`, `NtQueryFullAttributesFile`). The plugin points the import slots for
those at its own functions, which answer a read-only open of an adjusted mod file with the copy
and pass everything else through. The console command `exalted` lists how often each copy was
read, and how the unique and set hooks have been used. A directory listing still reports the
mod's own file sizes.

## How the tier works

- **Upgrade:** the cube's `exc` output turns an item into its row's `ubercode`. Pointing an
  elite row's `ubercode` at the exalted base makes `"useitem,mod,exc"` on an elite item produce
  the exalted base while keeping the item (so uniques, sets and rares stay what they are).
- **Back to elite:** the cube's `eli` output turns an item into its row's `ultracode`, which
  for an exalted row is still the elite base. One recipe per exalted base (its code + the
  `downgrade` material, default the El rune) with `"useitem,mod,eli"` gives the same item on
  its elite base, whatever its quality. A recipe naming a kind of item instead would also lift
  normal and exceptional items to elite, hence one row per base.
- **Tier:** the exalted row keeps the family's original norm/uber/ultra codes, so it counts as
  none of the three stock tiers and no stock upgrade recipe accepts it.
- **Stats:** the exceptional -> elite step of the same family is applied once more
  (`stat_step_percent` scales it). Damage/defence pairs grow together; requirements grow by at
  most 50%, damage and defence by at most 100%. Level requirement keeps the elite base's ratio
  to its item level.

## Install

Copy `build/d2rl-exalted-items.dll` to `<game>/mods/<mod>/d2rloader/plugins/` (or the global
`<game>/d2rloader/plugins/`). The mod must be in folder form (`<mod>.mpq` as a folder, which is
what D2RMM produces; any mod name works).

Characters that hold exalted items need the plugin to open. To remove it, first get rid of
those items, then delete the DLL (and the `d2rloader/exalted-items` folder if you like).

## Build and test

`./build.sh` (mingw-w64; needs the D2RLoader Plugin SDK headers).

`test/run_tests.sh` runs everything that does not need the game:

- `test/gen_test.cpp` runs the generator natively against a mod's `data` folder (read only);
  `test/check_tables.py` checks its output against the mod's files;
- the tier choice is simulated against a merged list;
- `test/scan_test.cpp` runs the picker search against a dumped game image;
- `test/fake_host.cpp` is a stand-in loader that loads the real DLL under Wine with a made-up
  game image and item table, presses both hooks and runs the console command;
- `test/same_output.sh <reference source dir>` builds an earlier version of the generator and
  checks that the current one writes the same files byte for byte (by default against
  `../exalted-plugin-0.9.0-backup`; set `REF=` to use another);
- `test/check_zone.py <output without> <output with>` checks the terror zone setting: the only
  cells that differ are the conditions on the exalted rows and the rows that name a level table
  above the game's own;
- `test/check_crystal.py <mod> <vanilla> <output without> <output with>` checks the crystal:
  its row, the marker's number, one restore recipe per exalted base with exactly the
  attributes of its kind, the crystal beside every essence and nowhere else, the tinted
  picture, and that nothing else changed;
- `test/check_families.py` works out the family lists for the picker hooks a second time from
  the tables, by the game's rule (a row counts only when its `spawnable` cell says yes and its
  `disabled` cell does not), and compares them with the generator's;
- `test/core_test.cpp` checks the quick ways of reading tables and JSON against the plain way;
- `test/match_test.cpp` checks the file redirect's path compare (`redirect_match.h`);
- `test/bench.cpp` times the generator, the picker search and the path compare:
  `g++ -std=c++20 -O2 -I . -o build/native/bench test/bench.cpp exalted_core.cpp`, then
  `build/native/bench <mod data dir> <vanilla data dir> <game image dump | none>`.
