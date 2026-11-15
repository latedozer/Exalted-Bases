// Native test of the file redirect's path compare (redirect_match.h), against the plain
// compare v0.9.0 used: every path has to get the same answer from both.
#include "../redirect_match.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

// plugin.cpp MatchRedirect as it was in v0.9.0.
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

int main() {
	const std::string root = "\\mods\\d2rmm\\d2rmm.mpq\\data\\";
	const char* const files[] = { "global\\excel\\weapons.txt", "global\\excel\\armor.txt", "global\\excel\\cubemain.txt", "global\\excel\\treasureclassex.txt", "global\\excel\\base\\weapons.txt",
		                          "global\\excel\\base\\armor.txt", "global\\excel\\base\\cubemain.txt", "global\\excel\\base\\treasureclassex.txt", "local\\lng\\strings\\item-names.json",
		                          "hd\\items\\items.json" };
	std::vector<std::u16string>  suffixes;
	exalted::SuffixSet<char16_t> set;
	int                          failures = 0, checked = 0, matched = 0;
	if (set.Match(u"anything.txt", 12) != -1) {
		printf("FAIL: an empty set matched something\n");
		++failures;
	}
	for (const char* file : files) {
		suffixes.push_back(Wide(root + file));
		if (set.Add(suffixes.back()) != static_cast<int>(suffixes.size()) - 1) {
			printf("FAIL: Add returned the wrong number\n");
			++failures;
		}
	}

	std::vector<std::u16string> paths;
	const std::string           drive = "\\??\\S:\\steamapps\\common\\Diablo II Resurrected";
	for (const char* file : files) {
		const std::string exact = drive + "\\mods\\D2RMM\\D2RMM.mpq\\data\\" + file;
		paths.push_back(Wide(exact));
		std::string upper = exact, slashed = exact, mixed = exact;
		for (char& c : upper) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
		for (char& c : slashed) c = c == '\\' ? '/' : c;
		for (size_t i = 0; i < mixed.size(); i += 2) mixed[i] = static_cast<char>(mixed[i] >= 'a' && mixed[i] <= 'z' ? mixed[i] - 'a' + 'A' : mixed[i] == '\\' ? '/' : mixed[i]);
		paths.push_back(Wide(upper));
		paths.push_back(Wide(slashed));
		paths.push_back(Wide(mixed));
		paths.push_back(Wide(root + file));                         // exactly the ending, nothing in front
		paths.push_back(Wide((root + file).substr(1)));             // one character short
		paths.push_back(Wide(exact + ".bak"));                      // something after it
		paths.push_back(Wide(exact + " "));
		paths.push_back(Wide(exact.substr(0, exact.size() - 1)));   // last character missing
		paths.push_back(Wide(drive + "\\mods\\Other\\Other.mpq\\data\\" + file)); // another mod
		paths.push_back(Wide(drive + "\\mods\\D2RMM\\D2RMM.mpq\\data\\x" + file));
		paths.push_back(Wide(drive + "\\mods\\D2RMM\\D2RMM.mpq\\data\\" + std::string(file).insert(std::string(file).rfind('\\') + 1, "x"))); // xitems.json
		paths.push_back(Wide(drive + "\\mods\\D2RMM\\D2RMM.mpq\\Data\\" + file));
		paths.push_back(Wide(drive + "\\Mods\\D2RMM\\d2rmm.MPQ\\data\\" + file));
	}
	for (const char* other : { "", "a", "txt", ".txt", "json", "s.json", "\\", "\\??\\C:\\windows\\system32\\kernelbase.dll", "\\??\\S:\\x\\Data\\data\\data.004", "\\Device\\NamedPipe\\wine",
		                       "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\mods\\D2RMM\\D2RMM.mpq\\data\\global\\excel\\monstats.txt",
		                       "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\mods\\D2RMM\\D2RMM.mpq\\data\\hd\\missiles\\missiles.json",
		                       "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\mods\\D2RMM\\D2RMM.mpq\\data\\hd\\items\\items.JSON",
		                       "\\??\\S:\\steamapps\\common\\Diablo II Resurrected\\mods\\D2RMM\\D2RMM.mpq\\data\\global\\excel\\ARMOR.TXT" }) {
		paths.push_back(Wide(other));
	}
	// Characters outside ASCII near the end must not be mistaken for the ending.
	std::u16string odd = Wide(drive + "\\mods\\D2RMM\\D2RMM.mpq\\data\\hd\\items\\items.json");
	odd[odd.size() - 2] = static_cast<char16_t>(0x016F); // not 'o'
	paths.push_back(odd);
	odd = Wide(drive + "\\mods\\D2RMM\\D2RMM.mpq\\data\\global\\excel\\armor.txt");
	odd[odd.size() - 9] = static_cast<char16_t>(0x0100 + 'a');
	paths.push_back(odd);

	for (const std::u16string& path : paths) {
		const int expected = OldMatch(path.data(), path.size(), suffixes);
		const int got      = set.Match(path.data(), path.size());
		++checked;
		matched += expected >= 0 ? 1 : 0;
		if (got != expected) {
			printf("FAIL: '%s' gives %d, expected %d\n", std::string(path.begin(), path.end()).c_str(), got, expected);
			++failures;
		}
	}
	// An ending shorter than four characters switches the quick test off; answers stay right.
	exalted::SuffixSet<char16_t> shortSet;
	std::vector<std::u16string>  shortSuffixes = { u"\\a.txt", u"b", u".json" };
	for (const auto& suffix : shortSuffixes) shortSet.Add(suffix);
	for (const char16_t* text : { u"c:\\a.txt", u"b", u"xb", u"x.JSON", u"", u"a.tx", u"C:/A.TXT" }) {
		const std::u16string path = text;
		++checked;
		if (shortSet.Match(path.data(), path.size()) != OldMatch(path.data(), path.size(), shortSuffixes)) {
			printf("FAIL: short endings, '%s'\n", std::string(path.begin(), path.end()).c_str());
			++failures;
		}
	}
	printf("path compare: %d paths checked against the v0.9.0 compare (%d of them adjusted files), %d wrong -> %s\n", checked, matched, failures, failures == 0 && matched >= 40 ? "ok" : "FAILED");
	return failures == 0 && matched >= 40 ? 0 : 1;
}
