// Native timing of the three places the plugin spends time:
//   bench <mod data dir> <vanilla data dir> <game image dump | none> [runs]
//  * the generator (exalted::Run) on a mod's files, with file reads served from memory after
//    the first run, so the number is the generator's own work;
//  * the search for the game's two pickers in a dumped image;
//  * the path compare of the file redirect, on a mix of paths a game opens.
// Builds against any version of the sources:  g++ -I <source dir> test/bench.cpp <source dir>/exalted_core.cpp
#include "exalted_core.h"
#include "game_code.h"
#if __has_include("redirect_match.h")
#include "redirect_match.h"
#define HAVE_REDIRECT_MATCH 1
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

namespace {

double Now() {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s) {
	for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return s;
}

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

struct BenchIo : exalted::Io {
	struct Entry {
		bool        ok = false;
		std::string data;
	};
	std::string mod, base;
	std::map<std::string, Entry>                    cache[2];
	std::map<std::string, std::vector<std::string>> lists;
	size_t written = 0, read = 0;
	int    files   = 0;

	bool Read(int which, const std::string& rel, std::string& out) {
		auto found = cache[which].find(rel);
		if (found == cache[which].end()) {
			Entry       entry;
			std::string path;
			entry.ok = Resolve(which == 0 ? mod : base, rel, path) && ReadFile(path, entry.data);
			found    = cache[which].emplace(rel, std::move(entry)).first;
		}
		if (found->second.ok) {
			out = found->second.data;
			read += out.size();
		}
		return found->second.ok;
	}
	bool ReadMod(const std::string& rel, std::string& out) override { return Read(0, rel, out); }
	bool ReadBase(const std::string& rel, std::string& out) override { return Read(1, rel, out); }
	bool Write(const std::string&, const std::string& data) override {
		written += data.size();
		++files;
		return true;
	}
	void ListMod(const std::string& relDir, std::vector<std::string>& names) override {
		auto found = lists.find(relDir);
		if (found == lists.end()) {
			std::vector<std::string> list;
			std::string              path;
			if (Resolve(mod, relDir, path)) {
				if (DIR* dir = opendir(path.c_str())) {
					while (dirent* e = readdir(dir)) {
						if (e->d_name[0] != '.') list.push_back(e->d_name);
					}
					closedir(dir);
				}
			}
			std::sort(list.begin(), list.end());
			found = lists.emplace(relDir, std::move(list)).first;
		}
		names.insert(names.end(), found->second.begin(), found->second.end());
	}
	void Log(const char*) override {}
};

void Report(const char* what, std::vector<double> times, const char* unit) {
	std::sort(times.begin(), times.end());
	printf("%-34s min %8.2f  median %8.2f  max %8.2f %s (%zu runs)\n", what, times.front(), times[times.size() / 2], times.back(), unit, times.size());
}

// The path compare of v0.9.0 (plugin.cpp MatchRedirect), kept here as the reference to time against.
int OldMatch(const char16_t* text, size_t length, const std::vector<std::u16string>& suffixes) {
	for (size_t i = 0; i < suffixes.size(); ++i) {
		const std::u16string& suffix = suffixes[i];
		const size_t          size   = suffix.size();
		if (length < size) {
			continue;
		}
		const char16_t* tail  = text + (length - size);
		bool            equal = true;
		for (size_t k = size; k-- > 0 && equal;) {
			char16_t c = tail[k];
			if (c >= u'A' && c <= u'Z') {
				c = static_cast<char16_t>(c - u'A' + u'a');
			} else if (c == u'/') {
				c = u'\\';
			}
			equal = c == suffix[k];
		}
		if (equal) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

std::u16string Wide(const std::string& s) {
	return std::u16string(s.begin(), s.end());
}

}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: bench <mod data dir> <vanilla data dir> <game image dump | none> [runs]\n");
		return 2;
	}
	const int runs = argc > 4 ? std::max(3, atoi(argv[4])) : 20;

	// 1. The generator.
	{
		BenchIo io;
		io.mod  = argv[1];
		io.base = argv[2];
		exalted::Config config;
		config.recipes = { { "unique_weapon", "r26 r30 gpg" }, { "unique_armor", "r29 r27 gpw" }, { "set_weapon", "r26 r30 gpg" },
			               { "set_armor", "r29 r27 gpw" },     { "rare_weapon", "r29 r33 gpb" },  { "rare_armor", "r28 r32 gpv" } };
		exalted::Summary summary = exalted::Run(io, config); // fills the file cache
		if (!summary.ok) {
			printf("generator failed: %s\n", summary.error.c_str());
			return 1;
		}
		std::vector<double> times;
		for (int i = 0; i < runs; ++i) {
			io.written = io.read = 0;
			io.files             = 0;
			const double start   = Now();
			summary              = exalted::Run(io, config);
			times.push_back(Now() - start);
		}
		Report("generator (exalted::Run)", times, "ms");
		printf("  %d bases, %d files, %.1f MB read, %.1f MB written per run\n", summary.bases, io.files, io.read / 1048576.0, io.written / 1048576.0);
	}

	// 2. The picker search.
	if (strcmp(argv[3], "none") != 0) {
		std::string image;
		if (!ReadFile(argv[3], image)) {
			printf("cannot read %s\n", argv[3]);
			return 1;
		}
		const auto* bytes = reinterpret_cast<const uint8_t*>(image.data());
		// The parts that hold code, from the section table (file offset == RVA in a dump).
		std::vector<exalted::CodeRange> ranges;
		uint32_t                        lfanew = 0, stamp = 0;
		memcpy(&lfanew, bytes + 0x3c, 4);
		memcpy(&stamp, bytes + lfanew + 8, 4);
		uint16_t sections = 0, optional = 0;
		memcpy(&sections, bytes + lfanew + 6, 2);
		memcpy(&optional, bytes + lfanew + 20, 2);
		size_t total = 0;
		for (unsigned i = 0; i < sections; ++i) {
			const uint8_t* section = bytes + lfanew + 24 + optional + 40 * i;
			uint32_t       vsize = 0, va = 0, flags = 0;
			memcpy(&vsize, section + 8, 4);
			memcpy(&va, section + 12, 4);
			memcpy(&flags, section + 36, 4);
			if ((flags & 0x20000000u) != 0 && va + static_cast<size_t>(vsize) <= image.size()) {
				ranges.push_back({ bytes + va, bytes + va + vsize });
				total += vsize;
			}
		}
		std::vector<double> times;
		exalted::GameCode   code;
		for (int i = 0; i < runs; ++i) {
			const double start = Now();
			code               = exalted::FindGameCode(bytes, ranges);
			times.push_back(Now() - start);
		}
		Report("picker search (whole image)", times, "ms");
		printf("  %.1f MB of code, image stamp %08x: set 0x%llx, unique 0x%llx\n", total / 1048576.0, stamp, static_cast<unsigned long long>(code.pickRva[exalted::PickSet]),
		       static_cast<unsigned long long>(code.pickRva[exalted::PickUnique]));
#ifdef EXALTED_GAME_CODE_HINT
		// With the known addresses of this image handed in, nothing is searched.
		const uint64_t hint[2] = { code.pickRva[0], code.pickRva[1] };
		times.clear();
		exalted::GameCode hinted;
		for (int i = 0; i < runs * 50; ++i) {
			const double start = Now();
			hinted             = exalted::FindGameCode(bytes, ranges, hint);
			times.push_back((Now() - start) * 1000.0);
		}
		Report("picker check (known addresses)", times, "us");
		printf("  set 0x%llx, unique 0x%llx: %s\n", static_cast<unsigned long long>(hinted.pickRva[exalted::PickSet]), static_cast<unsigned long long>(hinted.pickRva[exalted::PickUnique]),
		       hinted.notes.c_str());
#endif
	}

	// 3. The path compare of the file redirect.
	{
		const std::string root = "\\mods\\d2rmm\\d2rmm.mpq\\data\\";
		std::vector<std::u16string> suffixes;
		for (const char* rel : { "global\\excel\\weapons.txt", "global\\excel\\armor.txt", "global\\excel\\cubemain.txt", "global\\excel\\treasureclassex.txt", "global\\excel\\base\\weapons.txt",
			                     "global\\excel\\base\\armor.txt", "global\\excel\\base\\cubemain.txt", "global\\excel\\base\\treasureclassex.txt", "local\\lng\\strings\\item-names.json",
			                     "hd\\items\\items.json" }) {
			suffixes.push_back(Wide(root + rel));
		}
		const std::string game = "\\??\\S:\\steamapps\\common\\Diablo II Resurrected";
		const std::string data = game + "\\mods\\D2RMM\\D2RMM.mpq\\data\\";
		std::vector<std::u16string> paths;
		// What a game opens: mostly models, textures, sounds and archives; some tables and json.
		for (const char* tail : { "hd\\character\\player\\sorceress\\sorceress.model", "hd\\character\\enemy\\fallen\\fallen_lod1.model", "hd\\global\\ui\\panel\\inventory\\background.sprite",
			                      "hd\\env\\model\\act1\\outdoors\\tree_a.model", "hd\\missiles\\missiles.json", "hd\\character\\enemy\\zombie.json", "hd\\items\\weapon\\sword\\long_sword.json",
			                      "hd\\global\\excel\\monstats.txt", "global\\excel\\monstats.txt", "global\\excel\\levels.txt", "global\\excel\\weapons.txt", "local\\lng\\strings\\ui.json",
			                      "hd\\vfx\\textures\\fire_01.texture", "hd\\global\\sfx\\cursor\\button.flac", "global\\excel\\skills.txt", "hd\\items\\items.json" }) {
			paths.push_back(Wide(data + tail));
		}
		for (const char* whole : { "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\Data\\data\\data.004", "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\Data\\indices\\0000000012.idx",
			                       "\\??\\C:\\windows\\system32\\kernelbase.dll", "\\??\\C:\\users\\steamuser\\Saved Games\\Diablo II Resurrected\\mods\\D2RMM\\Sorc.d2s", "\\??\\C:\\windows",
			                       "\\Device\\NamedPipe\\wine", "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\mods\\D2RMM\\d2rloader\\logs\\exalted-items.log", "\\??\\nul" }) {
			paths.push_back(Wide(whole));
		}
		// Nearly every call is for a path that is not an adjusted file, so that is the number that
		// matters; the adjusted files themselves are opened a handful of times per game start.
		std::vector<std::u16string> others, ours;
		for (const std::u16string& path : paths) {
			(OldMatch(path.data(), path.size(), suffixes) >= 0 ? ours : others).push_back(path);
		}
		const int rounds = 400000;
		auto      time   = [&](const std::vector<std::u16string>& list, auto&& match, long& hits) {
			const double start = Now();
			for (int r = 0; r < rounds; ++r) {
				for (const std::u16string& path : list) {
					hits += match(path) >= 0 ? 1 : 0;
				}
			}
			return (Now() - start) * 1e6 / (static_cast<double>(rounds) * static_cast<double>(list.size()));
		};
		long       hitsOld = 0, hitsNew = 0;
		const auto oldMatch = [&](const std::u16string& path) { return OldMatch(path.data(), path.size(), suffixes); };
		printf("%-34s %6.1f ns per call for other paths (%zu kinds), %6.1f ns for an adjusted file\n", "path compare, v0.9.0", time(others, oldMatch, hitsOld), others.size(), time(ours, oldMatch, hitsOld));
#ifdef HAVE_REDIRECT_MATCH
		exalted::SuffixSet<char16_t> set;
		for (const std::u16string& suffix : suffixes) {
			set.Add(suffix);
		}
		const auto newMatch = [&](const std::u16string& path) { return set.Match(path.data(), path.size()); };
		printf("%-34s %6.1f ns per call for other paths (%zu kinds), %6.1f ns for an adjusted file\n", "path compare, now", time(others, newMatch, hitsNew), others.size(), time(ours, newMatch, hitsNew));
		if (hitsNew != hitsOld) {
			printf("MISMATCH between the two compares\n");
			return 1;
		}
#endif
	}
	return 0;
}
