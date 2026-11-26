// Exalted Items: the one-time stat migration (1.2.0 layout -> packed numbers).
//
// Up to 1.2.0 the plugin's stats had fixed numbers: the marker at 500, the roll stats at 499 down
// to 487, and empty "xlt_unused_<n>" rows from the mod's last stat up to them. This tool moves
// every saved item's marker and roll stats to the packed numbers right after the mod's own stats,
// so the table needs no filler rows:
//
//   marker = first, roll k = first + 1 + k      where first = the mod's own stats (the larger of both banks)
//
// Only Exalted's own stats. Stats other plugins made are theirs to move: a save
// holding any other number past the mod's stats is not touched, and the tool stops.
//
// It runs offline with the game closed, and only when started by hand. Without --apply it only
// works everything out and checks it (nothing is written). With --apply:
//   1. backs up every save, stat-ids.txt and codes.txt into a new dated folder;
//   2. writes the changed saves, reads them back from disk and compares every item with the
//      original (only the moved stat numbers may differ); anything else puts the backup back;
//   3. writes the new stat-ids.txt last.
//
// A stat is moved in place: its 9-bit number is rewritten, its value bits stay where they are
// (the new row has the same shape). Where the number sits in the item is found by trying every
// place that holds the old number within the item and keeping the one where the file then reads
// back as expected; exactly one place must fit, or the tool stops.
//
// Needs the stat-ids.txt that 1.3.0 writes at its first start (so the plugin installed is one that
// reads the map). Going back to 1.2.0 after the migration would make the moved items unreadable.
//
//   exalted_migrate --saves <dir> --tables <excel dir of Exalted's stored copies>
//                   --stat-ids <stat-ids.txt> --codes <codes.txt>
//                   [--out <dir: dry run writes the would-be files here>] [--apply]
#include "../exalted_core.h"
#include "../save_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using exalted::saves::FileResult;
using exalted::saves::Item;
using exalted::saves::Tables;

namespace {

bool Slurp(const fs::path& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::stringstream text;
	text << in.rdbuf();
	out = text.str();
	return true;
}

bool WriteAtomic(const fs::path& path, const std::string& data) {
	const fs::path temp = path.string() + ".migrate-tmp";
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		if (!out || !out.write(data.data(), static_cast<std::streamsize>(data.size())) || !out.flush()) {
			return false;
		}
	}
	std::error_code error;
	fs::rename(temp, path, error);
	if (error) {
		fs::remove(temp, error);
		return false;
	}
	std::string back;
	return Slurp(path, back) && back == data;
}

bool GameRunning() {
	for (const auto& entry : fs::directory_iterator("/proc")) {
		const std::string name = entry.path().filename().string();
		if (name.find_first_not_of("0123456789") != std::string::npos) {
			continue;
		}
		std::string cmd;
		if (!Slurp(entry.path() / "cmdline", cmd)) {
			continue;
		}
		std::replace(cmd.begin(), cmd.end(), '\0', ' ');
		std::string lower = cmd;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
		if (lower.find("d2r.exe") != std::string::npos || lower.find("d2rloader.exe") != std::string::npos) {
			return true;
		}
	}
	return false;
}

std::vector<std::vector<std::string>> Rows(const std::string& text) {
	std::vector<std::vector<std::string>> rows;
	std::stringstream in(text);
	std::string       line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		std::vector<std::string> cells;
		std::stringstream        cut(line);
		std::string              cell;
		while (std::getline(cut, cell, '\t')) {
			cells.push_back(cell);
		}
		if (!line.empty() && line.back() == '\t') {
			cells.emplace_back();
		}
		rows.push_back(cells);
	}
	return rows;
}

// The mod's own stats in one of Exalted's stored itemstatcost.txt copies: the rows before the
// first row the plugin added.
int ModStats(const std::string& text) {
	const auto rows = Rows(text);
	int        n    = 0;
	for (size_t i = 1; i < rows.size(); ++i) {
		if (rows[i].empty() || rows[i][0].empty()) {
			continue;
		}
		const std::string& name = rows[i][0];
		if (name.rfind("xlt_", 0) == 0) {
			break;
		}
		++n;
	}
	return n;
}

uint32_t Checksum(const std::string& bytes) {
	uint32_t sum = 0;
	for (size_t i = 0; i < bytes.size(); ++i) {
		const uint8_t byte = i >= 12 && i < 16 ? 0 : static_cast<uint8_t>(bytes[i]);
		sum                = ((sum << 1) | (sum >> 31)) + byte;
	}
	return sum;
}

uint32_t Dword(const std::string& b, size_t at) {
	uint32_t v;
	memcpy(&v, b.data() + at, 4);
	return v;
}

FileResult Read(const fs::path& path, const std::string& bytes, const Tables& tables) {
	const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
	return path.extension() == ".d2s" ? exalted::saves::ReadCharacter(data, bytes.size(), tables) : exalted::saves::ReadStash(data, bytes.size(), tables);
}

bool SameItem(const Item& a, const Item& b) {
	if (a.code != b.code || a.flags != b.flags || a.location != b.location || a.equipped != b.equipped || a.x != b.x || a.y != b.y || a.page != b.page || a.quality != b.quality
	    || a.level != b.level || a.inSocket != b.inSocket || a.quantity != b.quantity || a.offset != b.offset || a.section != b.section || a.stats.size() != b.stats.size()) {
		return false;
	}
	for (size_t i = 0; i < a.stats.size(); ++i) {
		if (a.stats[i].stat != b.stats[i].stat || a.stats[i].value != b.stats[i].value || a.stats[i].param != b.stats[i].param) {
			return false;
		}
	}
	return true;
}

bool SameItems(const std::vector<Item>& a, const std::vector<Item>& b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (!SameItem(a[i], b[i])) {
			return false;
		}
	}
	return true;
}

void PutBits(std::string& bytes, size_t bit, unsigned count, uint32_t value) {
	for (unsigned i = 0; i < count; ++i, ++bit) {
		uint8_t& byte = reinterpret_cast<uint8_t&>(bytes[bit >> 3]);
		byte          = static_cast<uint8_t>((byte & ~(1u << (bit & 7))) | (((value >> i) & 1u) << (bit & 7)));
	}
}

uint32_t GetBits(const std::string& bytes, size_t bit, unsigned count) {
	uint32_t value = 0;
	for (unsigned i = 0; i < count; ++i, ++bit) {
		value |= static_cast<uint32_t>((static_cast<uint8_t>(bytes[bit >> 3]) >> (bit & 7)) & 1u) << i;
	}
	return value;
}

struct Plan {
	exalted::StatIds          before, after;
	std::map<int, int>        moves;        // old number -> new number
	int                       first = 0;
	Tables                    oldTables, unionTables, newTables;
};

// One save file moved to the new numbers in memory. False (and why) when it cannot be done.
bool MoveFile(const fs::path& path, const std::string& original, const Plan& plan, std::string& moved, int& count, std::string& why) {
	const FileResult start = Read(path, original, plan.oldTables);
	if (!start.ok) {
		why = "cannot be read with the current tables: " + start.problem;
		return false;
	}
	const bool character = path.extension() == ".d2s";
	if (character && Checksum(original) != Dword(original, 12)) {
		why = "its checksum does not match the known way of working it out; not touched";
		return false;
	}
	for (const Item& item : start.items) {
		for (const auto& stat : item.stats) {
			if (stat.stat >= plan.first && plan.moves.count(stat.stat) == 0 && stat.stat <= exalted::kMaxStatId) {
				why = "an item (" + item.code + ") holds stat " + std::to_string(stat.stat) + ", which is not one of Exalted Items' stats (another plugin's?): this tool moves only Exalted's, and that one would no longer read. Move that plugin's stats first";
				return false;
			}
		}
	}
	moved = original;
	count = 0;
	std::vector<Item> expected = start.items;
	std::vector<size_t> order(start.items.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return start.items[a].offset < start.items[b].offset; });
	for (size_t o = 0; o < order.size(); ++o) {
		const size_t idx   = order[o];
		const size_t from  = static_cast<size_t>(start.items[idx].offset) * 8;
		const size_t until = (o + 1 < order.size() ? static_cast<size_t>(start.items[order[o + 1]].offset) : original.size()) * 8;
		for (size_t s = 0; s < expected[idx].stats.size(); ++s) {
			const auto move = plan.moves.find(expected[idx].stats[s].stat);
			if (move == plan.moves.end() || expected[idx].stats[s].stat == move->second) {
				continue;
			}
			std::vector<Item> want = expected;
			want[idx].stats[s].stat = static_cast<uint16_t>(move->second);
			int         fits = 0;
			std::string chosen;
			for (size_t bit = from; bit + 9 <= until; ++bit) {
				if (GetBits(moved, bit, 9) != static_cast<uint32_t>(move->first)) {
					continue;
				}
				std::string trial = moved;
				PutBits(trial, bit, 9, static_cast<uint32_t>(move->second));
				const FileResult result = Read(path, trial, plan.unionTables);
				if (result.ok && SameItems(result.items, want)) {
					++fits;
					chosen = trial;
				}
			}
			if (fits != 1) {
				why = "stat " + std::to_string(move->first) + " of item " + expected[idx].code + " (byte " + std::to_string(start.items[idx].offset) + "): " + std::to_string(fits)
				      + " places in the item fit (exactly one must)";
				return false;
			}
			moved    = chosen;
			expected = want;
			++count;
		}
	}
	if (character) {
		const uint32_t sum = Checksum(moved);
		memcpy(moved.data() + 12, &sum, 4);
	}
	// The end result, read with the tables the game will have: every item as before, apart from
	// the moved numbers.
	const FileResult end = Read(path, moved, plan.newTables);
	if (!end.ok || !SameItems(end.items, expected)) {
		why = "the moved file does not read back as expected" + (end.ok ? std::string() : ": " + end.problem);
		return false;
	}
	if (moved.size() != original.size()) {
		why = "internal error: the size changed";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	std::map<std::string, std::string> arg;
	bool                               apply = false;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--apply") {
			apply = true;
		} else if (a.rfind("--", 0) == 0 && i + 1 < argc) {
			arg[a.substr(2)] = argv[++i];
		}
	}
	for (const char* need : { "saves", "tables", "stat-ids", "codes" }) {
		if (arg.count(need) == 0) {
			printf("usage: exalted_migrate --saves <dir> --tables <excel dir> --stat-ids <file> --codes <file> [--out <dir>] [--apply]\n");
			return 2;
		}
	}
	if (getenv("EXALTED_MIGRATE_NO_GAME_CHECK") == nullptr && GameRunning()) {
		printf("The game is running. Close it first: a save the game has open cannot be rewritten safely.\n");
		return 3;
	}
	const fs::path saves = arg["saves"], tables = arg["tables"], statIdsPath = arg["stat-ids"], codesPath = arg["codes"];

	// 1. The plan.
	Plan        plan;
	std::string text, why;
	// stat-ids.txt is written by 1.3.0 at its first start. Without it the installed plugin may be
	// 1.2.0 or older, which would read the moved saves with the old numbers: refuse.
	if (!Slurp(statIdsPath, text)) {
		printf("No stat-ids.txt at %s: start the game once with Exalted Items 1.3.0 (or newer) first, then close it and run this again.\n", statIdsPath.c_str());
		return 1;
	}
	if (!exalted::ParseStatIds(text, plan.before, why)) {
		printf("stat-ids.txt cannot be read: %s\n", why.c_str());
		return 1;
	}
	if (!plan.before.legacy) {
		printf("stat-ids.txt is already in the packed layout: nothing to migrate.\n");
		return 0;
	}
	std::string mainStats, baseStats;
	if (!Slurp(tables / "itemstatcost.txt", mainStats)) {
		printf("cannot read %s\n", (tables / "itemstatcost.txt").c_str());
		return 1;
	}
	plan.first = ModStats(mainStats);
	if (Slurp(tables / "base" / "itemstatcost.txt", baseStats)) {
		plan.first = std::max(plan.first, ModStats(baseStats));
	}
	plan.after = exalted::NewStatIds(plan.first);
	plan.moves[plan.before.marker] = plan.after.marker;
	for (int k = 0; k < exalted::kRollStatCount; ++k) {
		plan.moves[plan.before.roll[k]] = plan.after.roll[k];
	}
	if (plan.after.Highest() >= plan.before.Lowest()) {
		printf("the mod has %d stats of its own: the packed numbers would reach the old ones; nothing to gain\n", plan.first);
		return 1;
	}

	// The three stat tables: as the saves were written, both at once (for one move at a time),
	// and as the game will have them.
	exalted::saves::ShapesFromText(plan.oldTables, mainStats);
	for (const char* file : { "armor.txt", "weapons.txt", "misc.txt" }) {
		std::string items;
		if (!Slurp(tables / file, items)) {
			printf("cannot read %s\n", (tables / file).c_str());
			return 1;
		}
		exalted::saves::ReadItemTable(plan.oldTables, items, std::string(file) == "armor.txt" ? exalted::saves::kArmor : std::string(file) == "weapons.txt" ? exalted::saves::kWeapon : exalted::saves::kMisc);
	}
	plan.unionTables = plan.oldTables;
	plan.newTables   = plan.oldTables;
	plan.newTables.stats.assign(static_cast<size_t>(plan.after.Highest()) + 1, exalted::saves::StatShape {});
	for (int n = 0; n < plan.first; ++n) {
		plan.newTables.stats[static_cast<size_t>(n)] = plan.oldTables.stats[static_cast<size_t>(n)];
	}
	for (const auto& [from, to] : plan.moves) {
		if (static_cast<size_t>(from) >= plan.oldTables.stats.size()) {
			printf("stat %d is past the current table\n", from);
			return 1;
		}
		plan.unionTables.stats[static_cast<size_t>(to)] = plan.oldTables.stats[static_cast<size_t>(from)];
		plan.newTables.stats[static_cast<size_t>(to)]   = plan.oldTables.stats[static_cast<size_t>(from)];
	}
	printf("plan: the mod has %d stats; marker %d -> %d, rolls %d..%d -> %d..%d", plan.first, plan.before.marker, plan.after.marker, plan.before.roll[0], plan.before.roll[exalted::kRollStatCount - 1],
	       plan.after.roll[0], plan.after.roll[exalted::kRollStatCount - 1]);
	printf("\n");

	// 2. Every save, in memory.
	struct Done {
		fs::path    path;
		std::string original, moved;
		int         count = 0;
	};
	std::vector<Done> files;
	for (const auto& entry : fs::directory_iterator(saves)) {
		const auto ext = entry.path().extension();
		if (!entry.is_regular_file() || (ext != ".d2s" && ext != ".d2i")) {
			continue;
		}
		Done done { entry.path(), {}, {}, 0 };
		if (!Slurp(done.path, done.original)) {
			printf("cannot read %s\n", done.path.c_str());
			return 1;
		}
		if (!MoveFile(done.path, done.original, plan, done.moved, done.count, why)) {
			printf("%s: %s\nNothing was written.\n", done.path.filename().c_str(), why.c_str());
			return 1;
		}
		printf("  %-40s %s\n", done.path.filename().c_str(), done.count ? (std::to_string(done.count) + " stat numbers moved, read back: every item as before").c_str() : "nothing to move");
		files.push_back(std::move(done));
	}
	std::sort(files.begin(), files.end(), [](const Done& a, const Done& b) { return a.path < b.path; });
	const std::string newIds = exalted::FormatStatIds(plan.after);
	exalted::StatIds  check;
	if (!exalted::ParseStatIds(newIds, check, why) || !(check == plan.after)) {
		printf("internal error: the new stat-ids.txt does not read back\n");
		return 1;
	}

	if (!apply) {
		if (arg.count("out")) {
			const fs::path out = arg["out"];
			fs::create_directories(out);
			for (const Done& d : files) {
				if (d.count) {
					std::ofstream(out / d.path.filename(), std::ios::binary) << d.moved;
				}
			}
			std::ofstream(out / "stat-ids.txt", std::ios::binary) << newIds;
			printf("dry run: the would-be files are in %s\n", out.c_str());
		}
		printf("dry run: nothing was changed. Run again with --apply (game closed) to do it.\n");
		return 0;
	}

	// 3. Backup: a new dated folder, never one that is there already.
	char      stamp[32];
	const time_t now = time(nullptr);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	const fs::path backup = saves / ("exalted-migration-backup-" + std::string(stamp));
	std::error_code error;
	if (fs::exists(backup) || !fs::create_directory(backup, error) || error) {
		printf("cannot make the backup folder %s (it must not exist yet). Nothing was written.\n", backup.c_str());
		return 1;
	}
	std::vector<std::pair<fs::path, std::string>> kept; // what the backup holds: original place, bytes
	for (const Done& d : files) {
		kept.emplace_back(d.path, d.original);
	}
	for (const fs::path& p : { statIdsPath, codesPath }) {
		std::string bytes;
		if (!p.empty() && Slurp(p, bytes)) {
			kept.emplace_back(p, bytes);
		}
	}
	std::string list;
	for (size_t i = 0; i < kept.size(); ++i) {
		const fs::path copy = backup / (std::to_string(i) + "-" + kept[i].first.filename().string());
		std::ofstream(copy, std::ios::binary) << kept[i].second;
		std::string back;
		if (!Slurp(copy, back) || back != kept[i].second) {
			printf("the backup of %s could not be written. Nothing was changed.\n", kept[i].first.c_str());
			return 1;
		}
		list += copy.filename().string() + "\t" + kept[i].first.string() + "\n";
	}
	std::ofstream(backup / "where-each-file-came-from.txt", std::ios::binary) << list;
	printf("backup: %s (%zu files)\n", backup.c_str(), kept.size());

	const auto putBack = [&](const char* reason) {
		bool all = true;
		for (const auto& [path, bytes] : kept) {
			all = WriteAtomic(path, bytes) && all;
		}
		printf("%s - %s\n", reason, all ? "every file put back from the backup" : "PUTTING BACK FAILED: copy the files back by hand from the backup folder");
		return 1;
	};

	// 4. The saves, read back from disk.
	for (const Done& d : files) {
		if (!d.count) {
			continue;
		}
		std::string back;
		if (!WriteAtomic(d.path, d.moved) || !Slurp(d.path, back) || back != d.moved) {
			return putBack(("writing " + d.path.filename().string() + " failed").c_str());
		}
		const FileResult before = Read(d.path, d.original, plan.oldTables), after = Read(d.path, back, plan.newTables);
		bool same = before.ok && after.ok && before.items.size() == after.items.size();
		for (size_t i = 0; same && i < before.items.size(); ++i) {
			Item want = before.items[i];
			for (auto& s : want.stats) {
				const auto m = plan.moves.find(s.stat);
				s.stat       = m != plan.moves.end() ? static_cast<uint16_t>(m->second) : s.stat;
			}
			same = SameItem(want, after.items[i]);
		}
		if (!same || getenv("EXALTED_MIGRATE_TEST_FAIL") != nullptr) { // (the variable: tests of the putting back)
			return putBack((d.path.filename().string() + " did not read back as expected").c_str());
		}
		printf("  %s written and read back: every item as before, %d stat numbers moved\n", d.path.filename().c_str(), d.count);
	}
	// 5. stat-ids.txt (last).
	if (!WriteAtomic(statIdsPath, newIds)) {
		return putBack("writing stat-ids.txt failed");
	}
	printf("done: stat-ids.txt is in the packed layout. At the next game start Exalted Items writes its tables without filler rows.\n");
	return 0;
}
