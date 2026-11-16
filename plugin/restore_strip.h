// Exalted Items: taking a restore off an item again.
//
// A restore adds rolled amounts, and the cube has no way to take attributes off. So the restore
// recipes use "twin" properties that write each roll into a hidden stat of the item as well
// (see kRollProperties in exalted_core.h), and the recipe that turns an exalted item back to
// elite carries a sign: the marker property with an amount of nothing. When the game applies
// that sign, the plugin instead applies every twin again with the opposite of what the item's
// hidden stat holds, through the game's own "apply a property" routine. That takes the
// attribute and its hidden stat off in one go; then the marker itself.
//
// An item restored before the rolls were written down (0.10.x) has no hidden stats: it keeps
// its attributes and its marker.
//
// Portable (no Windows or game calls): the game is reached through `Engine`, so the logic can
// be tested outside the game.
#pragma once

#include "exalted_core.h"

#include <cstdint>
#include <cstring>

namespace exalted {

// What the game's property routine is handed (also the layout the loader's SDK documents).
struct PropertyCall {
	int32_t id;      // properties.txt row
	int32_t param;
	int32_t min, max;
};

// A compiled properties.txt row, as far as it is read here.
constexpr size_t kPropertyStats  = 0x0a; // int16[7]: the stat of each function
constexpr size_t kPropertyFuncs  = 0x18; // uint8[7]: the function numbers
constexpr int    kPropertySlots  = 7;

inline int PropertyStat(const uint8_t* row, int slot) {
	int16_t stat;
	memcpy(&stat, row + kPropertyStats + static_cast<size_t>(slot) * 2, sizeof(stat));
	return stat;
}

// The twin of each roll stat, per data version (the two sets of tables number their rows apart).
struct TwinTable {
	bool    known[256]                  = {};
	int32_t twin[256][kRollStatCount]   = {};
};

// Engine needs:
//   int            Version(void* item)
//   int            PropertyCount(int version)
//   const uint8_t* Property(int version, int id)      (nullptr when there is none)
//   int            Stat(void* item, int stat)
//   void           Apply(const PropertyCall& call)    (the game's routine, for the same item and arguments as the sign)

// True when `call` is the sign: the marker property (first stat = the marker, function 1) with
// an amount of nothing.
template <typename Engine>
bool IsStripSign(Engine& engine, void* item, const PropertyCall& call) {
	if (item == nullptr || call.min != 0 || call.max != 0 || call.id < 0 || call.id > 0xFFFF) {
		return false;
	}
	const uint8_t* row = engine.Property(engine.Version(item), call.id);
	return row != nullptr && row[kPropertyFuncs] == 1 && PropertyStat(row, 0) == kRestoredStatId;
}

// Takes the restore off. Returns how many attributes were taken off (0: the item has no
// written-down rolls, and nothing was changed).
template <typename Engine>
int StripRestore(Engine& engine, void* item, const PropertyCall& sign, TwinTable& table) {
	const int version = engine.Version(item) & 0xFF;
	if (!table.known[version]) {
		for (int32_t& twin : table.twin[version]) {
			twin = -1;
		}
		const int count = engine.PropertyCount(version);
		for (int id = 0; id < count && id <= 0xFFFF; ++id) {
			const uint8_t* row = engine.Property(version, id);
			for (int slot = 1; row != nullptr && slot < kPropertySlots; ++slot) {
				const int stat = PropertyStat(row, slot);
				if (row[kPropertyFuncs + slot] == 3 && stat <= kRollStatTop && stat > kRollStatTop - kRollStatCount) {
					table.twin[version][kRollStatTop - stat] = id;
				}
			}
		}
		table.known[version] = true;
	}
	int taken = 0;
	for (int k = 0; k < kRollStatCount; ++k) {
		const int amount = engine.Stat(item, kRollStatTop - k);
		if (amount == 0 || table.twin[version][k] < 0) {
			continue;
		}
		engine.Apply(PropertyCall { table.twin[version][k], 0, -amount, -amount });
		++taken;
	}
	if (taken > 0 && engine.Stat(item, kRestoredStatId) != 0) {
		engine.Apply(PropertyCall { sign.id, 0, -1, -1 });
	}
	return taken;
}

}
