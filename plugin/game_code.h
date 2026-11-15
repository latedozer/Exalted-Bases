// Exalted Items: finding the game's unique and set pickers in the game image.
//
// The plugin hooks two routines of the game: the one that picks a unique row for a new item
// and the one that picks a set row. Their addresses differ from one game build to the next,
// so they are found by the shape of their first instructions, with every address, stack slot
// and structure offset left open. The helper routines and structure offsets the hook needs
// are then read out of the matched code instead of being written down here:
//
//     item = arg 2 ... call <class id of unit>
//     movzx ecx, byte [game + VERSION]        <- offset of the data version in the game
//     call <ITEM ROW>(version, class id)      <- the game's item row lookup
//     mov eax, [row + CODE]                   <- offset of the item code in an item row
//     call <ITEM LEVEL>(item)                 <- the game's item level lookup (set picker)
//
// Portable C++ (no Windows calls), so the search can be run against a dumped image.
//
// Searching the whole image takes a while, so a caller that already knows where the pickers
// are in one particular game image can hand those addresses in. A hinted address is put
// through the same test as a search result (the whole pattern, and what the routine does
// further in); when it passes, that picker is not searched for.
#pragma once

#define EXALTED_GAME_CODE_HINT 1

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace exalted {

struct CodeRange {
	const uint8_t* begin;
	const uint8_t* end;
};

enum : int { PickUnique = 0, PickSet = 1 };

struct GameCode {
	uint64_t    pickRva[2]       = { 0, 0 }; // 0 = not found
	uint32_t    pickEntrySize[2] = { 0, 0 }; // whole instructions at the start of the routine
	uint64_t    itemRowRva       = 0;        // (version, row number) -> item row
	uint64_t    itemLevelRva     = 0;        // (item) -> item level; 0 = not found
	uint32_t    versionOffset    = 0;        // game -> data version (one byte)
	uint32_t    codeOffset       = 0;        // item row -> item code
	bool        searched         = false;    // the image was gone through (false: hints were enough)
	std::string notes;                       // what was and was not found, for the log
};

namespace detail {

struct Pattern {
	std::vector<uint8_t> bytes, mask;
};

// Two hex digits per byte; "??" is a byte that may differ between game builds.
inline Pattern ParsePattern(const char* text) {
	Pattern pattern;
	auto    digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
	for (const char* p = text; *p != '\0';) {
		if (*p == ' ') {
			++p;
		} else if (p[0] == '?' && p[1] == '?') {
			pattern.bytes.push_back(0);
			pattern.mask.push_back(0);
			p += 2;
		} else if (digit(p[0]) >= 0 && digit(p[1]) >= 0) {
			pattern.bytes.push_back(static_cast<uint8_t>(digit(p[0]) * 16 + digit(p[1])));
			pattern.mask.push_back(1);
			p += 2;
		} else {
			break;
		}
	}
	return pattern;
}

inline void FindAll(const CodeRange& range, const Pattern& pattern, std::vector<const uint8_t*>& hits, size_t limit) {
	const size_t size = pattern.bytes.size();
	if (size < 4 || range.end < range.begin || static_cast<size_t>(range.end - range.begin) < size) {
		return;
	}
	// Three fixed bytes in a row to look for first.
	size_t anchor = 0;
	while (anchor + 3 <= size && !(pattern.mask[anchor] != 0 && pattern.mask[anchor + 1] != 0 && pattern.mask[anchor + 2] != 0)) {
		++anchor;
	}
	if (anchor + 3 > size) {
		return;
	}
	const uint8_t  a = pattern.bytes[anchor], b = pattern.bytes[anchor + 1], c = pattern.bytes[anchor + 2];
	const uint8_t* const last = range.end - size;
	for (const uint8_t* p = range.begin; p <= last && hits.size() < limit; ++p) {
		if (p[anchor] != a || p[anchor + 1] != b || p[anchor + 2] != c) {
			continue;
		}
		bool equal = true;
		for (size_t i = 0; i < size && equal; ++i) {
			equal = pattern.mask[i] == 0 || p[i] == pattern.bytes[i];
		}
		if (equal) {
			hits.push_back(p);
		}
	}
}

// The same pattern with its first bytes left open: a routine that something else has already
// hooked has a jump where its first instructions were.
inline Pattern WithoutEntry(Pattern pattern, size_t entry) {
	for (size_t i = 0; i < entry && i < pattern.mask.size(); ++i) {
		pattern.mask[i] = 0;
	}
	return pattern;
}

inline uint32_t ReadU32(const uint8_t* p) {
	uint32_t value;
	memcpy(&value, p, sizeof(value));
	return value;
}

// Where a "call rel32" at `call` goes, as an offset from the start of the image.
inline uint64_t CallTarget(const uint8_t* image, const uint8_t* call) {
	int32_t relative;
	memcpy(&relative, call + 1, sizeof(relative));
	return static_cast<uint64_t>((call + 5 - image) + relative);
}

// Whether `bytes` occurs within `reach` bytes after `from`, without leaving the range.
inline bool Follows(const CodeRange& range, const uint8_t* from, size_t reach, const uint8_t* bytes, size_t size) {
	const uint8_t* end = from + reach < range.end ? from + reach : range.end;
	for (const uint8_t* p = from; p + size <= end; ++p) {
		if (memcmp(p, bytes, size) == 0) {
			return true;
		}
	}
	return false;
}

// Whether the pattern is at `p`, without leaving the range.
inline bool MatchAt(const CodeRange& range, const uint8_t* p, const Pattern& pattern) {
	const size_t size = pattern.bytes.size();
	if (p < range.begin || p > range.end || static_cast<size_t>(range.end - p) < size) {
		return false;
	}
	for (size_t i = 0; i < size; ++i) {
		if (pattern.mask[i] != 0 && p[i] != pattern.bytes[i]) {
			return false;
		}
	}
	return true;
}

// Set picker (game, item, drop): prologue, class id of the item, item row, item version,
// item code, item level. Offsets below count from the start of the routine.
inline const Pattern& SetPattern() {
	static const Pattern pattern = ParsePattern(
		"48 89 5c 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 81 ec ?? ?? 00 00" // 0: pushes, sub rsp
		" 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 84 24 ?? ?? 00 00"               // 23: stack cookie
		" 48 8b fa 48 89 54 24 ?? 4d 8b f0 48 8d 15 ?? ?? ?? ?? 4c 8b e9"      // 41: rdi = item, r14 = drop, r13 = game
		" 33 c0 48 8b cf 89 44 24 ?? 41 b8 ?? ?? 00 00 33 ed e8 ?? ?? ?? ??"   // 62: class id of the item
		" 41 0f b6 8d ?? ?? 00 00 8b d0 e8 ?? ?? ?? ??"                        // 84: version at 88, item row call at 94
		" 48 8b d8 48 85 c0 0f 84 ?? ?? ?? ?? 48 8b cf e8 ?? ?? ?? ??"         // 99
		" 0f b7 f0 66 89 44 24 ?? 8b 83 ?? ?? 00 00"                           // 119: code offset at 129
		" 48 8b cf 89 44 24 ?? e8 ?? ?? ?? ??");                               // 133: item level call at 140
	return pattern;
}

// Unique picker (game, item, drop): prologue, class id of the item, item row.
inline const Pattern& UniquePattern() {
	static const Pattern pattern = ParsePattern(
		"48 89 5c 24 ?? 4c 89 44 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 ec ??" // 0: pushes, sub rsp
		" 4c 8b f2 4d 8b e0 48 8b e9 48 8d 15 ?? ?? ?? ?? 49 8b ce"                  // 25: r14 = item, r12 = drop, rbp = game
		" 41 b8 ?? ?? 00 00 e8 ?? ?? ?? ??"                                          // 44: class id of the item
		" 0f b6 8d ?? ?? 00 00 8b d0 e8 ?? ?? ?? ??"                                 // 55: version at 58, item row call at 64
		" 4c 8b e8 48 85 c0 75 ??");                                                 // 69
	return pattern;
}

// What each routine does further in, to tell it from a routine that merely starts alike: the
// set picker rolls with the game's random number step, the unique picker checks the "already
// dropped in this game" list, which holds 4096 uniques.
inline bool IsPicker(const CodeRange& range, const uint8_t* p, bool isSet) {
	static const uint8_t randomStep[] = { 0x69, 0xc0, 0xc5, 0x90, 0xc6, 0x6a }; // imul r8, rax, 0x6ac690c5 (after its prefix)
	static const uint8_t listSize[]   = { 0x00, 0x10, 0x00, 0x00 };
	if (!MatchAt(range, p, isSet ? SetPattern() : UniquePattern())) {
		return false;
	}
	return isSet ? Follows(range, p, 0x400, randomStep, sizeof(randomStep)) : Follows(range, p, 0x800, listSize, sizeof(listSize));
}

// Every place in the range that is one of the two pickers, in one pass. Both patterns begin
// with the same four fixed bytes, so each position costs one comparison of four bytes; only
// the few places that pass it are looked at more closely. `want` says which of the two are
// looked for. At most `limit` places that match a pattern are considered for each.
inline void FindPickers(const CodeRange& range, const bool want[2], std::vector<const uint8_t*> hits[2], size_t limit) {
	const Pattern* const patterns[2] = { &UniquePattern(), &SetPattern() };
	const size_t         shortest    = patterns[PickUnique]->bytes.size() < patterns[PickSet]->bytes.size() ? patterns[PickUnique]->bytes.size() : patterns[PickSet]->bytes.size();
	bool                 sameLead    = shortest >= 4;
	for (size_t i = 0; i < 4 && sameLead; ++i) {
		sameLead = patterns[0]->mask[i] != 0 && patterns[1]->mask[i] != 0 && patterns[0]->bytes[i] == patterns[1]->bytes[i];
	}
	if (!sameLead) { // patterns that do not start alike: one pass each
		for (int kind = 0; kind < 2; ++kind) {
			std::vector<const uint8_t*> raw;
			if (want[kind]) {
				FindAll(range, *patterns[kind], raw, limit);
			}
			for (const uint8_t* hit : raw) {
				if (IsPicker(range, hit, kind == PickSet)) {
					hits[kind].push_back(hit);
				}
			}
		}
		return;
	}
	if (range.end < range.begin || static_cast<size_t>(range.end - range.begin) < shortest) {
		return;
	}
	const uint32_t       lead = ReadU32(patterns[0]->bytes.data());
	const uint8_t* const last = range.end - shortest;
	size_t               raw[2] = { 0, 0 };
	for (const uint8_t* p = range.begin; p <= last; ++p) {
		if (ReadU32(p) != lead) {
			continue;
		}
		for (int kind = 0; kind < 2; ++kind) {
			if (!want[kind] || raw[kind] >= limit || !MatchAt(range, p, *patterns[kind])) {
				continue;
			}
			++raw[kind];
			if (IsPicker(range, p, kind == PickSet)) {
				hits[kind].push_back(p);
			}
		}
	}
}

}

// `image` points at the start of the game image (offset 0); the ranges are the parts of it
// that hold code. `hint` (optional) holds, for [PickUnique] and [PickSet], an address at which
// that picker is known to be in this very image, as an offset from the start of the image;
// 0 = not known.
inline GameCode FindGameCode(const uint8_t* image, const std::vector<CodeRange>& ranges, const uint64_t* hint = nullptr) {
	using namespace detail;
	GameCode found;

	std::vector<const uint8_t*> hits[2]; // [PickUnique], [PickSet]
	bool                        want[2] = { true, true };
	for (int kind = 0; kind < 2 && hint != nullptr; ++kind) {
		if (hint[kind] == 0) {
			continue;
		}
		for (const CodeRange& range : ranges) {
			const uint64_t begin = static_cast<uint64_t>(range.begin - image), end = static_cast<uint64_t>(range.end - image);
			if (hint[kind] >= begin && hint[kind] < end && IsPicker(range, image + hint[kind], kind == PickSet)) {
				hits[kind].push_back(image + hint[kind]);
				want[kind] = false;
				break;
			}
		}
	}
	if (want[0] || want[1]) {
		found.searched = true;
		for (const CodeRange& range : ranges) {
			FindPickers(range, want, hits, 16);
		}
	}
	const std::vector<const uint8_t*>& sets    = hits[PickSet];
	const std::vector<const uint8_t*>& uniques = hits[PickUnique];

	char     text[256];
	uint64_t rowFromSet = 0, rowFromUnique = 0;
	uint32_t versionFromSet = 0, versionFromUnique = 0;
	if (sets.size() == 1) {
		const uint8_t* at            = sets[0];
		found.pickRva[PickSet]       = static_cast<uint64_t>(at - image);
		found.pickEntrySize[PickSet] = 23;
		versionFromSet               = ReadU32(at + 88);
		rowFromSet                   = CallTarget(image, at + 94);
		found.codeOffset             = ReadU32(at + 129);
		found.itemLevelRva           = CallTarget(image, at + 140);
		snprintf(text, sizeof(text), "set picker at 0x%llx", static_cast<unsigned long long>(found.pickRva[PickSet]));
	} else {
		snprintf(text, sizeof(text), "set picker not found (%d places look like it)", static_cast<int>(sets.size()));
	}
	found.notes = text;
	if (uniques.size() == 1) {
		const uint8_t* at               = uniques[0];
		found.pickRva[PickUnique]       = static_cast<uint64_t>(at - image);
		found.pickEntrySize[PickUnique] = 25;
		versionFromUnique               = ReadU32(at + 58);
		rowFromUnique                   = CallTarget(image, at + 64);
		snprintf(text, sizeof(text), "; unique picker at 0x%llx", static_cast<unsigned long long>(found.pickRva[PickUnique]));
	} else {
		snprintf(text, sizeof(text), "; unique picker not found (%d places look like it)", static_cast<int>(uniques.size()));
	}
	found.notes += text;

	// A picker that is there but no longer starts with its own instructions has been hooked by
	// something else. It is left alone (two hooks on one routine have to be arranged by the
	// loader), but the log should say that this is what happened.
	for (const bool isSet : { true, false }) {
		if (found.pickRva[isSet ? PickSet : PickUnique] != 0) {
			continue;
		}
		const Pattern               open = WithoutEntry(isSet ? SetPattern() : UniquePattern(), 16);
		std::vector<const uint8_t*> places;
		for (const CodeRange& range : ranges) {
			FindAll(range, open, places, 4);
		}
		if (places.size() == 1) {
			snprintf(text, sizeof(text), "; the %s picker seems to be at 0x%llx but its first bytes have been replaced (already hooked by the loader or another plugin?)", isSet ? "set" : "unique",
			         static_cast<unsigned long long>(places[0] - image));
			found.notes += text;
		}
	}

	// Both routines look up the item row the same way. When both were found they have to agree;
	// if they do not, neither is trusted.
	if (rowFromSet != 0 && rowFromUnique != 0 && (rowFromSet != rowFromUnique || versionFromSet != versionFromUnique)) {
		found.pickRva[PickSet] = found.pickRva[PickUnique] = 0;
		found.itemLevelRva                                 = 0;
		found.notes += "; the two pickers disagree about the item row lookup, so neither is used";
		return found;
	}
	found.itemRowRva    = rowFromSet != 0 ? rowFromSet : rowFromUnique;
	found.versionOffset = rowFromSet != 0 ? versionFromSet : versionFromUnique;
	if (found.codeOffset == 0 && found.itemRowRva != 0) {
		found.codeOffset = 0x80; // only the set picker shows it; this is where it has been in every build seen
		found.notes += "; item code offset assumed";
	}
	if (found.itemRowRva != 0) {
		snprintf(text, sizeof(text), "; item row lookup at 0x%llx, item level lookup at 0x%llx, version at game+0x%x, code at row+0x%x", static_cast<unsigned long long>(found.itemRowRva),
		         static_cast<unsigned long long>(found.itemLevelRva), found.versionOffset, found.codeOffset);
		found.notes += text;
	}
	// Offsets that make no sense mean the match is not what it seems.
	if (found.itemRowRva != 0 && (found.versionOffset == 0 || found.versionOffset > 0x10000 || found.codeOffset == 0 || found.codeOffset > 0x1000)) {
		found.pickRva[PickSet] = found.pickRva[PickUnique] = 0;
		found.notes += "; the offsets read from the code are out of range, so the pickers are not used";
	}
	return found;
}

}
