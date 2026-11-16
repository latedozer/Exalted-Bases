# Exalted Items: user guide (v0.11.0)

Exalted Items adds a fourth tier of weapons and armor above elite, called Exalted.

v0.11.0 has passed its offline checks but has not been played yet. The Restoration Crystal,
its stash slot, the cube recipes and multiplayer are untested in the game.

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

**Back to elite.** Any exalted item plus an El rune becomes the same item on its elite base.
This works for plain, magic, rare, set and unique items.

## The Restoration Crystal

A purple Worldstone Shard. It cannot be used on its own; it goes in the cube.

**Where it drops.** Only from bosses in Hell: the five act bosses, Blood Raven, the Countess
and Nihlathak. An act boss can drop it about as often as its essence. The other three have
the same chance per drop as Andariel.

**In the stash.** Crystals pile up in the stash's stacking tabs, like runes and Worldstone
Shards. The crystal has its own slot there, in a free place that continues a row of
slots near the Western Worldstone Shard. With the stash layout of this mod set that is the
place left of Diablo's Horn. The slot's frame is painted into a copy of the stash background the first
time the game starts with it. Take one out to use it in the cube. The size of a pile is the loader's
`set_materials_limit`.

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

**Back to elite takes the restore off.** Turning a restored item back to elite (the El rune
recipe) removes exactly what the restore added, and the "Restored" line with it. The item is
then as it was before the restore, on its elite base, and can be upgraded and restored again.
The crystal is not given back. This works only for items restored with v0.11.0 or later: a
restore now writes the amounts it rolled onto the item as hidden values, and older restored
items have none, so they keep their attributes. It needs the game build the plugin knows; in
another build the `exalted` console line says so and restored items keep their attributes.

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
| `downgrade` (in `[recipes]`) | `"r01"` (El) | Material that turns an exalted item back to elite |
| `[crystal]` `enabled` | `true` | `false`: no crystal. Give the recipes other materials then |
| `[crystal]` `name` | `"Restoration Crystal"` | Its name |
| `[crystal]` `name_color` | `"orange"` | Colour of the name. Orange is what the game uses for essences, keys and Worldstone Shards |
| `[crystal]` `drop_weight_percent` | `100` | How often bosses drop it, compared with their essence. `0` = never |
| `[restore]` `strip_on_downgrade` | `true` | Back to elite takes a restore off. `false`: nothing is written down and a restored item keeps its attributes |
| `[crystal]` `stash_slot` | `true` | The crystal has a slot in the stash's stacking tabs. `false`: an ordinary item. Take your crystals out of that slot before you turn this off |
| `[restore]` `enabled` | `true` | Turns restoring on or off |
| `[restore]` `show_line` | `true` | Whether a restored item says "Restored" |
| `[restore.<kind>]` | all `true` | One line per attribute. `false` leaves it out of later restores |

## Other mods

The tier is rebuilt from your mods every time the game starts.

- Changes to elite bases carry over. If a mod gives gloves sockets, exalted gloves have them.
- A new base gets an exalted version if a mod adds it as the top tier of a family.
- Your mod's files are never changed.

## What to expect

- No effect on frame rate.
- A slightly longer start, well under a tenth of a second.
- After a game update, unique and set rolls on exalted bases may fall back to rare and magic
  until the plugin is updated. The bases themselves still drop.

## Checking it works

Type `exalted` in the game console. It shows what was built and how often it was used.

## Be careful

- **Characters holding exalted items or crystals need the plugin.** Remove those items from
  every character and the shared stash before you remove or turn off the plugin. The same
  goes for restored items, even after they are turned back to elite.
- **Do not edit or delete `codes.txt`.** It is in `mods/<mod>/d2rloader/exalted-items/`, or in
  `d2rloader/exalted-items/<mod>/` when the plugin is installed for every mod.
- **Back up your saves** before the first run and before changing versions.
