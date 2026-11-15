// Native test of the generator's table and JSON reading: the quick ways of reading (a table
// that is only read, a table taken apart only through one column, one member of a JSON object)
// against the plain way, on real files and on awkward made-up ones.
//   core_test <data dir> [<data dir> ...]
// Build:  g++ -std=c++20 -O2 -DEXALTED_SELF_TEST test/core_test.cpp exalted_core.cpp
#include "../exalted_core.h"

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace exalted {
int SelfTestTable(const std::string& text, void (*report)(const char*));
int SelfTestJson(const std::string& text, void (*report)(const char*));
}

namespace {

const char* g_current = "";

void Report(const char* what) {
	printf("FAIL [%s]: %s\n", g_current, what);
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

bool EndsWith(const std::string& text, const char* end) {
	const std::string tail = end;
	return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

void Walk(const std::string& dir, std::vector<std::string>& files, int depth = 0) {
	DIR* handle = opendir(dir.c_str());
	if (handle == nullptr || depth > 8) return;
	while (dirent* e = readdir(handle)) {
		if (e->d_name[0] == '.') continue;
		const std::string path = dir + "/" + e->d_name;
		struct stat       info {};
		if (stat(path.c_str(), &info) != 0) continue;
		if (S_ISDIR(info.st_mode)) Walk(path, files, depth + 1);
		else files.push_back(path);
	}
	closedir(handle);
}

}

int main(int argc, char** argv) {
	int failures = 0, tables = 0, jsons = 0;

	// Made-up tables: every way a line or a file can end, short and long rows, empty cells.
	const std::vector<std::string> made = {
		"",
		"a",
		"a\n",
		"a\r\n",
		"a\tb\tc",
		"a\tb\tc\n1\t2\t3",
		"a\tb\tc\n1\t2\t3\n",
		"a\tb\tc\r\n1\t2\t3\r\n",
		"a\tb\tc\r\n1\t2\t3\n4\t5\t6\r\n",          // mixed line ends
		"a\tb\tc\r\n\r\n1\t2\t3\r\n\n\n4\r\n",      // empty lines, a short row
		"a\tb\tc\n1\t2\t3\t4\t5\n\t\t\n1\n",        // a long row, an all-empty row
		"a\tb\tc\r\n 12 \t-7\t+3x\r\n\t 0\t99999999999999999999\r\n",
		"name\tcode\tname\r\nx\ty\tz\r\n",          // a column name twice
		"A\tB\t*eol\r\n1\t2\t0\r\n3\t4\t0",         // no line end at the end of the file
		"one\r\n\r\n",
		"\n\n",
		"\t\n\t\t\t\n",
		"a\tb\r\n1\t2\r\r\n3\t4\r\n",               // a stray \r in front of the line end
	};
	for (size_t i = 0; i < made.size(); ++i) {
		const std::string label = "made-up table " + std::to_string(i);
		g_current               = label.c_str();
		failures += exalted::SelfTestTable(made[i], Report);
		++tables;
	}
	const std::vector<std::string> madeJson = {
		"[]",
		"[\n]",
		"[{\"id\":5,\"Key\":\"a\",\"enUS\":\"x\"}]",
		"\xEF\xBB\xBF[\r\n  {\r\n    \"id\": 12,\r\n    \"Key\": \"xlt_abc\",\r\n    \"enUS\": \"say \\\"hi\\\" {x} [y]\"\r\n  },\r\n  {\r\n    \"Key\": \"second\",\r\n    \"id\": 700000\r\n  }\r\n]\r\n",
		"[{\"Key\":\"twice\",\"Key\":\"other\",\"id\":1},{\"nokey\":true,\"id\" : 3},{},{\"id\":}]",
		"[{\"Key\":{\"nested\":[1,2,{\"id\":9}]},\"id\":2}, 5, \"text with \\\"id\\\": 99\", {broken]",
		"[{\"Key\":\"a\",\"id\":1}] \"id\": 77",
	};
	for (size_t i = 0; i < madeJson.size(); ++i) {
		const std::string label = "made-up JSON " + std::to_string(i);
		g_current               = label.c_str();
		failures += exalted::SelfTestJson(madeJson[i], Report);
		++jsons;
	}

	// Real files: every table of the given data folders, and their string and model files.
	for (int a = 1; a < argc; ++a) {
		std::vector<std::string> files;
		Walk(std::string(argv[a]) + "/global/excel", files);
		Walk(std::string(argv[a]) + "/local/lng/strings", files);
		Walk(std::string(argv[a]) + "/hd/items", files);
		for (const std::string& path : files) {
			std::string text;
			if (!ReadFile(path, text)) continue;
			g_current = path.c_str();
			if (EndsWith(path, ".txt")) {
				failures += exalted::SelfTestTable(text, Report);
				++tables;
			} else if (EndsWith(path, ".json")) {
				failures += exalted::SelfTestJson(text, Report);
				++jsons;
			}
		}
	}
	printf("table and JSON reading: %d tables and %d JSON files read both ways, %d differences -> %s\n", tables, jsons, failures, failures == 0 ? "ok" : "FAILED");
	return failures == 0 ? 0 : 1;
}
