# Exalted Items: user guide (v1.3.0-pre.1)

Exalted Items adds a fourth tier of weapons and armor above elite, called Exalted.

## What it adds

- An exalted version of every elite base: Shako gets Exalted Shako, and so on.
- Higher stats. Bramble Mitts have 54 to 62 defense; Exalted Bramble Mitts have 99 to 114.
  Requirements go up too.
- The same look as the elite base. Only the name and the numbers change.

## How to get exalted items

**Drops.** They drop like any other item, mostly in Hell.
The best ones need high-level monsters: terror zones, Hell Diablo, Baal, Nihlathak.

With `terror_zone_only = true` they drop only in terror zones. Everything else stays the same:
uniques, sets and the cube recipes work as before.

**The cube.** An elite item plus a Restoration Crystal becomes the same item on its exalted
base. This works for unique, set and rare items. The item keeps everything it had.

**Back to elite.** Any exalted item plus a Shard of Desecration becomes the same item on its elite base.
This works for plain, magic, rare, set and unique items.

## The Restoration Crystal

A purple Worldstone Shard. It cannot be used on its own; it goes in the cube.

**Where it drops.** Only from bosses in Hell: the five act bosses, Blood Raven, the Countess
and Nihlathak. An act boss can drop it about as often as its essence. The other three have
the same chance per drop as Andariel.

**In the stash.** Crystals pile up in the stash's stacking tabs, like runes and Worldstone
Shards. The crystal has its own slot there, placed a set number of slots from the Western
Worldstone Shard's slot (settings `stash_slot_right` and `stash_slot_down`); with the stash
layout of this mod set that is the bottom right corner, under Mephisto's Brain. The slot's
frame is painted into a copy of the stash background. Take one out to use it in the cube. The
size of a pile is the loader's `set_materials_limit`.

**Restoring.** An exalted item plus a crystal gets every attribute listed for its kind, each
rolled in its range. It works on any quality, once per item. A restored item says "Restored".

| Kind | A restore adds |
| --- | --- |
| Body armor | All resistances +4 to 7%, enhanced defense +30 to 40%, maximum life +3 to 4% |
| Helms | Enhanced defense +30 to 40%, maximum life +3 to 4%, replenish life +5 to 10 |
| Shields | All resistances +4 to 7%, enhanced defense +30 to 40%, chance to block +5 to 10% |
| Belts | Faster run/walk +5 to 10%, maximum mana +5%, attack speed +5 to 10% |
| Gloves | Attack speed +5 to 10%, attack rating +5 to 10%, faster hit recovery +5 to 10% |
| Boots | Faster run/walk +5 to 10%, faster hit recovery +5 to 10%, maximum mana +5% |
| Staves, wands, orbs | Faster cast rate +5 to 10%, maximum mana +5%, all resistances +4 to 7% |
| Other weapons | Enhanced damage +15 to 30%, attack speed +5 to 10%, attack rating +5 to 10% |

Every kind also gets **requirements -5 to 10%**: the strength and dexterity needed to equip
the item go down by that much. The level requirement does not change.

An attribute the item already has is added on top. The item cannot be restored a second time
while it is restored.

**Back to elite takes the restore off.** Turning a restored item back to elite (the Shard of Desecration
recipe) removes exactly what the restore added, and the "Restored" line with it. The item is
then as it was before the restore, on its elite base, and can be upgraded and restored again.
The crystal is not given back. This works only for items restored with v0.11.0 or later: a
restore now writes the amounts it rolled onto the item as hidden values, and older restored
items have none, so they keep their attributes. It needs the game build the plugin knows. Where
the plugin cannot take a restore off (another game build, or the loader refused its hook), back
to elite takes only items that were never restored: the cube refuses a restored item, and the
`exalted` console line says why. A mod with Exalted Items already in its own tables keeps its
own back-to-elite recipes, which have no such check: there a restored item goes back with its
restore kept, and the log says so.

## The Shard of Desecration

A red Worldstone Shard, the counterpart of the crystal: in the cube it turns an exalted item
back into the same item on its elite base. It replaces the El rune in that recipe.

It drops where the crystal does: from the same bosses in Hell (the act bosses, Blood Raven, the
Countess and Nihlathak), in the same loot rows, at half the crystal's chance by default
(`[desecration] drop_weight_percent = 50`). It stacks in the stash too. Its slot and the crystal's stand side by
side, in the row under Baal's Eye and Mephisto's Brain with this mod set's stash.

## Loot filter

A rule in the game's loot filter has a fourth Quality box: **Exalted**, beside Normal,
Exceptional and Elite. Exalted items count as that tier only, so a rule that should cover them
needs the box ticked. A profile that uses the box needs the plugin installed.

## Uniques and sets

An exalted base that rolls unique becomes a real unique from its family. An Exalted Shako can
become War Bonnet, Peasant Crown or Harlequin Crest. Set pieces work the same way.

If its family has nothing for that item, it becomes a rare (or a magic item for a set roll),
as the game does for any base without uniques.

## Settings

Edit `exalted-items.toml` in the `config` folder beside the `plugins` folder the plugin is in, or use the "I Hate TOMLs" menu in the
game. Changes apply the next time the game starts.

| Setting | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` | Turns the plugin on or off |
| `drop_level_max` | `96` | Item level of the highest exalted bases |
| `terror_zone_only` | `false` | `true`: exalted bases drop only in terror zones |
| `stat_step_percent` | `100` | How much stronger than elite. `0` = same as elite |
| `level_req_cap` | `90` | Highest level requirement |
| `unique_drops`, `set_drops` | `"all"` | Which tiers' uniques and sets can appear: `off`, `normal`, `exceptional`, `elite`, `all` |
| `name_prefix` | `"Exalted "` | Text in front of the name |
| `[recipes]` | `"crystal"` | Cube materials for the upgrade. `crystal` is the Restoration Crystal; rune and gem codes work too. `""` turns a recipe off |
| `downgrade` (in `[recipes]`) | `"desecration"` | Material that turns an exalted item back to elite. `desecration` is the Shard of Desecration; `"r01"` would be the El rune |
| `[crystal]` `enabled` | `true` | `false`: no crystal. Give the recipes other materials then |
| `[crystal]` `name` | `"Restoration Crystal"` | Its name |
| `[crystal]` `name_color` | `"orange"` | Colour of the name. Orange is what the game uses for essences, keys and Worldstone Shards |
| `[crystal]` `drop_weight_percent` | `100` | How often bosses drop it, compared with their essence. `0` = never |
| `[crystal]` `stash_slot_right`, `stash_slot_down` | `4`, `3` | Where the crystal's stash slot goes, counted in slots from the Western Worldstone Shard's slot. `0` and `0`: a free place near it |
| `[desecration]` | as `[crystal]` | The same settings for the Shard of Desecration: `enabled`, `name`, `name_color`, `drop_weight_percent` (`50`), `stash_slot`, `stash_slot_right` (`3`), `stash_slot_down` (`3`) |
| `[restore]` `strip_on_downgrade` | `true` | Back to elite takes a restore off. `false`: nothing is written down and a restored item keeps its attributes |
| `[crystal]` `stash_slot` | `true` | The crystal has a slot in the stash's stacking tabs. `false`: an ordinary item. Take your crystals out of that slot before you turn this off |
| `[restore]` `enabled` | `true` | Turns restoring on or off |
| `[restore]` `show_line` | `true` | Whether a restored item says "Restored" |
| `[restore.<kind>]` | all `true` | One line per attribute. `false` leaves it out of later restores |

Some settings change items you already have:
- `stat_step_percent` and `level_req_cap` change the numbers and requirements of every exalted
  item, the ones in your saves too. A character may no longer be able to wear its gear.
- `name_prefix` renames every exalted item.
- Turning `[crystal] enabled` (or `[desecration] enabled`) off leaves the stored materials
  without an item: take them out of the stash first, or keep the setting on. The same for
  `stash_slot`: take the materials out of their slot before you turn it off.

## Other mods

The tier is rebuilt from your mods every time the game starts.

- Changes to elite bases carry over. If a mod gives gloves sockets, exalted gloves have them.
- A new base gets an exalted version if a mod adds it as the top tier of a family.
- Your mod's files are never changed.
- A mod can bring its own versions of the exalted cube recipes (rows whose description starts
  with `[xlt] `, e.g. a restore that gives only enhanced defense). Those are kept as the mod
  wrote them, and every other base gets the plugin's recipe. A mod row with `enabled` 0 takes a
  recipe away.
- A mod that already has Exalted Items in its own tables (exported from a game that ran the
  plugin, or with the plugin's files copied in by hand) is used exactly as it is. Keep the plugin on: it still handles unique and set drops, taking a restore off and
  the loot filter box. Settings that shape the tables do nothing for such a mod.

## What to expect

- No effect on frame rate.
- A longer start: about half a second on this mod set (the log line "start-up took" gives the
  time). Exalted is built to stay within 500 ms. A start that has to recover `codes.txt` or
  `stat-ids.txt` from the saves takes longer once.
- After a game update, unique and set rolls on exalted bases may fall back to rare and magic
  until the plugin is updated. The bases themselves still drop.

## Checking it works

Type `exalted` in the game console. Its first line names the build (version, build date and a
hash of its sources); then what was built and how often it was used, and any warning from this
start again. Warnings also show on the console when the game starts, for example:
- "the mod has a stat of its own at ...": a mod update put a stat on a number Exalted's restore
  stats use. Restoring is off; restored items in your saves may read wrong. Back up your saves
  and do not play those characters until this is solved.
- "... of the mod's own Exalted cube rows disagree with this install": your mod brings Exalted
  recipes made for another install (another stat number). They are used as the mod has them,
  which can let an item be restored twice. Fix or remove those rows in the mod.

## Item codes

Type `exalted legend` in the game console. It writes `legend.txt` next to `codes.txt` (see
below), listing every exalted item and material with its item code. Use those codes in your own
cube recipes or loot tables. The codes belong to your computer; on another computer with other
mods a few can differ, so run the command there too.

## Be careful

- **Characters holding exalted items or crystals need the plugin.** Remove those items from
  every character and the shared stash before you remove or turn off the plugin. The same
  goes for restored items, even after they are turned back to elite.
- **Do not edit or delete `codes.txt` or `stat-ids.txt`.** They are in
  `mods/<mod>/d2rloader/exalted-items/`, or in `d2rloader/exalted-items/<mod>/` when the plugin
  is installed for every mod. Saved items name their base by `codes.txt` and their restore by
  `stat-ids.txt`. Reinstalling your mods (rebuilding the mod's `.mpq` folder) does not touch that folder. Exalted keeps a previous copy
  of both in its `backups/` folder. If one goes missing, Exalted puts it back from there, or
  works it out again from your saves (they are only read), and says so on the console. If neither
  works, Exalted stays off and names the characters that hold exalted items: those do not load
  until the files are back. So back up that folder with your saves.
- **Moving to another computer:** take `codes.txt` and `stat-ids.txt` with the saves.
- **Back up your saves** before the first run and before changing versions.
- **Leaving the mod.** Before you remove the plugin or turn it off: turn every exalted item back
  to elite (a Shard of Desecration in the cube; restored items lose their restore), and drop or
  sell every Restoration Crystal and Shard of Desecration, on every character, the mercenary and
  the shared stash. Items restored before v0.11.0 keep Exalted's "Restored" stat even on their
  elite base, and those need the plugin as well.
