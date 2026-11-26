// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Latedozer
// Taken from Save Scan (save_core) and relicensed to MIT by its author on 2026-11-26.
#include "save_reader.h"

#include <cstring>

namespace exalted::saves {

namespace {

// The item code's Huffman table (D2R). Each entry: the letter and its bits in reading order.
struct Code {
	char        letter;
	const char* bits;
};
constexpr Code kCodes[] = {
	{ ' ', "10" },       { '0', "11111011" }, { '1', "1111100" },  { '2', "001100" },   { '3', "1101101" },  { '4', "11111010" }, { '5', "00010110" }, { '6', "1101111" },
	{ '7', "01111" },    { '8', "000100" },   { '9', "01110" },    { 'a', "11110" },    { 'b', "0101" },     { 'c', "01000" },    { 'd', "110001" },   { 'e', "110000" },
	{ 'f', "010011" },   { 'g', "11010" },    { 'h', "00011" },    { 'i', "1111110" },  { 'j', "000101110" }, { 'k', "010010" },  { 'l', "11101" },    { 'm', "01101" },
	{ 'n', "001101" },   { 'o', "1111111" },  { 'p', "11001" },    { 'q', "11011001" }, { 'r', "11100" },    { 's', "0010" },     { 't', "01100" },    { 'u', "00001" },
	{ 'v', "1101110" },  { 'w', "00000" },    { 'x', "00111" },    { 'y', "0001010" },  { 'z', "11011000" },
};

struct Bits {
	const uint8_t* data;
	size_t         size; // bytes
	size_t         at;   // bits
	bool           bad = false;

	uint64_t take(unsigned count) {
		uint64_t value = 0;
		for (unsigned i = 0; i < count; ++i) {
			const size_t bit = at + i;
			if ((bit >> 3) >= size) {
				bad = true;
				at += count;
				return 0;
			}
			if (i < 64) {
				value |= static_cast<uint64_t>((data[bit >> 3] >> (bit & 7)) & 1) << i;
			}
		}
		at += count;
		return value;
	}
	void skip(size_t count) {
		at += count;
		bad = bad || ((at + 7) >> 3) > size;
	}
	void toByte() { at = (at + 7) & ~static_cast<size_t>(7); }
	size_t byte() const { return at >> 3; }
};

bool ReadCode(Bits& bits, std::string& code) {
	code.clear();
	for (;;) {
		char pattern[12] = {};
		int  length      = 0;
		char letter      = 0;
		while (letter == 0) {
			if (length >= 9) {
				return false;
			}
			pattern[length++] = bits.take(1) != 0 ? '1' : '0';
			if (bits.bad) {
				return false;
			}
			for (const Code& entry : kCodes) {
				if (strcmp(entry.bits, pattern) == 0) {
					letter = entry.letter;
					break;
				}
			}
		}
		if (letter == ' ') {
			return !code.empty();
		}
		code += letter;
		if (code.size() > 4) {
			return false;
		}
	}
}

int Followers(uint16_t stat) { // stats saved together with this one
	return stat == 17 || stat == 48 || stat == 50 || stat == 52 ? 1 : stat == 54 || stat == 57 ? 2 : 0;
}

bool ReadStats(Bits& bits, const Tables& tables, std::vector<StatValue>& out, std::string& problem, uint8_t list) {
	for (int guard = 0; guard < 512; ++guard) {
		const uint16_t id = static_cast<uint16_t>(bits.take(9));
		if (bits.bad) {
			problem = "the file ends inside a stat list";
			return false;
		}
		if (id == 0x1ff) {
			return true;
		}
		for (int k = 0; k <= Followers(id); ++k) {
			const size_t stat = static_cast<size_t>(id) + static_cast<size_t>(k);
			if (stat >= tables.stats.size() || tables.stats[stat].saveBits == 0 || tables.stats[stat].saveBits > 32 || tables.stats[stat].paramBits > 32) {
				problem = "stat " + std::to_string(stat) + (stat >= tables.stats.size() ? " is past the stat table" : " has no save bits in the stat table");
				return false;
			}
			const StatShape& shape = tables.stats[stat];
			const uint32_t   param = shape.paramBits != 0 ? static_cast<uint32_t>(bits.take(shape.paramBits)) : 0;
			const int64_t    raw   = static_cast<int64_t>(bits.take(shape.saveBits));
			out.push_back({ static_cast<uint16_t>(stat), static_cast<int32_t>(raw - shape.saveAdd), param, list, k > 0 });
		}
	}
	problem = "a stat list has no end";
	return false;
}

bool ReadItem(Bits& bits, const Tables& tables, Item& item, unsigned& inSockets, std::string& problem) {
	const size_t start = bits.at;
	item               = Item {};
	item.offset        = static_cast<uint32_t>(start >> 3);
	item.bitStart      = static_cast<uint32_t>(start);
	item.flags         = static_cast<uint32_t>(bits.take(32));
	bits.take(3);
	item.location = static_cast<uint8_t>(bits.take(3));
	item.equipped = static_cast<uint8_t>(bits.take(4));
	item.x        = static_cast<uint8_t>(bits.take(4));
	item.y        = static_cast<uint8_t>(bits.take(4));
	item.page     = static_cast<uint8_t>(bits.take(3));
	inSockets     = 0;
	const bool simple = (item.flags >> 21 & 1) != 0;
	const auto fail   = [&](const std::string& why) {
		problem = "item at byte " + std::to_string(item.offset) + (item.code.empty() ? "" : " (" + item.code + ")") + ": " + why;
		return false;
	};
	ItemKind kind;
	if ((item.flags >> 16 & 1) != 0) { // an ear: class, level, the name. NOT confirmed on a real file.
		item.code = "ear";
		bits.take(3);
		bits.take(7);
		for (int i = 0; i < 16 && bits.take(7) != 0 && !bits.bad; ++i) {
		}
	} else {
		if (!ReadCode(bits, item.code)) {
			return fail("its code cannot be read");
		}
		const auto found = tables.items.find(item.code);
		if (found == tables.items.end()) {
			return fail("its code is in none of the item tables, so its shape is unknown");
		}
		kind = found->second;
	}
	if (simple) {
		if (kind.questDiff) {
			bits.take(2);
		}
		if (bits.take(1) != 0) {
			bits.skip(128);
		}
		if (bits.take(1) != 0) {
			item.quantity = static_cast<uint16_t>(bits.take(8));
		}
	} else {
		inSockets = static_cast<unsigned>(bits.take(3));
		bits.take(32);
		item.level   = static_cast<uint8_t>(bits.take(7));
		item.quality = static_cast<uint8_t>(bits.take(4));
		if (bits.take(1) != 0) {
			bits.take(3);
		}
		if (bits.take(1) != 0) {
			bits.take(11);
		}
		const uint8_t q = item.quality;
		if (q == 1 || q == 3) {
			bits.take(3);
		} else if (q == 2) {
			if (kind.charm) {
				bits.take(12);
			}
		} else if (q == 4) {
			for (uint8_t k = 0; k < 2; ++k) {
				AffixSlot slot;
				slot.suffix = k;
				slot.slot   = k;
				slot.bit    = static_cast<uint32_t>(bits.at);
				slot.id     = static_cast<uint16_t>(bits.take(11));
				item.affixes.push_back(slot);
			}
		} else if (q == 5 || q == 7) {
			bits.take(12);
		} else if (q == 6 || q == 8) {
			bits.take(16);
			for (uint8_t i = 0; i < 6; ++i) {
				if (bits.take(1) != 0) {
					AffixSlot slot;
					slot.suffix  = i & 1;
					slot.slot    = i;
					slot.flagged = true;
					slot.bit     = static_cast<uint32_t>(bits.at);
					slot.id      = static_cast<uint16_t>(bits.take(11));
					item.affixes.push_back(slot);
				}
			}
		} else {
			return fail("quality " + std::to_string(q) + " is not one the reader knows");
		}
		const bool runeword = (item.flags >> 26 & 1) != 0;
		if (runeword) {
			bits.take(16);
		}
		if ((item.flags >> 24 & 1) != 0) { // a name: NOT confirmed on a real file
			for (int i = 0; i < 64 && bits.take(8) != 0 && !bits.bad; ++i) {
			}
		}
		if (kind.tome) {
			bits.take(5);
		}
		if (bits.take(1) != 0) {
			bits.skip(128);
		}
		bits.take(1);
		if (kind.kind == kArmor) {
			bits.take(11);
		}
		if (kind.kind == kArmor || kind.kind == kWeapon) {
			if (bits.take(8) != 0) {
				bits.take(9);
			}
		}
		if (kind.stackable) {
			item.quantity = static_cast<uint16_t>(bits.take(9));
		}
		if ((item.flags >> 11 & 1) != 0) {
			bits.take(4);
		}
		const unsigned sets = q == 5 ? static_cast<unsigned>(bits.take(5)) : 0;
		if (bits.bad) {
			return fail("the file ends inside it");
		}
		std::string why;
		int         lists = 1 + (runeword ? 1 : 0);
		for (unsigned i = 0; i < 5; ++i) {
			lists += (sets >> i & 1) != 0 ? 1 : 0;
		}
		for (int i = 0; i < lists; ++i) {
			const size_t from = bits.at;
			if (!ReadStats(bits, tables, item.stats, why, static_cast<uint8_t>(i))) {
				return fail(why);
			}
			item.lists.emplace_back(static_cast<uint32_t>(from), static_cast<uint32_t>(bits.at));
		}
		if (bits.take(1) != 0) {
			item.quantity = static_cast<uint16_t>(bits.take(8));
		}
		if ((item.flags >> 28 & 1) != 0) {
			bits.skip(55);
		}
	}
	if (bits.bad) {
		return fail("the file ends inside it");
	}
	item.bitEnd = static_cast<uint32_t>(bits.at);
	bits.toByte();
	return true;
}

// `count` items (each followed by the items in its sockets) from byte `at`. True: all read; `at` is the byte after them.
bool ReadList(const uint8_t* data, size_t size, size_t& at, unsigned count, const Tables& tables, const std::string& section, std::vector<Item>& out, std::string& problem) {
	Bits bits { data, size, at * 8 };
	for (unsigned n = 0; n < count; ++n) {
		Item     item;
		unsigned inside = 0;
		if (!ReadItem(bits, tables, item, inside, problem)) {
			problem = section + ", item " + std::to_string(n + 1) + " of " + std::to_string(count) + ": " + problem;
			return false;
		}
		item.section = section;
		out.push_back(std::move(item));
		for (unsigned s = 0; s < inside; ++s) {
			Item     socketed;
			unsigned none = 0;
			if (!ReadItem(bits, tables, socketed, none, problem)) {
				problem = section + ", item " + std::to_string(n + 1) + " of " + std::to_string(count) + ", in its socket " + std::to_string(s + 1) + ": " + problem;
				return false;
			}
			socketed.section  = section;
			socketed.inSocket = true;
			out.push_back(std::move(socketed));
		}
	}
	at = bits.byte();
	return true;
}

bool Is(const uint8_t* data, size_t size, size_t at, const char* two) {
	return at + 2 <= size && data[at] == static_cast<uint8_t>(two[0]) && data[at + 1] == static_cast<uint8_t>(two[1]);
}
uint16_t Word(const uint8_t* data, size_t at) {
	return static_cast<uint16_t>(data[at] | data[at + 1] << 8);
}
uint32_t Dword(const uint8_t* data, size_t at) {
	uint32_t v;
	memcpy(&v, data + at, 4);
	return v;
}

// "JM", a count, that many items.
bool ReadMarked(const uint8_t* data, size_t size, size_t& at, const Tables& tables, const std::string& section, std::vector<Item>& out, std::string& problem) {
	if (!Is(data, size, at, "JM") || at + 4 > size) {
		problem = section + ": no item list where one must be (byte " + std::to_string(at) + ")";
		return false;
	}
	const unsigned count = Word(data, at + 2);
	at += 4;
	return ReadList(data, size, at, count, tables, section, out, problem);
}

std::vector<std::string_view> Split(std::string_view line) {
	std::vector<std::string_view> cells;
	for (size_t from = 0; from <= line.size();) {
		size_t tab = line.find('\t', from);
		tab        = tab == std::string_view::npos ? line.size() : tab;
		cells.push_back(line.substr(from, tab - from));
		from = tab + 1;
	}
	return cells;
}

// Calls row(cells, column-of) for each line under the header.
template <typename Fn>
void EachRow(std::string_view text, Fn row) {
	if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xef && static_cast<uint8_t>(text[1]) == 0xbb && static_cast<uint8_t>(text[2]) == 0xbf) {
		text.remove_prefix(3);
	}
	std::unordered_map<std::string, size_t> columns;
	bool                                    first = true;
	for (size_t from = 0; from < text.size();) {
		size_t end = text.find('\n', from);
		end        = end == std::string_view::npos ? text.size() : end;
		std::string_view line = text.substr(from, end - from);
		from                  = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		const std::vector<std::string_view> cells = Split(line);
		if (first) {
			for (size_t i = 0; i < cells.size(); ++i) {
				columns.emplace(std::string(cells[i]), i);
			}
			first = false;
			continue;
		}
		const auto cell = [&](const char* name) -> std::string_view {
			const auto found = columns.find(name);
			return found != columns.end() && found->second < cells.size() ? cells[found->second] : std::string_view();
		};
		row(cell);
	}
}

long Number(std::string_view text) {
	return text.empty() ? 0 : strtol(std::string(text).c_str(), nullptr, 10);
}

// The character's attributes: "gf", then 9-bit stat ids each with its CSvBits value bits, 0x1ff,
// up to the next whole byte, which must be the skills' "if" at `skills`.
void ReadAttributes(const uint8_t* data, size_t size, const Tables& tables, FileResult& result) {
	result.attributesProblem = "no \"gf\" whose attributes end where the skills start";
	for (size_t p = 0x2a0; p + 2 < size; ++p) {
		if (!Is(data, size, p, "gf")) {
			continue;
		}
		Bits                   bits { data, size, (p + 2) * 8 };
		std::vector<StatValue> found;
		bool                   good = false;
		for (int guard = 0; guard < 64 && !bits.bad; ++guard) {
			const uint16_t id = static_cast<uint16_t>(bits.take(9));
			if (bits.bad) {
				break;
			}
			if (id == 0x1ff) {
				bits.toByte();
				good = bits.byte() == size;
				break;
			}
			if (id >= tables.stats.size() || tables.stats[id].csvBits == 0 || tables.stats[id].csvBits > 32) {
				break;
			}
			found.push_back({ id, static_cast<int32_t>(bits.take(tables.stats[id].csvBits)), 0, 0, false });
		}
		if (good) {
			result.attributesRead = true;
			result.attributesProblem.clear();
			result.attributes = std::move(found);
			return;
		}
	}
	bool known = false;
	for (const StatShape& shape : tables.stats) {
		known = known || shape.csvBits != 0;
	}
	if (!known) {
		result.attributesProblem = "the stat table has no CSvBits";
	}
}

} // namespace

void ShapesFromRows(Tables& tables, const uint8_t* rows, size_t count) {
	tables.stats.assign(count, StatShape {});
	for (size_t i = 0; i < count; ++i) {
		const uint8_t* row = rows + i * kStatRow;
		int32_t        add = 0, param = 0;
		memcpy(&add, row + kRowSaveAdd, 4);
		memcpy(&param, row + kRowSaveParam, 4);
		tables.stats[i].saveBits  = row[kRowSaveBits];
		tables.stats[i].saveAdd   = add;
		tables.stats[i].paramBits = param >= 0 && param <= 255 ? static_cast<uint8_t>(param) : 255; // 255: refused when met
	}
	for (size_t i = 0; i < count; ++i) {
		tables.stats[i].csvBits = rows[i * kStatRow + kRowCsvBits];
	}
}

bool UsualCsvBits(const Tables& tables) {
	static const uint8_t kUsual[16] = { 10, 10, 10, 10, 10, 8, 21, 21, 21, 21, 21, 21, 7, 32, 25, 25 };
	if (tables.stats.size() < 16) {
		return false;
	}
	for (size_t i = 0; i < 16; ++i) {
		if (tables.stats[i].csvBits != kUsual[i]) {
			return false;
		}
	}
	return true;
}

void ShapesFromText(Tables& tables, std::string_view text) {
	tables.stats.clear();
	EachRow(text, [&](const auto& cell) {
		StatShape shape;
		shape.saveBits  = static_cast<uint8_t>(Number(cell("Save Bits")));
		shape.saveAdd   = static_cast<int32_t>(Number(cell("Save Add")));
		shape.paramBits = static_cast<uint8_t>(Number(cell("Save Param Bits")));
		shape.csvBits   = static_cast<uint8_t>(Number(cell("CSvBits")));
		tables.stats.push_back(shape);
	});
}

void ReadItemTable(Tables& tables, std::string_view text, uint8_t kind) {
	EachRow(text, [&](const auto& cell) {
		const std::string_view code = cell("code");
		if (code.empty() || code.size() > 4) {
			return;
		}
		const std::string_view type = cell("type");
		ItemKind               entry;
		entry.kind      = kind;
		entry.stackable = cell("stackable") == "1";
		entry.questDiff = cell("questdiffcheck") == "1";
		entry.charm     = type == "scha" || type == "mcha" || type == "lcha";
		entry.tome      = type == "book";
		tables.items[std::string(code)] = entry;
	});
}

FileResult ReadCharacter(const uint8_t* data, size_t size, const Tables& tables) {
	FileResult result;
	if (size < 0x300 || Dword(data, 0) != 0xaa55aa55u) {
		result.problem = "not a character file";
		return result;
	}
	if (Dword(data, 4) != 105) {
		result.problem = "save version " + std::to_string(Dword(data, 4)) + " (the reader knows 105)";
		return result;
	}
	// the skills section ("if" + 30 bytes) is the last thing before the items
	size_t at = 0;
	for (size_t p = 0x300; p + 36 <= size && at == 0; ++p) {
		if (Is(data, size, p, "if") && Is(data, size, p + 32, "JM")) {
			at = p + 32;
		}
	}
	if (at == 0) {
		result.problem = "the item list was not found";
		return result;
	}
	ReadAttributes(data, at - 32, tables, result);
	if (!ReadMarked(data, size, at, tables, "items", result.items, result.problem)) {
		return result;
	}
	// the corpse: "JM", how many; each: 12 bytes, then its own item list
	if (!Is(data, size, at, "JM") || at + 4 > size) {
		result.problem = "the items do not end where the corpse section starts (byte " + std::to_string(at) + ")";
		return result;
	}
	const unsigned corpses = Word(data, at + 2);
	at += 4;
	for (unsigned c = 0; c < corpses; ++c) {
		at += 12;
		if (!ReadMarked(data, size, at, tables, "corpse", result.items, result.problem)) {
			return result;
		}
	}
	if (at == size) { // a classic character ends here
		result.ok = true;
		return result;
	}
	if (!Is(data, size, at, "jf")) {
		result.problem = "the corpse section does not end where the mercenary section starts (byte " + std::to_string(at) + ")";
		return result;
	}
	at += 2;
	if (Is(data, size, at, "JM") && !ReadMarked(data, size, at, tables, "mercenary", result.items, result.problem)) {
		return result;
	}
	if (!Is(data, size, at, "kf") || at + 3 > size) {
		result.problem = "the mercenary section does not end where the golem section starts (byte " + std::to_string(at) + ")";
		return result;
	}
	const bool golem = data[at + 2] != 0;
	at += 3;
	if (golem && !ReadList(data, size, at, 1, tables, "golem", result.items, result.problem)) {
		return result;
	}
	if (at == size) {
		result.ok = true;
		return result;
	}
	// save version 105 ends with two bytes, "lf" and a count that was 0 in every file seen
	if (at + 6 == size && Is(data, size, at + 2, "lf") && Word(data, at + 4) == 0) {
		result.ok = true;
		return result;
	}
	if (at + 6 == size && Is(data, size, at + 2, "lf")) {
		result.problem = "its closing \"lf\" section has the count " + std::to_string(Word(data, at + 4)) + "; only 0 is known (what follows it is not known, and could hold items)";
		return result;
	}
	result.problem = "after the golem section comes something the reader does not know (byte " + std::to_string(at) + " of " + std::to_string(size) + ")";
	return result;
}

FileResult ReadStash(const uint8_t* data, size_t size, const Tables& tables) {
	FileResult result;
	size_t     at  = 0;
	int        tab = 0;
	while (at < size) {
		if (at + 68 > size || Dword(data, at) != 0xaa55aa55u) {
			result.problem = "no section where one must start (byte " + std::to_string(at) + ")";
			return result;
		}
		const uint32_t length = Dword(data, at + 16), type = Dword(data, at + 20);
		if (length < 64 || at + length > size) {
			result.problem = "a section's size is wrong (byte " + std::to_string(at) + ")";
			return result;
		}
		if (type == 0 || type == 1) {
			size_t            p       = at + 64;
			const std::string section = type == 0 ? "stash tab " + std::to_string(++tab) : std::string("stash materials");
			if (!ReadMarked(data, at + length, p, tables, section, result.items, result.problem)) {
				return result;
			}
			if (p != at + length) {
				result.problem = section + ": its items end at byte " + std::to_string(p) + ", the section at " + std::to_string(at + length);
				return result;
			}
		} else if (type != 2) { // 2: the record of found items (no items of its own that the reader knows of)
			result.problem = "a section of a kind the reader does not know (" + std::to_string(type) + ")";
			return result;
		}
		at += length;
	}
	result.ok = true;
	return result;
}

std::string Where(const Item& item) {
	std::string where;
	if (item.inSocket || item.location == 6) {
		where = "in a socket";
	} else if (item.location == 1) {
		where = "worn";
	} else if (item.location == 2) {
		where = "belt";
	} else if (item.location == 4) {
		where = "on the cursor";
	} else if (item.section.rfind("stash", 0) == 0) {
		where = "";
	} else {
		where = item.page == 1 ? "inventory" : item.page == 4 ? "cube" : item.page == 5 ? "stash" : "stored (page " + std::to_string(item.page) + ")";
	}
	if (item.section == "items") {
		return where;
	}
	return where.empty() ? item.section : item.section + ", " + where;
}

} // namespace exalted::saves
