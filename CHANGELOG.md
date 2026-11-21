# Exalted Items: change log

Newest first. Versions before 0.8.5 were test builds and are not listed; no notes were kept
for them.

## 1.0.0 (2026-11-21)

- The game's loot filter gets a fourth Quality box in a rule's screen, beside Normal,
  Exceptional and Elite: **Exalted**. Tick it the way you tick the others. In a profile's code
  the tier is written `"exalted"`.
- An exalted base now counts as that tier in the filter. Before, the filter saw it as a Normal
  base, because the game works a base's tier out from its three family codes and an exalted
  base is none of them. So a rule that hides Normal items no longer hides exalted ones, and a
  rule that ticks only the game's three tiers leaves exalted items alone: tick Exalted in it
  if it should cover them.
- A profile that uses the box needs the plugin. Without it the game does not know the word
  `exalted` in a rule.
- Settings: `[loot_filter] exalted_box` (default true) and `label` (the text beside the box).
  Off: no box, and the filter sees exalted bases as Normal, as before.
- Only in the game build the plugin was written against. In another build the box is left
  out of effect and the log says so.
- Not changed: the Chronicle's item list still counts an exalted base as a Normal one.

## 0.13.0 (2026-11-17)

- New console command `exalted legend`: writes `legend.txt` beside `codes.txt` with the item
  code of every exalted base and of the two materials. Bases are grouped by kind and listed by
  required level, each with its English name, its elite base's code, its required level and
  whether it can drop as a unique or set item. The file is replaced each time the command runs
  and always matches this computer's codes. Nothing else changed.
- Typing `exalted` alone shows a line pointing to the new command.

## 0.12.0 (2026-11-15)

- New item: the Shard of Desecration, a Worldstone Shard tinted red. It is the material that
  turns an exalted item back to elite, in place of the El rune (`downgrade = "desecration"`;
  `"r01"` still works if you prefer the rune).
- It drops through the same loot rows as the Restoration Crystal (the Hell act bosses, Blood
  Raven, the Countess and Nihlathak; 43 rows), with the same weight. Settings under
  `[desecration]`, the same ones the crystal has.
- Stash slots: the crystal's slot moves one row down and three slots right, and the shard's
  slot stands beside it (with this mod set: the row under Baal's Eye and Mephisto's Brain).
  New settings `stash_slot_right` / `stash_slot_down` give each slot's place, counted in
  slots from the Western Worldstone Shard's slot; a place that is taken, or that does not join
  the existing slots, falls back to a free place found near it.
- The painted slot frames follow the slots: the stored stash background is made again
  whenever the slots' places change.
- Each of those boss loot rows now holds two more entries than the mod's own (crystal and
  shard), so the other things in those rows drop a little less often than with the crystal
  alone.
- Includes 0.11.0 (back to elite takes a restore off) and 0.10.6, neither installed before.

## 0.11.0 (2026-11-15)

- Turning a restored item back to elite takes the restore off: the item loses exactly what the
  restore added and its "Restored" line, and can be restored again later. Before, it kept the
  attributes, so a crystal and an El rune put restore attributes on an elite item.
- How: the cube cannot remove attributes, so a restore now writes each amount it rolled into a
  hidden value on the item (13 new hidden stats, numbers 487 to 499), and the plugin has a
  third hook, on the game's "apply a property to an item" routine. The back-to-elite recipe
  carries a sign; when the game applies it, the plugin applies the opposite of each written
  amount through the game's own routine.
- Items restored by 0.10.x have no written amounts: they keep their attributes and their mark.
- The hook is only placed in the game build it was found in. Elsewhere the status line says so
  and restored items keep their attributes; restoring still works.
- New setting `strip_on_downgrade` under `[restore]` (default `true`), also in the menu.
- Includes 0.10.6 (requirements -5 to 10% on restore), which was never installed on its own.

## 0.10.6 (2026-11-15)

- Restoring also lowers the item's requirements by 5 to 10% (strength and dexterity; the
  game's "Requirements -X%" attribute), for every kind of item. Each kind has a new
  `requirements` line under `[restore.<kind>]` to turn it off. Items restored before this
  version keep what they got and cannot be restored again.

## 0.10.5 (2026-11-15)

- Fixed: 0.10.4 crashed the game when a character was loaded. The slot's frame was a picture
  widget placed beside the stash slots, and the game takes everything in that group for a
  slot. The frame is now painted into an adjusted copy of the stash background, where the
  game's own frames are. The layout gets the slot and nothing else, as in 0.10.3.

## 0.10.4 (2026-11-15)

- The crystal's stash slot now continues a row of slots instead of standing alone: with this
  mod set's stash it is one place further right, directly left of Diablo's Horn.
- The slot has a frame. The game's frames are painted into the stash background, so the
  plugin cuts the Western Worldstone Shard's frame out of that background when the game
  starts and puts it behind the new slot (mouse and controller, both picture sizes).

## 0.10.3 (2026-11-15)

- Restoration Crystals pile up in the stash. The crystal gets a slot of its own in the stash's
  stacking tabs, the way the game keeps runes, keys and Worldstone Shards: one more slot in
  the stash layout (mouse and controller), in the free place nearest to the Western Worldstone
  Shard, and the crystal's row is marked as a stacking material. The mod's layout files are
  read, not changed.
- New setting `stash_slot` under `[crystal]` (default `true`), also in the settings menu.
- The slot has no frame painted for it in the stash background unless the background already
  has an empty one there.

## 0.10.2 (2026-11-15)

- The Restoration Crystal's name is written in orange, the colour of essences, keys, runes
  and Worldstone Shards. The game colours its own materials by their item code, which a new
  item cannot share, so the colour code is put in front of the name.
- New setting `name_color` under `[crystal]` (default `"orange"`).

## 0.10.1 (2026-11-15)

- The Restoration Crystal also drops from Blood Raven, the Countess and Nihlathak in Hell,
  including their terror zone rows (14 loot rows). These bosses have no essence, so the crystal
  gets the weight that gives it the same share of a drop as it has in Andariel's row (about
  1.5%). `drop_weight_percent` scales it like the rest. Normal and Nightmare are unchanged.

## 0.10.0 (2026-11-15)

- New item: the Restoration Crystal, a Worldstone Shard that cannot be used on its own. Its
  picture is that shard's, tinted purple by the plugin when the game starts. On the ground it
  looks like the shard.
- Only bosses drop it. It is listed beside the boss essence in every loot row that has one:
  the five act bosses in Hell, including their first-kill and terror zone rows. Setting
  `drop_weight_percent` (100 = as often as the essence, 0 = never).
- The upgrade to exalted now takes one crystal instead of runes and a gem. In a recipe the
  word `crystal` stands for the item; rune and gem codes still work.
- New cube recipe, restoring: an exalted item plus a crystal gets every attribute listed for
  its kind (body armor, helms, shields, belts, gloves, boots, staves/wands/orbs, other
  weapons), each rolled in its range. Any quality, once per item. A restored item is marked,
  shows "Restored", and the cube does not take it again.
- Every attribute has its own on/off line under `[restore.<kind>]`.
- New adjusted files: `misc.txt`, `itemstatcost.txt`, `properties.txt`, the crystal's two
  pictures and its model description. The marker is stat number 500, with unused rows in
  between, so that other mods adding stats cannot move it.
- A settings file from an earlier version keeps its rune recipes until its `[recipes]` lines
  are changed to `"crystal"`.
- Not known yet, none of it has been run in the game: whether the game loads the new picture
  file, and whether the cube recipes behave as the tables say.

## 0.9.5 (2026-11-14)

- Works with a mod of any name. The mod's files are read from `mods/<mod>/<mod>.mpq/data`,
  whatever `<mod>` is. If the mod's folder has no `<mod>.mpq` but holds a single `.mpq` folder
  with another name, that one is used.
- The plugin's files now go beside the loader folder the plugin is installed in:
  - installed for one mod (`mods/<mod>/d2rloader/plugins/`): `mods/<mod>/d2rloader/exalted-items/`, as before
  - installed for every mod (`d2rloader/plugins/` in the game folder): `d2rloader/exalted-items/<mod>/`
- When the plugin is moved between those two places, `codes.txt` is carried over on the next
  start and the old adjusted copies are removed.
- Messages no longer assume D2RMM.
- Still not supported: a mod that is a packed `.mpq` archive file instead of a folder.

## 0.9.4 (2026-11-14)

- New setting `terror_zone_only` (off by default). When on, exalted bases drop only in terror
  zones. Each exalted base carries the game's own terror zone drop condition; outside a terror
  zone the game leaves it out of the roll, so other drops are unchanged.
- Unique and set items on exalted bases and the cube recipes work the same with the setting on
  or off.
- Not known yet: whether chests opened inside a terror zone count.
- This version was never installed on its own; it shipped as part of 0.9.5.

## 0.9.3 (2026-11-14)

- The cube upgrade to exalted no longer adds a level requirement, and the `upgrade_level_req`
  setting is gone. The game cannot store a negative level requirement, so one added by the
  upgrade could not be taken off again when the item is turned back to elite.

## 0.9.2 (2026-11-14)

- New cube recipe: any exalted item plus a low rune (El by default, setting `downgrade` under
  `[recipes]`) becomes the same item on its elite base. Works for plain, magic, rare, set and
  unique items.

## 0.9.1 (2026-11-14)

- Faster start: building the tables takes less than half the time and memory, and the game's
  two picking routines are found without a search on the known game build.
- Fix: unique and set rows the game can never pick (blank `spawnable`, or `disabled`) were
  counted when choosing which family base an exalted item borrows from, so some unique and set
  rolls were lost. Only rows the game can pick are counted now.
- The log has a line saying how long the start took.
- The adjusted files are byte for byte the same as 0.9.0's.

## 0.9.0 (2026-11-14)

- Exalted bases are ordinary members of the game's loot tables. They drop by item level like
  any other base; the separate drop roll and its drop chance setting are gone.
- The tier is spread over ten level tables, counted down from `drop_level_max` (default 96).
  Above the game's own top table the loot rows are continued upward.
- Unique and set items on exalted bases come from the game's own picking routines. Nothing is
  added to `uniqueitems.txt`, `setitems.txt` or `sets.txt` any more: the item is the real unique
  or set piece, on an exalted base. Settings `unique_drops` and `set_drops` choose which tiers
  of the family can appear.
- Exalted base codes are saved in `codes.txt` and reused on every start, so adding or removing
  other mods cannot turn a saved exalted item into another item.
- The first-use warning was removed.

## 0.8.5 (2026-11-12)

- Proof of concept. Exalted versions of every elite base with their names and models, cube
  upgrade recipes, and a settings menu through the "I Hate TOMLs" plugin.
- The mod's files are never changed; the game is handed adjusted copies.
- Unique items on exalted bases were done by adding a copy of each unique to
  `uniqueitems.txt`. Set items were done by a hook on the game's set picking routine.
- Drops came from a separate pool with its own roll for monsters above a set level.
- Known problem, fixed in 0.9.0 by removing the copies: the copied unique rows sat at the end
  of the unique table. Anything that remembers a unique by its row number (the game's record
  of found items, a grail) could be left pointing at a row that no longer exists once the
  copies were gone.
