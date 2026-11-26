// Exalted Items: checking the saves against a set of tables, read only. See save_check.h.
#include "save_check.h"

#include "save_reader.h"

#include <cstring>

namespace exalted {

namespace {

saves::Tables Load(const SaveTables& text) {
	saves::Tables tables;
	saves::ShapesFromText(tables, text.stats);
	saves::ReadItemTable(tables, text.armor, saves::kArmor);
	saves::ReadItemTable(tables, text.weapons, saves::kWeapon);
	saves::ReadItemTable(tables, text.misc, saves::kMisc);
	return tables;
}

bool EndsWith(const std::string& text, const char* end) {
	const size_t n = strlen(end);
	if (text.size() < n) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		const char a = text[text.size() - n + i], b = end[i];
		if ((a >= 'A' && a <= 'Z' ? a + 32 : a) != b) {
			return false;
		}
	}
	return true;
}

saves::FileResult Read(const SaveFile& file, const saves::Tables& tables) {
	const auto* data = reinterpret_cast<const uint8_t*>(file.bytes.data());
	return EndsWith(file.name, ".d2i") ? saves::ReadStash(data, file.bytes.size(), tables) : saves::ReadCharacter(data, file.bytes.size(), tables);
}

bool IsSave(const SaveFile& file) {
	return EndsWith(file.name, ".d2s") || EndsWith(file.name, ".d2i");
}

}

SaveCheck CheckSaves(const std::vector<SaveFile>& files, const SaveTables& built, const SaveTables& plain, const std::set<std::string>& exalted) {
	SaveCheck           check;
	const saves::Tables withBuilt = Load(built), withPlain = Load(plain);
	for (const SaveFile& file : files) {
		if (!IsSave(file)) {
			continue;
		}
		++check.files;
		const saves::FileResult result = Read(file, withBuilt);
		if (!result.ok) {
			const saves::FileResult alone = Read(file, withPlain);
			if (!alone.ok && alone.problem == result.problem) {
				check.skipped.push_back(file.name + ": " + result.problem);
			} else {
				check.unreadable.push_back(file.name + ": " + result.problem);
			}
			continue;
		}
		for (const saves::Item& item : result.items) {
			if (exalted.count(item.code) != 0) {
				check.holders.push_back(file.name);
				break;
			}
		}
	}
	return check;
}

std::vector<std::string> FilesWithUnknownItems(const std::vector<SaveFile>& files, const SaveTables& plain) {
	std::vector<std::string> out;
	const saves::Tables      tables = Load(plain);
	for (const SaveFile& file : files) {
		if (IsSave(file)) {
			const saves::FileResult result = Read(file, tables);
			if (!result.ok && result.problem.find("its code is in none of the item tables") != std::string::npos) {
				out.push_back(file.name);
			}
		}
	}
	return out;
}

std::string SaveLabel(const std::string& file) {
	const size_t      dot  = file.find_last_of('.');
	const std::string stem = file.substr(0, dot);
	return EndsWith(file, ".d2i") ? "the shared stash (" + stem + ")" : stem;
}

}
