// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Latedozer
// Taken from Save Scan (save_core) and relicensed to MIT by its author on 2026-11-26.
// Exalted Items' own save reader: it reads saved items, and never writes. No game, no Windows: it
// works on file bytes and plain tables, so the tests run it natively against copies of real saves.
//
// The item layout below is D2R 3.4 (save version 105, "Reign of the Warlock"), worked out from
// the user's own saves in November 2026:
//
//   32 flags (bit 4 identified, 11 socketed, 16 ear, 21 simple, 24 personalized, 26 runeword,
//             28 "recorded": a block of 55 more bits at the very end)
//   3 version, 3 location, 4 equipped place, 4 x, 4 y, 3 page
//   the item code, Huffman coded, up to a space
//   simple item:  [2 bits when the item's table row has questdiffcheck = 1]
//                 1 bit: an id follows -> 128 bits
//                 1 bit: a quantity follows -> 8 bits
//   other item:   3 bits: items in its sockets (they follow it in the list)
//                 32 id, 7 level, 4 quality, 1 (+3) picture, 1 (+11) class-specific
//                 by quality: low/superior 3, normal charm 12, magic 22, set/unique 12,
//                             rare/crafted 16 + 6 x (1 + 11)
//                 runeword: 16;  personalized: 8-bit letters up to a 0;  tome: 5
//                 1 bit: an id follows -> 128 bits;  1 bit (meaning not known)
//                 armor: 11 defense;  armor and weapons: 8 max durability, then 9 current if not 0
//                 classic stackable: 9 quantity;  socketed: 4 sockets;  set: 5 bits list mask
//                 the stat list, one more per bit of the set mask, one more for a runeword
//                 1 bit: a quantity follows -> 8 bits;  flag 28: 55 bits
//   then on to the next whole byte.
//
// A stat list is: 9-bit stat number (0x1ff ends it), then for that stat - and for the one or
// two stats that always follow it (17; 48, 50, 52; 54 and 57 with two) - its param bits and
// its save bits, both from the stat table. That table is handed in: Exalted Items gives it the
// tables it built, which are the ones the game saves with.
//
// Nothing here guesses its way on: a list of items counts as read only when the reader ends
// exactly on the marker or section end that must follow it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace exalted::saves {

struct StatShape {
	uint8_t saveBits  = 0; // 0: the stat cannot be on a saved item
	int32_t saveAdd   = 0;
	uint8_t paramBits = 0;
	uint8_t csvBits   = 0; // CSvBits: the stat's bits in a character's attributes ("gf"); 0 = not there
};

enum : uint8_t { kMisc = 0, kArmor = 1, kWeapon = 2 };
struct ItemKind {
	uint8_t kind      = kMisc;
	bool    stackable = false; // the table's own "stackable" column (9-bit quantity)
	bool    questDiff = false; // questdiffcheck = 1
	bool    charm     = false; // type scha / mcha / lcha
	bool    tome      = false; // type book
};

struct Tables {
	std::vector<StatShape>                    stats;
	std::unordered_map<std::string, ItemKind> items;
};

// One ItemStatCost row of the game is 0x144 bytes; these are the places the reader needs.
constexpr size_t kStatRow = 0x144, kRowSaveBits = 0x15, kRowSaveAdd = 0x18, kRowSaveParam = 0x20;
constexpr size_t kRowCsvBits = 0xa; // CSvBits (from the game's column table at 0x31e310)
void ShapesFromRows(Tables& tables, const uint8_t* rows, size_t count);
// Stats 0..15 have the CSvBits every table has (10,10,10,10,10,8,21 x6,7,32,25,25)? (a check of the row layout)
bool UsualCsvBits(const Tables& tables);
// armor.txt / weapons.txt / misc.txt as text; rows of several files add up.
void ReadItemTable(Tables& tables, std::string_view text, uint8_t kind);
// itemstatcost.txt as text (the tables Exalted Items builds, and the tests).
void ShapesFromText(Tables& tables, std::string_view text);

struct StatValue {
	uint16_t stat;
	int32_t  value;
	uint32_t param;
	uint8_t  list     = 0;     // which of the item's stat lists it is in
	bool     follower = false; // saved together with the stat before it (no id of its own in the file)
};

// A saved affix id of an item: magic items have a prefix and a suffix (11 bits each, 0 = none);
// rare and crafted items up to 6 (prefix, suffix, prefix, ...), each a 1-bit flag + 11 bits.
struct AffixSlot {
	uint8_t  suffix  = 0;     // 0 prefix, 1 suffix
	uint8_t  slot    = 0;     // magic: 0 prefix, 1 suffix; rare/crafted: 0..5
	bool     flagged = false; // rare/crafted (the id has a presence flag before it)
	uint16_t id      = 0;
	uint32_t bit     = 0;     // where its 11 bits are (rare/crafted: its flag is the bit before)
};

struct Item {
	std::string code;
	uint32_t    flags    = 0;
	uint8_t     location = 0, equipped = 0, x = 0, y = 0, page = 0, quality = 0, level = 0;
	bool        inSocket = false; // sits in the item before it
	uint16_t    quantity = 0;
	uint32_t    offset   = 0;     // where it starts in the file
	std::string section;          // "items", "corpse", "mercenary", "golem", "stash tab 3", "stash materials"
	std::vector<StatValue> stats;
	// where it is, in bits from the file's start (for the writer): the item, and each stat list
	// (from its first stat id to after its 0x1ff end mark). bitEnd = before the pad to a byte.
	uint32_t bitStart = 0, bitEnd = 0;
	std::vector<std::pair<uint32_t, uint32_t>> lists;
	std::vector<AffixSlot>                     affixes; // magic: always both; rare/crafted: the ones present
};

struct FileResult {
	bool              ok = false; // every list of the file was read to its end marker
	std::string       problem;    // why not
	std::vector<Item> items;      // what was read (all of it when ok)
	// .d2s: the character's attributes ("gf"), read with csvBits. attributesRead = false: why in attributesProblem.
	bool                   attributesRead = false;
	std::string            attributesProblem;
	std::vector<StatValue> attributes;
};

FileResult ReadCharacter(const uint8_t* data, size_t size, const Tables& tables); // .d2s
FileResult ReadStash(const uint8_t* data, size_t size, const Tables& tables);     // .d2i

// "worn", "inventory", "stash", "cube", "belt", "in a socket" ...
std::string Where(const Item& item);

} // namespace exalted::saves
