// Exalted Items: checking the saves against a set of tables, read only.
//
// Used when codes.txt or stat-ids.txt is missing and has no previous copy: the tables a start
// builds are only taken when every save reads with them to its end, the way the game will read
// it. A save that holds an item those tables describe another way (another code, another stat
// layout) stops the reader, and then the tables are not used. Nothing here writes.
//
// Portable (no Windows or game calls), so the tests run it natively on copies of real saves.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace exalted {

struct SaveFile {
	std::string name;  // file name, e.g. "Hellpal.d2s"
	std::string bytes;
};

// The text of the tables a save is read with, as the game would load them (global/excel).
struct SaveTables {
	std::string armor, weapons, misc, stats;
};

struct SaveCheck {
	int files = 0; // character and stash files looked at
	// Files the reader cannot read with either set of tables for the same reason: a limit of the
	// reader (another save version, something it does not know), not Exalted's. Not counted.
	std::vector<std::string> skipped;
	// Files that do not read with the tables checked, but whose reading stops elsewhere with the
	// mod's tables alone: they hold something these tables describe another way. "<file>: why".
	std::vector<std::string> unreadable;
	// Files that hold exalted items or materials (an item code of `exalted`), read with the tables checked.
	std::vector<std::string> holders;
	bool Ok() const { return unreadable.empty(); }
};

// `built`: the tables to check. `plain`: the mod's own tables, without anything Exalted adds.
SaveCheck CheckSaves(const std::vector<SaveFile>& files, const SaveTables& built, const SaveTables& plain, const std::set<std::string>& exalted);

// The files that hold an item whose code is in none of the mod's own tables (read with `plain`
// only): the characters that hold exalted items, or items of a mod that is gone.
std::vector<std::string> FilesWithUnknownItems(const std::vector<SaveFile>& files, const SaveTables& plain);

// "Hellpal" for Hellpal.d2s, "the shared stash (SharedStashSoftCoreV2)" for a .d2i.
std::string SaveLabel(const std::string& file);

}
