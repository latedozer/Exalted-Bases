// Native test driver: runs the generator against a copy of a mod's data folder.
//   gen_test <mod data dir> <vanilla data dir> <output dir> [key=value ...]
// The mod folder is only read; adjusted files go to the output folder.
#include "../exalted_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

std::string Lower(std::string s) {
	for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return s;
}

// Resolves a path the way Windows would: without caring about letter case.
bool Resolve(const std::string& root, const std::string& rel, std::string& out) {
	std::string current = root;
	size_t      pos     = 0;
	while (pos < rel.size()) {
		size_t end = rel.find('/', pos);
		if (end == std::string::npos) end = rel.size();
		const std::string part = Lower(rel.substr(pos, end - pos));
		DIR*              dir  = opendir(current.c_str());
		if (dir == nullptr) return false;
		std::string match;
		while (dirent* e = readdir(dir)) {
			if (Lower(e->d_name) == part) { match = e->d_name; break; }
		}
		closedir(dir);
		if (match.empty()) return false;
		current += "/" + match;
		pos = end + 1;
	}
	out = current;
	return true;
}

bool ReadFile(const std::string& path, std::string& out) {
	FILE* f = fopen(path.c_str(), "rb");
	if (f == nullptr) return false;
	out.clear();
	char   buffer[65536];
	size_t n;
	while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
	fclose(f);
	return true;
}

struct TestIo : exalted::Io {
	std::string mod, base, out;
	bool ReadMod(const std::string& rel, std::string& out) override {
		std::string path;
		return Resolve(mod, rel, path) && ReadFile(path, out);
	}
	bool ReadBase(const std::string& rel, std::string& out) override {
		std::string path;
		return Resolve(base, rel, path) && ReadFile(path, out);
	}
	bool Write(const std::string& rel, const std::string& data) override {
		std::string path = out;
		size_t      pos  = 0;
		mkdir(path.c_str(), 0755);
		while (true) {
			const size_t end = rel.find('/', pos);
			if (end == std::string::npos) break;
			path += "/" + rel.substr(pos, end - pos);
			mkdir(path.c_str(), 0755);
			pos = end + 1;
		}
		path += "/" + rel.substr(pos);
		FILE* f = fopen(path.c_str(), "wb");
		if (f == nullptr) return false;
		fwrite(data.data(), 1, data.size(), f);
		fclose(f);
		printf("  wrote %s (%zu bytes)\n", rel.c_str(), data.size());
		return true;
	}
	void ListMod(const std::string& relDir, std::vector<std::string>& names) override {
		std::string path;
		if (!Resolve(mod, relDir, path)) return;
		DIR* dir = opendir(path.c_str());
		if (dir == nullptr) return;
		while (dirent* e = readdir(dir)) {
			if (e->d_name[0] != '.') names.push_back(e->d_name);
		}
		closedir(dir);
	}
	void Log(const char* text) override { printf("  log: %s\n", text); }
};

// Saved codes file: one "elite=exalted" pair per line.
void ReadCodes(const std::string& path, std::vector<std::pair<std::string, std::string>>& out) {
	std::string text;
	if (!ReadFile(path, text)) return;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(pos, end - pos);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		const size_t eq = line.find('=');
		if (eq != std::string::npos && !line.empty() && line[0] != '#') out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
		pos = end + 1;
	}
}

}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: gen_test <mod data dir> <vanilla data dir> <output dir> [enabled=0] [max=N] [uniques=mask] [sets=mask] [step=N] [zone=1]\n"
		                "                [codes=<file to read saved codes from>] [savecodes=<file>] [families=<file>]\n"
		                "       masks: 1 normal, 2 exceptional, 4 elite (7 = all, 0 = off)\n");
		return 2;
	}
	TestIo io;
	io.mod  = argv[1];
	io.base = argv[2];
	io.out  = argv[3];
	exalted::Config config;
	std::string     saveCodes, saveFamilies;
	int             pickTest = 0;
	// The tests written before the crystal run without it; "crystal=1" turns it on.
	config.crystalEnabled = false;
	config.recipes = { { "unique_weapon", "r26 r30 gpg" }, { "unique_armor", "r29 r27 gpw" }, { "set_weapon", "r26 r30 gpg" },
		               { "set_armor", "r29 r27 gpw" },     { "rare_weapon", "r29 r33 gpb" },  { "rare_armor", "r28 r32 gpv" } };
	for (int i = 4; i < argc; ++i) {
		const char* eq = strchr(argv[i], '=');
		if (eq == nullptr) continue;
		const std::string key(argv[i], eq - argv[i]);
		const int         value = atoi(eq + 1);
		if (key == "enabled") config.enabled = value != 0;
		if (key == "max") config.dropLevelMax = value;
		if (key == "step") config.statStepPercent = value;
		if (key == "uniques") config.uniqueTiers = value;
		if (key == "sets") config.setTiers = value;
		if (key == "codes") ReadCodes(eq + 1, config.savedCodes);
		if (key == "savecodes") saveCodes = eq + 1;
		if (key == "families") saveFamilies = eq + 1;
		if (key == "picktest") pickTest = value;
		if (key == "upgradereq") config.upgradeLevelReq = value;
		if (key == "zone") config.terrorZoneOnly = value != 0;
		if (key == "downgrade") config.downgrade = strcmp(eq + 1, "off") == 0 ? "" : eq + 1;
		if (key == "crystal") config.crystalEnabled = value != 0;
		if (key == "crystaldrop") config.crystalDropPercent = value;
		if (key == "crystalslot") config.crystalStashSlot = value != 0;
		if (key == "crystalcolor") config.crystalColor = strcmp(eq + 1, "none") == 0 ? '\0' : eq[1];
		if (key == "restore") config.restoreEnabled = value != 0;
		if (key == "restoreline") config.restoreShowLine = value != 0;
		if (key == "strip") config.restoreStrip = value != 0;
		if (key == "restoreoff") config.restoreOff.push_back(eq + 1);
		if (key == "recipes" && strcmp(eq + 1, "crystal") == 0) {
			for (auto& recipe : config.recipes) recipe.materials = "crystal";
		}
	}
	const exalted::Summary s = exalted::Run(io, config);
	printf("ok=%d error='%s' banks=%d bases=%d levels=%d..%d uniques=%d sets=%d recipes=%d downgrades=%d rungs=%d repointed=%d names=%d models=%d files=%d zone=%d zonerows=%d\n", s.ok, s.error.c_str(), s.banks, s.bases,
	       s.levelLow, s.levelHigh, s.uniques, s.sets, s.recipes, s.downgrades, s.rungs, s.repointed, s.names, s.models, s.filesWritten, s.terrorZoneOnly ? 1 : 0, s.zoneRows);
	if (config.crystalEnabled) {
		printf("crystal='%s' tinted=%d restores=%d crystalrows=%d\n", s.crystalCode.c_str(), s.crystalTinted ? 1 : 0, s.restores, s.crystalRows);
	}
	if (!saveCodes.empty()) {
		FILE* f = fopen(saveCodes.c_str(), "wb");
		if (f != nullptr) {
			for (const auto& code : s.codes) fprintf(f, "%s=%s\n", code.first.c_str(), code.second.c_str());
			fclose(f);
		}
	}
	if (!saveFamilies.empty()) {
		// bank kind exalted base tier level:weight ...
		FILE* f = fopen(saveFamilies.c_str(), "wb");
		if (f != nullptr) {
			for (const auto& family : s.families) {
				for (const auto& base : family.bases) {
					fprintf(f, "%d %s %s %s %d", family.bank, family.sets ? "set" : "unique", family.code.c_str(), base.code.c_str(), base.tier);
					for (const auto& row : base.rows) fprintf(f, " %d:%d", row.level, row.weight);
					fprintf(f, "\n");
				}
			}
			fclose(f);
		}
	}
	// Tier choice: a base chosen by FamilyWeights/FamilyChoice, then a row picked the way the
	// game picks (by weight among the rows the item level allows), against one merged list.
	if (pickTest > 0) {
		uint64_t state = 0x9E3779B97F4A7C15ULL;
		auto     next  = [&state](uint32_t below) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			return static_cast<uint32_t>((state >> 20) % below);
		};
		double worst = 0;
		int    cases = 0, wasted = 0;
		for (const auto& family : s.families) {
			if (family.bank != 0 || family.bases.size() < 2) continue;
			for (const int level : { 5, 10, 25, 40, 60, 95 }) {
				uint32_t       weights[exalted::kMaxFamilyBases];
				const uint32_t total = exalted::FamilyWeights(family, level, weights);
				std::vector<double> merged, got;
				double              mergedTotal = 0;
				for (const auto& base : family.bases)
					for (const auto& row : base.rows) {
						merged.push_back(row.level <= level ? row.weight : 0);
						mergedTotal += merged.back();
					}
				got.assign(merged.size(), 0);
				if (total == 0) {
					if (mergedTotal != 0) { printf("pick test: family %s level %d: no base chosen although rows are available\n", family.code.c_str(), level); return 1; }
					continue;
				}
				for (int n = 0; n < pickTest; ++n) {
					const int choice = exalted::FamilyChoice(weights, next(total));
					if (choice < 0) { printf("pick test: no choice\n"); return 1; }
					size_t offset = 0;
					for (int b = 0; b < choice; ++b) offset += family.bases[static_cast<size_t>(b)].rows.size();
					const auto& rows = family.bases[static_cast<size_t>(choice)].rows;
					uint32_t    sum  = 0;
					for (const auto& row : rows) sum += row.level <= level ? static_cast<uint32_t>(row.weight) : 0;
					if (sum == 0) { ++wasted; continue; }
					uint32_t roll = next(sum);
					for (size_t r = 0; r < rows.size(); ++r) {
						const uint32_t w = rows[r].level <= level ? static_cast<uint32_t>(rows[r].weight) : 0;
						if (roll < w) { got[offset + r] += 1; break; }
						roll -= w;
					}
				}
				for (size_t r = 0; r < merged.size(); ++r) worst = std::max(worst, std::fabs(got[r] / pickTest - merged[r] / mergedTotal));
				++cases;
			}
		}
		printf("pick test: %d family/level cases, %d picks each: largest difference from a merged list %.4f, wasted rolls %d -> %s\n", cases, pickTest, worst, wasted,
		       worst < 0.01 && wasted == 0 ? "ok" : "FAILED");
		if (worst >= 0.01 || wasted != 0) return 1;
	}
	return s.ok ? 0 : 1;
}
