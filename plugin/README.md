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

| Plugin DLL in | Its files (adjusted copies in `data/`, `codes.txt`, `stat-ids.txt`, previous copies in `backups/`) |
| --- | --- |
| `<game>/mods/<mod>/d2rloader/plugins/` (one mod) | `<game>/mods/<mod>/d2rloader/exalted-items/` |
| `<game>/d2rloader/plugins/` (every mod) | `<game>/d2rloader/exalted-items/<mod>/` |

The second has a folder per mod, because the copies and the codes belong to one mod's files.
When the plugin is moved from one place to the other, `codes.txt` and `stat-ids.txt` are taken
along on the next start and the copies left at the old place are removed. Reinstalling the mods
rebuilds only `mods/<mod>/<mod>.mpq/`, never the `d2rloader/` folder beside it, so both places
keep these files.

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

- **The Shard of Desecration.** A second item made by the same code as the crystal (same
  template row, red tint, code kept as `*desecration=`), word `desecration` in recipes,
  default material of the back-to-elite recipe. `AddMaterialDrops` runs once per item, so the
  shard sits beside the crystal in the same loot rows. Slots: `AddStashSlot` takes a wished
  place (slots right of / below the `xa1` slot, in the pitch of its neighbours); a wish that
  is taken or does not join an existing slot falls back to `FreeSlotPlace`. The frames of all
  new slots are painted into one copy of the background; the copy is reused only while the
  stored layout (`Io::ReadCopy`) is identical to the new one, i.e. the places are the same.
- **Taking a restore off (`restore_strip.h`).** The cube has no output that removes attributes
  and a recipe cannot read what an item rolled. So every restore property has a twin
  (`xlt-r<n>`, order fixed in `kRollProperties`): the same row plus one function 3 that puts
  the same roll into a hidden stat (`xlt_roll_<n>`, numbers from `stat-ids.txt`, shaped like
  `item_req_percent`: signed, saved as -100..155, no text, no price). The game hands the first
  function's amount to the later ones, and function 3 repeats it. Restore recipes use the
  twins. Each exalted -> elite recipe carries `mod 1 = xlt-restored 0..0`, which by itself
  does nothing. The plugin hooks the game's "apply one property" routine (0x3d4620 in image
  6ab3782c); a call whose row has function 1 on the marker stat and an amount of 0..0 is that sign.
  It then calls the routine with `-amount` for every twin whose hidden stat is set, and with
  -1 for the marker. An item without hidden stats (restored by 0.10.x) is left alone. Known
  image only: stamp plus the entry bytes of the routine and its four helpers are checked.
  The hook is placed before the tables are built (through the loader's mutation transaction
  where it has one, else its plain hook call), and the tables go by whether it is there
  (`Config::stripHooked`): without it each exalted -> elite recipe carries the restore recipe's
  own check instead of the sign (`op` 18, the marker stat = 0), so back to elite takes only
  items that were never restored and refuses a restored one. A mod with Exalted's rows in its
  own tables keeps its own back-to-elite rows: no such check there; the log and status say so.
  Tests: `test/strip_test.cpp` (model of the routine on generated tables), step 1b of
  `test/fake_host.cpp` (the real DLL's hook under Wine), and the stand-in loader refusing the
  hook (`FAKE_REFUSE_STRIP`).
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
  types belongs to). `op` 18 / `param` = the marker's number (`stat-ids.txt`; 375 on a packed
  install, 500 in 1.2.0's layout) / `value` 0 lets the recipe take only an item whose marker
  stat is 0.
- **Marker.** Stat `xlt_restored`, a copy of the game's `item_indesctructible` row (on the
  item or not, one line of text), and property `xlt-restored`.
- **Stat numbers (`stat-ids.txt`, beside codes.txt).** Saved items name stats by number, so the
  plugin's own numbers (marker and the 13 roll stats, nothing else) are kept in a map and never
  move. A new install gets them right after the stats the loaded tables have (the larger of both
  banks). Rows other plugins add come after them. An install that ran 1.2.0 or older (codes.txt,
  no map) keeps 1.2.0's layout (`layout=legacy`: marker 500, rolls 499 down to 487,
  `xlt_unused_<n>` rows in between), byte for byte, until the migration tool moves its saves; the
  saves decide that, not the rule alone (see below). Rows a loaded (exported) mod already has at
  the plugin's numbers are filled when they are padding (nothing in the row but its name and
  number, whatever the name) or the plugin's own `xlt_` rows; rows past them are kept, after the
  plugin's. Any other row there is a stat of the mod's own: restoring is switched off for that
  bank, nothing moves, and a loud console line says so (saved restored items may read wrong).
  Keys the plugin does not know in `stat-ids.txt` are ignored, so an older build put back after a
  newer one still starts.
- **Never bricking (`codes.txt`, `stat-ids.txt` missing).** After every start that worked, both
  files are copied to `backups/` beside them. A missing or unreadable file is taken, in this order,
  from the plugin's old place, from `backups/`, and then from the saves: the codes in the tables
  the last start stored (else codes worked out anew) and the stat layout of the rule above (else
  the other one), each set of tables taken only when every save in
  `<Saved Games>/Diablo II Resurrected/mods/<mod>/` reads with it to its end (`save_check.cpp`,
  with Exalted's own save reader `save_reader.cpp`; read only). When none does, the plugin stays
  off and the log, status and console name the characters that hold exalted items. A failed start
  or a missing file redirect names them too. An unreadable `stat-ids.txt` is kept aside as
  `stat-ids.txt.unreadable-<date>`, never deleted.
- **The mod's own Exalted cube rows that disagree with this install** (a restore row whose `op`
  18 check is not on this install's marker, a back-to-elite row checking another stat, a roll
  property this install does not have): kept as the mod has them, named in the log, and a loud
  console line.
- **Migration (`migrate/`, `exalted_migrate`).** Offline, game closed, started by hand: moves the
  saved marker and roll stats (Exalted's own only) from the 1.2.0 numbers to packed ones, in place (only the 9-bit
  numbers and the .d2s checksum change), with a dated backup folder in the saves folder first,
  a read-back of every item, and a full put-back on any failure; then stat-ids.txt last. A save
  holding another plugin's stat past the mod's stats stops it (that plugin moves its own). Dry run unless `--apply`. Needs the map 1.3.0 writes at its
  first start. Test: `test/check_migrate.sh` (copies in build/native/mig).
  Item saves have room for numbers up to 510 (nine bits; 511 ends a list).

## The loot filter's Exalted box

The game's loot filter knows three tiers (`equipmentQuality`: normal, exceptional, elite; 0 to
2, with 3 as "none", which a rule refuses). It gets a base's tier from the item row's three
family codes, so an exalted base comes out as normal. With `[loot_filter] exalted_box`:

- the core adds a box `ExaltedQualityChbx` to `global/ui/layouts/lootfilteritempanelhd.json`
  (a copy of the Elite box, 400 to its right, sending `UpdateQualityFilter:exalted`);
- the plugin points four calls of the game at itself, each through a jump in unused padding
  (0x3ed270, 0x3ed280, 0x3ed290): the tier of a dropped item (0x1c1235) and of an entry in the
  rule panel's list (0x151d706) give 4 for an exalted base; the list of tier names, built on
  first use (0x210156), gets `exalted` = 4, which serves the rule file's reader and writer and
  the boxes' click messages; the rule panel's tier -> box table (0x14acd16) gets the new box.

Keeping the tier in a rule, matching and ticking the box when a rule opens are the game's own
code, driven by those two tables. Known game image only; every place is checked first. The
Chronicle panel's tier calls are left alone.

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

`exalted legend` writes `legend.txt` beside `codes.txt`: every exalted base (English name from
the generated `item-names.json` entry, codes, required level, unique/set families) and the two
materials, built by `FormatLegend` from `Summary::legend`, which `AddLegend` fills from the first
data bank at start-up. `test/check_legend.py` checks it against codes.txt and item-names.json.

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
`<game>/d2rloader/plugins/`). The mod must be in folder form (`<mod>.mpq` as a folder; any mod
name works).

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
  game image and item table, presses both hooks and runs the console command (and
  `exalted legend`, which must write legend.txt);
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
