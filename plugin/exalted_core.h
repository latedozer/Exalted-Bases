// Exalted Items: generator for a fourth item tier above elite.
// Portable C++ (no Windows calls) so it can be run and checked outside the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace exalted {

// File access. Paths are relative to the "data" folder and use forward slashes,
// for example "global/excel/armor.txt".
struct Io {
	virtual ~Io() = default;
	// A file of the active mod (the folder the game reads its soft-mod files from).
	virtual bool ReadMod(const std::string& rel, std::string& out) = 0;
	// The unmodded game file, used when the mod does not have its own copy.
	virtual bool ReadBase(const std::string& rel, std::string& out) = 0;
	// Stores an adjusted copy of a file. Copies go to a folder of their own: the mod's files
	// are only ever read.
	virtual bool Write(const std::string& rel, const std::string& data) = 0;
	// True when an adjusted copy of that name is already stored. Used for files that are worked
	// out from the game's own art (the crystal's picture), which is not read again while the
	// result is still there.
	virtual bool HasCopy(const std::string& rel) { (void)rel; return false; }
	// Reads a stored adjusted copy (false when there is none).
	virtual bool ReadCopy(const std::string& rel, std::string& out) { (void)rel; (void)out; return false; }
	// File names (no folders) directly inside a mod folder.
	virtual void ListMod(const std::string& relDir, std::vector<std::string>& names) = 0;
	virtual void Log(const char* text) = 0;
};

struct Recipe {
	std::string key;       // "<quality>_<weapon|armor>", e.g. "unique_weapon"
	std::string materials; // item codes separated by spaces, e.g. "r26 r30 gpg"
};

// Tiers of an item family, as bits (for "which tiers does an exalted base borrow from").
enum Tier : int { TierNormal = 1, TierExceptional = 2, TierElite = 4, TierAll = 7 };

// The exalted tier fills kLevelTables of the game's level tables (one table per three item
// levels), counted down from the highest level. The game builds level tables up to 96.
constexpr int kLevelTables      = 10;
constexpr int kDropLevelFloor   = 30; // lowest value the highest level can be set to
constexpr int kDropLevelCeiling = 96;

// --- The Restoration Crystal -------------------------------------------------------------
//
// One item with two uses in the cube: elite item + crystal -> the same item on its exalted
// base, and exalted item + crystal -> the same item with every attribute listed for its kind
// ("restored"), once per item.

// What a restore adds to one kind of item. Each attribute is a property of the game's own
// (properties.txt) with the range it is rolled in.
struct RestoreAttribute {
	const char* key;      // name in the settings file
	const char* property; // properties.txt code
	int         min, max;
	const char* label;
};

struct RestoreKind {
	const char* key;   // settings section: [restore.<key>]
	const char* label;
	const char* types; // item types (itemtypes.txt codes) separated by spaces
	RestoreAttribute attributes[4];
};

// A base belongs to the first kind in this list that one of its item types is: a staff is
// "staves, wands, orbs" before it is a weapon.
inline constexpr RestoreKind kRestoreKinds[] = {
	{ "body_armor", "Body armor", "tors",
	  { { "all_resistances", "res-all", 4, 7, "All resistances +4 to 7%" }, { "enhanced_defense", "ac%", 30, 40, "Enhanced defense +30 to 40%" }, { "maximum_life", "hp%", 3, 4, "Maximum life +3 to 4%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "helms", "Helms", "helm",
	  { { "enhanced_defense", "ac%", 30, 40, "Enhanced defense +30 to 40%" }, { "maximum_life", "hp%", 3, 4, "Maximum life +3 to 4%" }, { "replenish_life", "regen", 5, 10, "Replenish life +5 to 10" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "shields", "Shields", "shld",
	  { { "all_resistances", "res-all", 4, 7, "All resistances +4 to 7%" }, { "enhanced_defense", "ac%", 30, 40, "Enhanced defense +30 to 40%" }, { "chance_to_block", "block", 5, 10, "Chance to block +5 to 10%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "belts", "Belts", "belt",
	  { { "faster_run_walk", "move1", 5, 10, "Faster run/walk +5 to 10%" }, { "maximum_mana", "mana%", 5, 5, "Maximum mana +5%" }, { "attack_speed", "swing1", 5, 10, "Increased attack speed +5 to 10%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "gloves", "Gloves", "glov",
	  { { "attack_speed", "swing1", 5, 10, "Increased attack speed +5 to 10%" }, { "attack_rating", "att%", 5, 10, "Attack rating +5 to 10%" }, { "faster_hit_recovery", "balance1", 5, 10, "Faster hit recovery +5 to 10%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "boots", "Boots", "boot",
	  { { "faster_run_walk", "move1", 5, 10, "Faster run/walk +5 to 10%" }, { "faster_hit_recovery", "balance1", 5, 10, "Faster hit recovery +5 to 10%" }, { "maximum_mana", "mana%", 5, 5, "Maximum mana +5%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "staves_wands_orbs", "Staves, wands, orbs", "staf wand orb",
	  { { "faster_cast_rate", "cast1", 5, 10, "Faster cast rate +5 to 10%" }, { "maximum_mana", "mana%", 5, 5, "Maximum mana +5%" }, { "all_resistances", "res-all", 4, 7, "All resistances +4 to 7%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
	{ "weapons", "Weapons", "weap",
	  { { "enhanced_damage", "dmg%", 15, 30, "Enhanced damage +15 to 30%" }, { "attack_speed", "swing1", 5, 10, "Increased attack speed +5 to 10%" }, { "attack_rating", "att%", 5, 10, "Attack rating +5 to 10%" }, { "requirements", "ease", -10, -5, "Requirements -5 to 10%" } } },
};

// Taking a restore off again. A restore rolls each attribute, so what it added has to be
// written down on the item: every property below gets a twin (`xlt-r<n>`) that puts the same
// roll into a hidden stat of its own as well, and restore recipes use the twin. The hidden
// stats have fixed numbers, counted down from kRollStatTop in this order, because a saved item
// stores a stat by number. NEVER reorder or remove an entry; new ones go at the end.
inline constexpr const char* kRollProperties[] = { "res-all", "ac%", "hp%", "regen", "block", "move1", "mana%", "swing1", "att%", "balance1", "cast1", "dmg%", "ease" };
inline constexpr int         kRollStatTop      = 499;
inline constexpr const char* kRestoredPropertyCode = "xlt-restored"; // the property that marks an item as restored
inline constexpr int         kRollStatCount    = static_cast<int>(sizeof(kRollProperties) / sizeof(kRollProperties[0]));

// The crystal's line in the saved codes (see Config::savedCodes): "*crystal=<item code>".
inline constexpr const char* kCrystalKey = "*crystal";
// The word that stands for the crystal in a list of cube materials.
inline constexpr const char* kCrystalWord = "crystal";
// The Shard of Desecration: a second item made the same way (a red Worldstone Shard, dropped by
// the same bosses), the default material for turning an exalted item back to elite.
inline constexpr const char* kDesecrationKey  = "*desecration";
inline constexpr const char* kDesecrationWord = "desecration";

// A restored item carries a marker stat, which is what makes a second restore impossible. A
// saved item names its stats by number, so the marker has a fixed number that does not move
// when other mods add stats of their own (they are numbered upward from the game's last one;
// the numbers in between are filled with unused rows). Item saves have room for numbers up to 510.
constexpr int kRestoredStatId = 500;

struct Config {
	bool        enabled         = true;
	int         dropLevelMax    = 96;  // item level of the highest exalted bases
	std::string namePrefix      = "Exalted ";
	int         statStepPercent = 100; // 100 = repeat the exceptional -> elite step in full
	int         levelReqCap     = 90;
	// Level requirement added to an item by the cube upgrade. The plugin leaves it at 0: the
	// game cannot store a negative one, so a requirement added here could not be taken off
	// again when an exalted item that dropped (and never got it) is turned back to elite.
	// Only the tests set it, to compare with v0.9.0, which added 9.
	int         upgradeLevelReq = 0;
	int         uniqueTiers     = TierAll; // tiers whose uniques a dropped exalted base can become; 0 = none
	int         setTiers        = TierAll; // the same for set items
	// true: exalted bases drop only in terror zones. Each exalted row gets the game's own
	// condition for that in its DropConditionCalc cell (the one the game's terror zone loot
	// rows use). The game leaves a base whose condition fails out of the roll, so outside a
	// terror zone the level tables give what they give without the plugin.
	bool        terrorZoneOnly  = false;
	std::vector<Recipe> recipes;
	// Materials that turn an exalted item back into the same item on its elite base (any
	// quality). Item codes separated by spaces; "" = no such recipe. r01 is the El rune.
	std::string downgrade = "desecration";
	// The Restoration Crystal. Off: no such item, and recipes that name it are left out.
	bool        crystalEnabled  = true;
	std::string crystalName     = "Restoration Crystal";
	// The colour its name is written in, as the game's colour code character: '8' is the orange
	// the game uses for essences, keys, runes and Worldstone Shards. 0 = no colour code (white).
	// (The game colours its own materials by their item code, which a new item cannot share,
	// so the code is put in front of the name, as mods do for their own materials.)
	char        crystalColor    = '8';
	// The crystal is listed beside the boss essences in the loot rows that hold one (the act
	// bosses in Hell), with this much of the essence's weight. 0 = it does not drop.
	int         crystalDropPercent = 100;
	// The crystal gets a slot of its own in the stash's stacking tabs (beside the Worldstone
	// Shards), where it piles up like the game's own materials. Off: an ordinary item.
	bool        crystalStashSlot = true;
	// Where that slot goes: this many slots to the right of and below the Western Worldstone
	// Shard's slot. When that place is taken (or both are 0) a free place near it is found.
	int         crystalSlotRight = 4, crystalSlotDown = 3;
	// The Shard of Desecration: the same settings as the crystal's.
	bool        desecrationEnabled     = true;
	std::string desecrationName        = "Shard of Desecration";
	char        desecrationColor       = '8';
	int         desecrationDropPercent = 100;
	bool        desecrationStashSlot   = true;
	int         desecrationSlotRight = 3, desecrationSlotDown = 3;
	// The game's loot filter gets a fourth Quality box, beside Normal, Exceptional and Elite,
	// that stands for the exalted bases (written "exalted" in a rule). The core adds the box to
	// the rule panel's layout; the plugin makes the game count an exalted base as that tier.
	// Off: the layout is left alone and the filter sees an exalted base as a Normal one.
	bool        filterBox       = true;
	std::string filterLabel     = "Exalted"; // the text beside the box
	// Restoring: exalted item + these materials -> the same item with the attributes of its kind.
	bool        restoreEnabled  = true;
	std::string restoreMaterial = "crystal";
	bool        restoreShowLine = true; // a restored item says so
	// Turning a restored item back to elite takes the restore attributes off again (and the
	// item can be restored anew). Needs the amounts a restore rolled, which are kept on the
	// item as hidden values (see kRollProperties). Off: nothing is recorded or taken off.
	bool        restoreStrip    = true;
	std::vector<std::string> restoreOff; // attributes turned off, as "<kind key>.<attribute key>"
	// Exalted codes handed out by earlier runs (elite code, exalted code). A family keeps its
	// code for good, whatever other mods add later. The crystal's code is kept here as well,
	// under kCrystalKey.
	std::vector<std::pair<std::string, std::string>> savedCodes;
};

// Uniques and set items are never repeated in the tables. Instead the plugin lets the game's
// own picker treat an exalted base as a base of its family for one call. This is the family:
// the bases (of the tiers the settings allow) that have unique or set rows, and for each row
// the item level it needs and its weight, which is what the picker goes by.
struct FamilyRow {
	int level  = 0;
	int weight = 1;
};

struct FamilyBase {
	std::string code;
	int         tier = 0; // one Tier bit
	std::vector<FamilyRow> rows;
};

struct Family {
	int         bank = 0;     // 0 = global/excel (RotW), 1 = global/excel/base
	bool        sets = false; // false: unique items, true: set items
	std::string code;         // exalted base code
	std::vector<FamilyBase> bases;
};

// Which base of its family an exalted base poses as for one pick. Each base weighs as much as
// the rows the game's picker would accept from it for an item of this level, so choosing a
// base this way and then letting the game pick among that base's rows gives every row the
// same chance as picking from one merged list of the whole family. A base with nothing to
// give at that level weighs nothing and is never chosen.
constexpr int kMaxFamilyBases = 8;

inline uint32_t FamilyWeights(const Family& family, int itemLevel, uint32_t weights[kMaxFamilyBases]) {
	uint32_t total = 0;
	for (size_t i = 0; i < static_cast<size_t>(kMaxFamilyBases); ++i) {
		weights[i] = 0;
		if (i < family.bases.size()) {
			for (const FamilyRow& row : family.bases[i].rows) {
				if (row.level <= itemLevel) {
					weights[i] += static_cast<uint32_t>(row.weight);
				}
			}
		}
		total += weights[i];
	}
	return total;
}

// The base a roll in [0, total) lands on; -1 when there is none.
inline int FamilyChoice(const uint32_t weights[kMaxFamilyBases], uint32_t roll) {
	for (int i = 0; i < kMaxFamilyBases; ++i) {
		if (roll < weights[i]) {
			return i;
		}
		roll -= weights[i];
	}
	return -1;
}

// One line of the item code legend (the 'exalted legend' console command).
struct LegendEntry {
	std::string exalted;     // exalted base code
	std::string elite;       // elite base code
	std::string name;        // English name of the exalted base (as the game shows it)
	std::string type;        // item type code (weapons.txt / armor.txt "type")
	std::string typeName;    // that type's name in itemtypes.txt
	bool        weapon  = false;
	int         level    = 0; // item level
	int         levelReq = 0; // required level
	bool        unique  = false; // can drop as a unique item
	bool        set     = false; // can drop as a set item
};

struct Summary {
	bool        ok           = false;
	std::string error;
	int         bases        = 0; // exalted base items (distinct codes)
	int         recipes      = 0; // cube upgrade recipes added per data bank
	int         downgrades   = 0; // cube recipes back to elite added per data bank (one per exalted base)
	int         names        = 0;
	int         models       = 0;
	int         uniques      = 0; // exalted bases that can drop as a unique item
	int         sets         = 0; // exalted bases that can drop as a set item
	int         levelLow     = 0; // lowest and highest item level given to an exalted base
	int         levelHigh    = 0;
	int         rungs        = 0; // loot rows added above the game's own (first data bank)
	int         repointed    = 0; // existing high-level loot rows pointed at the new rows
	std::string crystalCode;        // item code of the Restoration Crystal ("" = there is none)
	bool        crystalTinted  = false; // it has a picture of its own (the tinted shard)
	int         crystalRows    = 0; // loot rows the crystal was added to (first data bank)
	int         crystalSlots   = 0; // stash layouts that hold a slot for the crystal (0 = it does not stack)
	std::string desecrationCode;        // item code of the Shard of Desecration ("" = there is none)
	bool        desecrationTinted = false;
	int         desecrationRows   = 0;
	int         desecrationSlots  = 0;
	int         rollStats      = 0; // restore attributes whose roll is written down on the item (0 = a restore cannot be taken off)
	int         restores       = 0; // restore recipes added per data bank
	bool        filterBox      = false; // the loot filter's rule panel layout has the Exalted box
	bool        terrorZoneOnly = false; // the exalted rows carry the terror zone condition
	int         zoneRows     = 0; // loot rows added that open a level table above the game's own only in a terror zone
	std::vector<Family> families;
	std::vector<std::pair<std::string, std::string>> codes; // every (elite, exalted) code of this run
	int         filesWritten = 0;
	std::vector<std::string> files; // every adjusted file of this run (relative paths)
	int         banks        = 0;
	std::vector<LegendEntry> legend; // every exalted base of the first data bank
};

Summary Run(Io& io, const Config& config);

// The loot filter's rule panel layout with one more Quality box, "ExaltedQualityChbx", after
// the Elite one: a copy of the Elite box that sends "exalted", `shift` to its right, with
// `label` beside it. Returns false (and says why) when the layout is not the expected one;
// `layout` is unchanged then. A layout that already has the box counts as done.
bool AddFilterBox(std::string& layout, const std::string& label, std::string& why);

// The legend as plain text (UTF-8, Windows line ends): the two materials, then every exalted
// base grouped by kind, with its code, its elite base's code, its required level and whether
// it can drop as a unique or set item. `heading` goes on top, line by line.
std::string FormatLegend(const Summary& summary, const Config& config, const std::string& heading);

}
