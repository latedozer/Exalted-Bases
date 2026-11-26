// Lists every saved item stat from a number up, read with a given itemstatcost.txt (and the item
// tables of another folder). For check_migrate.sh.
//   scan_saves <saves dir> <item tables dir> <itemstatcost.txt> <from>
#include "../save_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

using namespace exalted::saves;
namespace fs = std::filesystem;

static std::string Slurp(const fs::path& p) {
	std::ifstream     f(p, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

int main(int argc, char** argv) {
	if (argc < 5) return 2;
	Tables t;
	ShapesFromText(t, Slurp(argv[3]));
	ReadItemTable(t, Slurp(fs::path(argv[2]) / "armor.txt"), kArmor);
	ReadItemTable(t, Slurp(fs::path(argv[2]) / "weapons.txt"), kWeapon);
	ReadItemTable(t, Slurp(fs::path(argv[2]) / "misc.txt"), kMisc);
	const int              from = atoi(argv[4]);
	std::map<int, int>     count;
	std::vector<fs::path>  files;
	for (auto& e : fs::directory_iterator(argv[1])) {
		if (e.path().extension() == ".d2s" || e.path().extension() == ".d2i") files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	for (auto& path : files) {
		const std::string b = Slurp(path);
		const FileResult  r = path.extension() == ".d2s" ? ReadCharacter(reinterpret_cast<const uint8_t*>(b.data()), b.size(), t) : ReadStash(reinterpret_cast<const uint8_t*>(b.data()), b.size(), t);
		printf("%s: %s %zu items %s\n", path.filename().c_str(), r.ok ? "ok" : "UNREADABLE", r.items.size(), r.problem.c_str());
		for (auto& it : r.items) {
			for (auto& s : it.stats) {
				if (s.stat >= from) {
					++count[s.stat];
					printf("   %s %s q%d lvl%d %s [%s] at %u: stat %d = %d\n", path.filename().c_str(), it.code.c_str(), it.quality, it.level, Where(it).c_str(), it.section.c_str(), it.offset, s.stat, s.value);
				}
			}
		}
	}
	for (auto& [k, x] : count) printf("stat %d on %d items\n", k, x);
}
