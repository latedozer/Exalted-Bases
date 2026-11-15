// Exalted Items: generator for a fourth item tier above elite.
//
// How the tier works with the stock engine (no code patches):
//  * Every elite base gets a copy with its own item code and scaled stats (the exalted base).
//  * The elite row's `ubercode` is pointed at the exalted code. The cube's "exc" output
//    switch turns an item into its row's ubercode, so "useitem,mod,exc" on an ELITE item
//    produces the exalted base and keeps the item itself (unique, set, rare...).
//  * The exalted row keeps the family's original norm/uber/ultra codes, so it counts as
//    none of the three stock tiers and no stock upgrade recipe accepts it.
//
// Drops need no roll of their own. The game sorts every spawnable base into a level table by
// its item level (weap3 ... weap96, armo3 ... armo96, one table per three levels), and an
// exalted base is an ordinary spawnable base. The exalted tier is spread over ten of those
// tables, counted down from Config::dropLevelMax. The game's loot rows only use tables up to
// 87, so for the tables above that the ladder of loot rows is continued (see ExtendLadders).
//
// Unique and set items are not repeated in the tables either. Run() reports, for each exalted
// base, which bases of its family have unique or set rows; the plugin lets the game's own
// picker see one of them for the length of one call.
//
// The mod's own files are only read. Every adjusted file is handed to Io::Write, which keeps
// the copies in a separate folder. Generated rows carry a marker ("[xlt] " / "xlt " / "xlt_")
// and are dropped from the input first.

#include "exalted_core.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

namespace exalted {
namespace {

const char* const kRowMarker    = "[xlt] "; // armor/weapons `name`, cubemain `description`
const char* const kTcMarker     = "xlt ";   // treasure class names
const char* const kStringMarker = "xlt_";   // string keys
const char* const kBanks[]      = { "global/excel/", "global/excel/base/" };
// The game's own test for "this drop happens in a terror zone", as its loot rows write it
// (treasureclassex.txt, ConditionCalc of "All Acts Terrorize Consumable").
const char* const kTerrorZone   = "cond('Desecrated')";
const char* const kStringsDir   = "local/lng/strings";
const char* const kNamesFile    = "local/lng/strings/item-names.json";
const char* const kModelsFile   = "hd/items/items.json";

void Logf(Io& io, const char* format, ...) {
	char    text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	io.Log(text);
}

std::string Lower(std::string text) {
	for (char& c : text) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return text;
}

bool StartsWith(std::string_view text, const char* prefix) {
	const size_t size = strlen(prefix);
	return text.size() >= size && memcmp(text.data(), prefix, size) == 0;
}

std::string Trim(const std::string& text) {
	size_t begin = 0, end = text.size();
	while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
		++begin;
	}
	while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
		--end;
	}
	return text.substr(begin, end - begin);
}

int ToInt(const std::string& text) {
	return atoi(text.c_str());
}

std::string_view TrimView(std::string_view text) {
	size_t begin = 0, end = text.size();
	while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
		++begin;
	}
	while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// The same reading of a number as above, for text that does not end in a zero byte.
int ToInt(std::string_view text) {
	char         buffer[32];
	const size_t size = text.size() < sizeof(buffer) - 1 ? text.size() : sizeof(buffer) - 1;
	memcpy(buffer, text.data(), size);
	buffer[size] = '\0';
	return atoi(buffer);
}

// ---------------------------------------------------------------------------------------
// Tab-separated tables
// ---------------------------------------------------------------------------------------

// Lines are split on "\n" only: a line's "\r" (when it has one) stays on its last cell, so
// files with mixed line endings are written back byte for byte.
struct Table {
	std::string rel;
	bool        loaded      = false;
	std::string original;
	bool        crlf        = true;
	bool        trailingEol = true;
	// A table of which only the first columns are read (cubemain.txt: 600,000 cells, of which
	// one column is looked at) has its rows taken apart only through column `splitThrough`; the
	// rest of each line stays in one piece as the row's last cell, and joining the cells again
	// gives the line back unchanged. Rows added later (NewRow) are whole. Asking for a cell
	// past that column in a row that is not whole is a mistake in this file, and stops the run.
	int          splitThrough  = -1;
	mutable bool readPastSplit = false;
	std::vector<std::string>              header;
	std::vector<std::string>              lowerHeader; // the header in lower case, without a line's "\r": what Col compares with
	std::vector<std::vector<std::string>> rows;

	static bool HasCr(const std::vector<std::string>& row) { return !row.empty() && !row.back().empty() && row.back().back() == '\r'; }

	// Column names are looked up many times; the header is put in lower case once, in Parse.
	int Col(const char* name) const {
		const std::string wanted = Lower(name);
		for (size_t i = 0; i < lowerHeader.size(); ++i) {
			if (lowerHeader[i] == wanted) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	bool PastSplit(const std::vector<std::string>& row, int col) const { return splitThrough >= 0 && col > splitThrough && row.size() < header.size(); }

	std::string Get(const std::vector<std::string>& row, int col) const {
		if (PastSplit(row, col)) {
			readPastSplit = true;
		}
		if (col < 0 || static_cast<size_t>(col) >= row.size()) {
			return {};
		}
		std::string cell = row[static_cast<size_t>(col)];
		if (static_cast<size_t>(col) + 1 == row.size() && !cell.empty() && cell.back() == '\r') {
			cell.pop_back();
		}
		return cell;
	}

	void Set(std::vector<std::string>& row, int col, const std::string& value) const {
		if (col < 0) {
			return;
		}
		if (PastSplit(row, col)) {
			readPastSplit = true;
		}
		const bool cr = HasCr(row);
		if (cr) {
			row.back().pop_back();
		}
		if (row.size() <= static_cast<size_t>(col)) {
			row.resize(static_cast<size_t>(col) + 1);
		}
		row[static_cast<size_t>(col)] = value;
		if (cr) {
			row.back() += '\r';
		}
	}

	std::vector<std::string> NewRow() const {
		std::vector<std::string> row(header.size());
		if (crlf && !row.empty()) {
			row.back() = "\r";
		}
		Set(row, Col("*eol"), "0");
		return row;
	}

	// throughColumn: when given (and the table has that column), rows are taken apart only
	// through it (see splitThrough).
	void Parse(std::string&& data, const char* throughColumn = nullptr) {
		original                = std::move(data);
		const std::string& text = original;
		crlf                    = text.find("\r\n") != std::string::npos;
		std::vector<std::vector<std::string>> lines;
		lines.reserve(static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1);
		size_t pos   = 0;
		size_t width = 0; // cells in the first line: what the other lines are given room for
		trailingEol  = false;
		while (pos <= text.size()) {
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) {
				if (pos == text.size()) {
					trailingEol = !lines.empty();
					break;
				}
				end = text.size();
			}
			std::vector<std::string> cells;
			const bool               whole = lines.empty() || splitThrough < 0;
			cells.reserve(whole ? width : static_cast<size_t>(splitThrough) + 2);
			size_t cellStart = pos;
			for (size_t i = pos; i <= end; ++i) {
				if (i == end || text[i] == '\t') {
					cells.emplace_back(text, cellStart, i - cellStart);
					cellStart = i + 1;
					if (!whole && cells.size() == static_cast<size_t>(splitThrough) + 1 && i < end) {
						cells.emplace_back(text, cellStart, end - cellStart); // the rest of the line, in one piece
						break;
					}
				}
			}
			if (lines.empty()) {
				width = cells.size();
				if (throughColumn != nullptr) {
					const std::string wanted = Lower(throughColumn);
					for (size_t i = 0; i < cells.size() && splitThrough < 0; ++i) {
						std::string cell = Lower(cells[i]);
						if (!cell.empty() && cell.back() == '\r') {
							cell.pop_back();
						}
						if (cell == wanted) {
							splitThrough = static_cast<int>(i);
						}
					}
				}
			}
			lines.push_back(std::move(cells));
			pos = end + 1;
		}
		if (!lines.empty()) {
			header = std::move(lines.front());
			lines.erase(lines.begin());
			rows   = std::move(lines);
			loaded = true;
			lowerHeader.clear();
			lowerHeader.reserve(header.size());
			for (const std::string& name : header) {
				std::string cell = Lower(name);
				if (!cell.empty() && cell.back() == '\r') {
					cell.pop_back();
				}
				lowerHeader.push_back(std::move(cell));
			}
		}
	}

	std::string Serialize() const {
		std::string out;
		out.reserve(original.size() + original.size() / 2 + 4096);
		auto addRow = [&out](const std::vector<std::string>& row) {
			for (size_t i = 0; i < row.size(); ++i) {
				if (i != 0) {
					out += '\t';
				}
				out += row[i];
			}
		};
		addRow(header);
		for (const auto& row : rows) {
			out += '\n';
			addRow(row);
		}
		if (trailingEol) {
			out += '\n';
		}
		return out;
	}
};

bool ReadAny(Io& io, const std::string& rel, std::string& out) {
	return io.ReadMod(rel, out) || io.ReadBase(rel, out);
}

Table LoadTable(Io& io, const std::string& bank, const char* name, const char* throughColumn = nullptr) {
	Table table;
	table.rel = bank + name;
	std::string text;
	if (ReadAny(io, table.rel, text) && !text.empty()) {
		table.Parse(std::move(text), throughColumn);
	}
	return table;
}

// The number of a column, which is added (empty, as the last column) when the file has none of
// that name. The game finds columns by name, so where a column sits does not matter.
int EnsureCol(Table& t, const char* name) {
	const int found = t.Col(name);
	if (found >= 0 || !t.loaded || t.splitThrough >= 0) {
		return found;
	}
	const bool cr = Table::HasCr(t.header);
	if (cr) {
		t.header.back().pop_back();
	}
	t.header.emplace_back(name);
	t.lowerHeader.push_back(Lower(name));
	if (cr) {
		t.header.back() += '\r';
	}
	const int col = static_cast<int>(t.header.size()) - 1;
	for (auto& row : t.rows) {
		if (!row.empty() && !(row.size() == 1 && (row[0].empty() || row[0] == "\r"))) {
			t.Set(row, col, "");
		}
	}
	return col;
}

// A table that is only read. The file's text is kept in one piece and a cell is handed out as
// a view into it, so no cell is copied (of the 700,000 cells of the tables read this way, a
// few thousand are looked at). Lines and cells are told apart exactly as Table::Parse does,
// and Get follows the same rule about a line's "\r".
struct RoTable {
	struct Row {
		uint32_t first = 0; // number of the row's first cell in `starts`
		uint32_t count = 0; // cells in the row
	};

	std::string rel;
	bool        loaded = false;
	std::string text;
	std::vector<uint32_t>    starts; // where each cell begins; after a row's last cell: one past the end of its line
	std::vector<std::string> header; // as Table::header
	std::vector<std::string> lowerHeader;
	std::vector<Row>         rows;

	int Col(const char* name) const {
		const std::string wanted = Lower(name);
		for (size_t i = 0; i < lowerHeader.size(); ++i) {
			if (lowerHeader[i] == wanted) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	std::string_view Get(const Row& row, int col) const {
		if (col < 0 || static_cast<uint32_t>(col) >= row.count) {
			return {};
		}
		const size_t     at = static_cast<size_t>(row.first) + static_cast<size_t>(col);
		std::string_view cell(text.data() + starts[at], starts[at + 1] - 1 - starts[at]);
		if (static_cast<uint32_t>(col) + 1 == row.count && !cell.empty() && cell.back() == '\r') {
			cell.remove_suffix(1);
		}
		return cell;
	}

	void Parse(std::string&& data) {
		text = std::move(data);
		if (text.size() >= 0xFFFFFFF0u) {
			return; // cell positions are kept in 32 bits
		}
		starts.reserve(static_cast<size_t>(std::count(text.begin(), text.end(), '\t')) + 2 * static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 4);
		size_t pos    = 0;
		bool   inBody = false;
		while (pos <= text.size()) {
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) {
				if (pos == text.size()) {
					break;
				}
				end = text.size();
			}
			const size_t first = starts.size();
			starts.push_back(static_cast<uint32_t>(pos));
			for (const char* at = text.data() + pos; (at = static_cast<const char*>(memchr(at, '\t', static_cast<size_t>(text.data() + end - at)))) != nullptr;) {
				++at;
				starts.push_back(static_cast<uint32_t>(at - text.data()));
			}
			starts.push_back(static_cast<uint32_t>(end + 1));
			const size_t count = starts.size() - 1 - first;
			if (inBody) {
				rows.push_back({ static_cast<uint32_t>(first), static_cast<uint32_t>(count) });
			} else {
				for (size_t i = 0; i < count; ++i) {
					header.emplace_back(text, starts[first + i], starts[first + i + 1] - 1 - starts[first + i]);
					std::string cell = Lower(header.back());
					if (!cell.empty() && cell.back() == '\r') {
						cell.pop_back();
					}
					lowerHeader.push_back(std::move(cell));
				}
				inBody = true;
			}
			pos = end + 1;
		}
		loaded = inBody;
	}
};

RoTable LoadRoTable(Io& io, const std::string& bank, const char* name) {
	RoTable table;
	table.rel = bank + name;
	std::string text;
	if (ReadAny(io, table.rel, text) && !text.empty()) {
		table.Parse(std::move(text));
	}
	return table;
}

// ---------------------------------------------------------------------------------------
// Minimal JSON reading: enough to walk an array of flat objects without re-encoding it.
// ---------------------------------------------------------------------------------------

struct Span {
	size_t begin = 0, end = 0;
};

size_t SkipWs(const std::string& s, size_t i) {
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
		++i;
	}
	return i;
}

size_t SkipString(const std::string& s, size_t i) { // s[i] == '"'
	for (++i; i < s.size(); ++i) {
		if (s[i] == '\\') {
			++i;
		} else if (s[i] == '"') {
			return i + 1;
		}
	}
	return std::string::npos;
}

size_t SkipValue(const std::string& s, size_t i) {
	if (i >= s.size()) {
		return std::string::npos;
	}
	if (s[i] == '"') {
		return SkipString(s, i);
	}
	if (s[i] == '{' || s[i] == '[') {
		int depth = 0;
		while (i < s.size()) {
			if (s[i] == '"') {
				i = SkipString(s, i);
				if (i == std::string::npos) {
					return i;
				}
				continue;
			}
			if (s[i] == '{' || s[i] == '[') {
				++depth;
			} else if (s[i] == '}' || s[i] == ']') {
				if (--depth == 0) {
					return i + 1;
				}
			}
			++i;
		}
		return std::string::npos;
	}
	while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
		++i;
	}
	return i;
}

struct JsonArray {
	bool              valid = false;
	std::string       prefix;    // everything up to the first element
	std::string       suffix;    // everything after the last element
	std::string       separator; // text between two elements
	std::vector<std::string> elements;

	void Parse(const std::string& s) {
		size_t i = 0;
		if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) {
			i = 3; // UTF-8 byte-order mark
		}
		i = SkipWs(s, i);
		if (i >= s.size() || s[i] != '[') {
			return;
		}
		size_t cursor = SkipWs(s, i + 1);
		prefix        = s.substr(0, cursor);
		separator     = ",";
		size_t lastEnd = cursor;
		while (cursor < s.size() && s[cursor] != ']') {
			const size_t end = SkipValue(s, cursor);
			if (end == std::string::npos) {
				return;
			}
			elements.push_back(s.substr(cursor, end - cursor));
			lastEnd      = end;
			size_t after = SkipWs(s, end);
			if (after < s.size() && s[after] == ',') {
				const size_t next = SkipWs(s, after + 1);
				if (elements.size() == 1) {
					separator = s.substr(end, next - end);
				}
				cursor = next;
			} else {
				cursor = after;
			}
		}
		if (cursor >= s.size()) {
			return;
		}
		suffix = s.substr(lastEnd);
		valid  = true;
	}

	std::string Serialize() const {
		std::string out = prefix;
		for (size_t i = 0; i < elements.size(); ++i) {
			if (i != 0) {
				out += separator;
			}
			out += elements[i];
		}
		out += suffix;
		return out;
	}
};

struct Member {
	std::string key;   // without quotes
	std::string value; // raw text
};

bool ObjectMembers(const std::string& object, std::vector<Member>& out) {
	out.clear();
	size_t i = SkipWs(object, 0);
	if (i >= object.size() || object[i] != '{') {
		return false;
	}
	i = SkipWs(object, i + 1);
	while (i < object.size() && object[i] != '}') {
		if (object[i] != '"') {
			return false;
		}
		const size_t keyEnd = SkipString(object, i);
		if (keyEnd == std::string::npos) {
			return false;
		}
		Member member;
		member.key = object.substr(i + 1, keyEnd - i - 2);
		i          = SkipWs(object, keyEnd);
		if (i >= object.size() || object[i] != ':') {
			return false;
		}
		i                    = SkipWs(object, i + 1);
		const size_t valueEnd = SkipValue(object, i);
		if (valueEnd == std::string::npos) {
			return false;
		}
		member.value = object.substr(i, valueEnd - i);
		out.push_back(std::move(member));
		i = SkipWs(object, valueEnd);
		if (i < object.size() && object[i] == ',') {
			i = SkipWs(object, i + 1);
		}
	}
	return true;
}

// The raw text of member `key` of a flat object, without taking the whole object apart.
// False exactly when ObjectMembers would be; an empty value when there is no such member. The
// first member of that name counts, as with MemberValue.
bool ObjectValue(const std::string& object, const char* key, std::string_view& value) {
	value                  = {};
	bool         found     = false;
	const size_t keyLength = strlen(key);
	size_t       i         = SkipWs(object, 0);
	if (i >= object.size() || object[i] != '{') {
		return false;
	}
	i = SkipWs(object, i + 1);
	while (i < object.size() && object[i] != '}') {
		if (object[i] != '"') {
			return false;
		}
		const size_t keyEnd = SkipString(object, i);
		if (keyEnd == std::string::npos) {
			return false;
		}
		const size_t nameAt = i + 1, nameLength = keyEnd - i - 2;
		i = SkipWs(object, keyEnd);
		if (i >= object.size() || object[i] != ':') {
			return false;
		}
		i                     = SkipWs(object, i + 1);
		const size_t valueEnd = SkipValue(object, i);
		if (valueEnd == std::string::npos) {
			return false;
		}
		if (!found && nameLength == keyLength && object.compare(nameAt, nameLength, key) == 0) {
			value = std::string_view(object).substr(i, valueEnd - i);
			found = true;
		}
		i = SkipWs(object, valueEnd);
		if (i < object.size() && object[i] == ',') {
			i = SkipWs(object, i + 1);
		}
	}
	return true;
}

std::string_view UnquoteView(std::string_view raw) {
	return raw.size() >= 2 && raw.front() == '"' && raw.back() == '"' ? raw.substr(1, raw.size() - 2) : raw;
}

std::string JsonEscape(const std::string& text) {
	std::string out;
	for (const char c : text) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		if (static_cast<unsigned char>(c) >= 0x20) {
			out += c;
		}
	}
	return out;
}

// Highest `"id": N` in a string file, without parsing the whole file.
long MaxStringId(const std::string& text) {
	long   best = 0;
	size_t pos  = 0;
	while ((pos = text.find("\"id\"", pos)) != std::string::npos) {
		size_t i = SkipWs(text, pos + 4);
		if (i < text.size() && text[i] == ':') {
			i          = SkipWs(text, i + 1);
			const long id = atol(text.c_str() + i);
			best       = std::max(best, id);
		}
		pos += 4;
	}
	return best;
}

// ---------------------------------------------------------------------------------------
// Item tables
// ---------------------------------------------------------------------------------------

struct EliteFamily {
	int         eliteRow = -1;
	int         excRow   = -1;
	std::string eliteCode;
};

struct ItemTable {
	Table table;
	bool  isWeapons = false;
	int   cName = -1, cCode = -1, cNameStr = -1, cNorm = -1, cUber = -1, cUltra = -1;
	std::vector<EliteFamily> families;

	bool Usable() const { return table.loaded && cName >= 0 && cCode >= 0 && cNameStr >= 0 && cNorm >= 0 && cUber >= 0 && cUltra >= 0; }

	void Bind() {
		cName    = table.Col("name");
		cCode    = table.Col("code");
		cNameStr = table.Col("namestr");
		cNorm    = table.Col("normcode");
		cUber    = table.Col("ubercode");
		cUltra   = table.Col("ultracode");
	}

	int FindCode(const std::string& code) const {
		for (size_t i = 0; i < table.rows.size(); ++i) {
			if (table.Get(table.rows[i], cCode) == code) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	// Removes generated rows and gives the elite rows their original ubercode back.
	void Strip(std::set<std::string>& oldCodes) {
		if (!Usable()) {
			return;
		}
		for (const auto& row : table.rows) {
			if (!StartsWith(table.Get(row, cName), kRowMarker)) {
				continue;
			}
			oldCodes.insert(table.Get(row, cCode));
			const int elite = FindCode(table.Get(row, cUltra));
			if (elite >= 0 && table.Get(table.rows[static_cast<size_t>(elite)], cUber) == table.Get(row, cCode)) {
				table.Set(table.rows[static_cast<size_t>(elite)], cUber, table.Get(row, cUber));
			}
		}
		const int nameCol = cName;
		const Table& t    = table;
		table.rows.erase(std::remove_if(table.rows.begin(), table.rows.end(), [&](const std::vector<std::string>& row) { return StartsWith(t.Get(row, nameCol), kRowMarker); }),
		                 table.rows.end());
	}

	void FindFamilies() {
		families.clear();
		if (!Usable()) {
			return;
		}
		std::set<std::string> seen;
		for (size_t i = 0; i < table.rows.size(); ++i) {
			const auto&        row  = table.rows[i];
			const std::string code = table.Get(row, cCode);
			if (code.empty() || code != table.Get(row, cUltra) || code == table.Get(row, cNorm) || !seen.insert(code).second) {
				continue;
			}
			EliteFamily family;
			family.eliteRow  = static_cast<int>(i);
			family.eliteCode = code;
			const std::string uber = table.Get(row, cUber);
			if (!uber.empty() && uber != code) {
				family.excRow = FindCode(uber);
			}
			families.push_back(family);
		}
	}
};

double Median(std::vector<double> values, double fallback) {
	if (values.empty()) {
		return fallback;
	}
	std::sort(values.begin(), values.end());
	const size_t mid = values.size() / 2;
	return values.size() % 2 != 0 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

// One stat, or a min/max pair that has to grow together so min never passes max.
struct StatRule {
	const char* a;
	const char* b; // nullptr for a single stat
	double      lo, hi;
	int         cap; // 0 = none
};

const StatRule kWeaponRules[] = {
	{ "mindam", "maxdam", 1.0, 2.0, 0 },         { "2handmindam", "2handmaxdam", 1.0, 2.0, 0 }, { "minmisdam", "maxmisdam", 1.0, 2.0, 0 },
	{ "reqstr", nullptr, 1.0, 1.5, 0 },          { "reqdex", nullptr, 1.0, 1.5, 0 },            { "durability", nullptr, 1.0, 2.0, 250 },
	{ "cost", nullptr, 1.0, 4.0, 0 },
};
const StatRule kArmorRules[] = {
	{ "minac", "maxac", 1.0, 2.0, 0 },  { "mindam", "maxdam", 1.0, 2.0, 0 },       { "reqstr", nullptr, 1.0, 1.5, 0 },
	{ "reqdex", nullptr, 1.0, 1.5, 0 }, { "durability", nullptr, 1.0, 2.0, 250 }, { "cost", nullptr, 1.0, 4.0, 0 },
};

// The columns of each stat rule of a table (b is -1 for a single stat).
std::vector<std::pair<int, int>> RuleColumns(const ItemTable& items) {
	const StatRule* rules = items.isWeapons ? kWeaponRules : kArmorRules;
	const size_t    count = items.isWeapons ? std::size(kWeaponRules) : std::size(kArmorRules);
	std::vector<std::pair<int, int>> columns;
	for (size_t r = 0; r < count; ++r) {
		columns.emplace_back(items.table.Col(rules[r].a), rules[r].b != nullptr ? items.table.Col(rules[r].b) : -1);
	}
	return columns;
}

// The step from exceptional to elite, repeated once more on top of the elite row.
void ScaleStats(ItemTable& items, const EliteFamily& family, std::vector<std::string>& row, const Config& config, const std::vector<double>& medians,
                const std::vector<std::pair<int, int>>& columns) {
	const Table&    t     = items.table;
	const StatRule* rules = items.isWeapons ? kWeaponRules : kArmorRules;
	const size_t    count = items.isWeapons ? std::size(kWeaponRules) : std::size(kArmorRules);
	const auto&     elite = t.rows[static_cast<size_t>(family.eliteRow)];
	for (size_t r = 0; r < count; ++r) {
		const StatRule& rule = rules[r];
		const int       ca   = columns[r].first;
		const int       cb   = columns[r].second;
		if (ca < 0) {
			continue;
		}
		const double eliteSum = ToInt(t.Get(elite, ca)) + (cb >= 0 ? ToInt(t.Get(elite, cb)) : 0);
		if (eliteSum <= 0) {
			continue;
		}
		double ratio = medians[r];
		if (family.excRow >= 0) {
			const auto&  exc    = t.rows[static_cast<size_t>(family.excRow)];
			const double excSum = ToInt(t.Get(exc, ca)) + (cb >= 0 ? ToInt(t.Get(exc, cb)) : 0);
			if (excSum > 0) {
				ratio = eliteSum / excSum;
			}
		}
		ratio = std::min(std::max(ratio, rule.lo), rule.hi);
		ratio = 1.0 + (ratio - 1.0) * config.statStepPercent / 100.0;
		ratio = std::max(ratio, 0.0);
		int previous = 0;
		for (const int col : { ca, cb }) {
			if (col < 0 || t.Get(elite, col).empty()) {
				continue;
			}
			const int old = ToInt(t.Get(elite, col));
			if (old <= 0) {
				continue;
			}
			int value = static_cast<int>(std::lround(old * ratio));
			if (rule.cap > 0 && value > rule.cap) {
				value = std::max(old, rule.cap);
			}
			value    = std::max(value, previous); // max never below min
			previous = value;
			t.Set(row, col, std::to_string(value));
		}
	}
}

std::vector<double> StatMedians(const ItemTable& items) {
	const Table&    t     = items.table;
	const StatRule* rules = items.isWeapons ? kWeaponRules : kArmorRules;
	const size_t    count = items.isWeapons ? std::size(kWeaponRules) : std::size(kArmorRules);
	std::vector<double> medians;
	for (size_t r = 0; r < count; ++r) {
		const int ca = t.Col(rules[r].a);
		const int cb = rules[r].b != nullptr ? t.Col(rules[r].b) : -1;
		std::vector<double> ratios;
		for (const EliteFamily& family : items.families) {
			if (family.excRow < 0 || ca < 0) {
				continue;
			}
			const auto&  elite = t.rows[static_cast<size_t>(family.eliteRow)];
			const auto&  exc   = t.rows[static_cast<size_t>(family.excRow)];
			const double a     = ToInt(t.Get(exc, ca)) + (cb >= 0 ? ToInt(t.Get(exc, cb)) : 0);
			const double b     = ToInt(t.Get(elite, ca)) + (cb >= 0 ? ToInt(t.Get(elite, cb)) : 0);
			if (a > 0 && b > 0) {
				ratios.push_back(b / a);
			}
		}
		medians.push_back(Median(ratios, 1.0));
	}
	return medians;
}

// ---------------------------------------------------------------------------------------
// Item codes
// ---------------------------------------------------------------------------------------

const char* const kReservedWords[] = { "any", "nos", "noe", "eth", "upg", "bas", "nor", "exc", "eli", "uni", "set", "rar", "mag", "crf", "tmp", "low", "hiq",
	                                   "nru", "qty", "mod", "pre", "suf", "lvl", "uns", "rem", "reg", "rep", "rch", "gld", "sup", "xxx", "sock" };

// A stable three-character code for the exalted version of an elite code: the same elite code
// gets the same exalted code on every run, so saved items keep pointing at the right base.
std::string MakeCode(const std::string& eliteCode, std::set<std::string>& taken) {
	static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	uint32_t          hash       = 2166136261u;
	for (const char c : eliteCode) {
		hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
	}
	uint32_t index = hash % 46656u;
	for (int attempt = 0; attempt < 46656; ++attempt, index = (index + 1) % 46656u) {
		const std::string code { alphabet[index / 1296u], alphabet[index / 36u % 36u], alphabet[index % 36u] };
		if (taken.insert(code).second) {
			return code;
		}
	}
	return {};
}

// ---------------------------------------------------------------------------------------
// One data bank (RotW: global/excel, LoD/Classic: global/excel/base)
// ---------------------------------------------------------------------------------------

struct Bank {
	std::string dir;
	ItemTable   weapons, armor;
	Table       cube, treasure;                                             // adjusted and written
	Table       miscTable, stats, props; // misc.txt, itemstatcost.txt, properties.txt: loaded (and written) only for the crystal
	std::string crystalCode;             // "" = this bank has no crystal
	bool        marker        = false;   // the "restored" stat is in this bank's tables
	int         restoresAdded = 0;
	int         crystalRows   = 0;
	RoTable     misc, itemTypes, uniques, setItems, monStats, superUniques; // only read
	int         index        = 0;
	int         eliteLow     = 0; // lowest and highest item level among the elite bases
	int         eliteHigh    = 0;
	int         levelLow     = 0; // lowest and highest item level given to an exalted base
	int         levelHigh    = 0;
	std::vector<Family> families;
	int         recipesAdded = 0;
	int         downgradesAdded = 0;
	int         rungs        = 0;
	int         repointed    = 0;
	int         zoneRows     = 0;

	bool Usable() const { return weapons.Usable() && armor.Usable(); }
};

template <typename AnyTable>
void StripMarked(AnyTable& table, const char* column, const char* marker) {
	const int col = table.Col(column);
	if (!table.loaded || col < 0) {
		return;
	}
	table.rows.erase(std::remove_if(table.rows.begin(), table.rows.end(), [&](const auto& row) { return StartsWith(table.Get(row, col), marker); }), table.rows.end());
}

template <typename AnyTable>
void AddCodes(const AnyTable& table, const char* column, std::set<std::string>& taken) {
	const int col = table.Col(column);
	if (col < 0) {
		return;
	}
	for (const auto& row : table.rows) {
		std::string code = Lower(std::string(TrimView(table.Get(row, col))));
		if (!code.empty()) {
			taken.insert(std::move(code));
		}
	}
}

// Item level of an elite base's exalted base. The tier fills kLevelTables of the game's level
// tables, three levels apart, counted down from the highest level. An elite base keeps its
// place within the elite range, so a low elite base becomes a low exalted base.
int ExaltedLevel(const Bank& bank, int eliteLevel, const Config& config) {
	const int span  = bank.eliteHigh - bank.eliteLow + 1;
	int       place = span > 0 ? (eliteLevel - bank.eliteLow) * kLevelTables / span : kLevelTables - 1;
	place           = std::min(std::max(place, 0), kLevelTables - 1);
	return std::max(1, config.dropLevelMax - 3 * (kLevelTables - 1 - place));
}

void AddItems(Bank& bank, ItemTable& items, const Config& config, const std::map<std::string, std::string>& codes, const std::map<std::string, std::string>& nameKeys) {
	Table&                    t       = items.table;
	const std::vector<double>              medians = StatMedians(items);
	const std::vector<std::pair<int, int>> columns = RuleColumns(items);
	const int                              cLevel = t.Col("level"), cLevelReq = t.Col("levelreq");
	const int                              cCondition = config.terrorZoneOnly ? EnsureCol(t, "DropConditionCalc") : -1;
	std::vector<std::vector<std::string>> added;
	for (const EliteFamily& family : items.families) {
		const auto codeIt = codes.find(family.eliteCode);
		if (codeIt == codes.end() || codeIt->second.empty()) {
			continue;
		}
		auto&                    elite = t.rows[static_cast<size_t>(family.eliteRow)];
		std::vector<std::string> row   = elite;

		t.Set(row, items.cName, kRowMarker + t.Get(elite, items.cName));
		t.Set(row, items.cCode, codeIt->second);
		const auto nameIt = nameKeys.find(family.eliteCode);
		if (nameIt != nameKeys.end() && !nameIt->second.empty()) {
			t.Set(row, items.cNameStr, nameIt->second);
		}
		// The exalted row keeps the family's original three codes (see the note at the top).
		ScaleStats(items, family, row, config, medians, columns);

		// An ordinary spawnable base: its item level alone decides which of the game's level
		// tables it drops from.
		const int eliteLevel = ToInt(t.Get(elite, cLevel));
		const int eliteReq   = ToInt(t.Get(elite, cLevelReq));
		const int level      = ExaltedLevel(bank, eliteLevel, config);
		t.Set(row, cLevel, std::to_string(level));
		bank.levelLow  = bank.levelLow == 0 ? level : std::min(bank.levelLow, level);
		bank.levelHigh = std::max(bank.levelHigh, level);
		if (cLevelReq >= 0 && eliteLevel > 0 && eliteReq > 0) {
			int req = static_cast<int>(std::lround(static_cast<double>(level) * eliteReq / eliteLevel));
			req     = std::min(req, config.levelReqCap);
			req     = std::max(req, eliteReq);
			t.Set(row, cLevelReq, std::to_string(req));
		}

		// Only in terror zones: the game's own condition, on the base itself. It is still an
		// ordinary member of its level table; the game leaves it out of a roll made elsewhere.
		// (A condition a mod gave the elite base stays, and both must hold.)
		if (cCondition >= 0) {
			const std::string own = Trim(t.Get(row, cCondition));
			t.Set(row, cCondition, own.empty() ? std::string(kTerrorZone) : "(" + own + ")*" + kTerrorZone);
		}

		t.Set(elite, items.cUber, codeIt->second);
		added.push_back(std::move(row));
	}
	for (auto& row : added) {
		t.rows.push_back(std::move(row));
	}
}

// --- uniques and sets -------------------------------------------------------------------

// The game picks a unique or a set piece for a new item by the item's own code, so an exalted
// base finds none. Rows are not repeated for it: a repeated unique is a separate unique to the
// game (its own saved row number, its own entry in a grail), and a repeated set row is an
// extra member of the set. Nothing is written here. This only lists, for each exalted base,
// the bases of its family that have rows, for the plugin's hook on the game's pickers.
void FindFamilies(Io& io, Bank& bank, bool sets, int tiers, const std::map<std::string, std::string>& codes) {
	const RoTable&    t    = sets ? bank.setItems : bank.uniques;
	const char* const file = sets ? "setitems.txt" : "uniqueitems.txt";
	if (!t.loaded || tiers == 0) {
		return;
	}
	const int cIndex = t.Col("index"), cCode = t.Col(sets ? "item" : "code"), cRarity = t.Col("rarity"), cLevel = t.Col("lvl");
	const int cDisabled = t.Col("disabled"), cSpawn = t.Col("spawnable"), cEnabled = t.Col("enabled");
	if (cIndex < 0 || cCode < 0) {
		Logf(io, "%s%s: columns not recognised, exalted bases will not drop as %s items", bank.dir.c_str(), file, sets ? "set" : "unique");
		return;
	}
	struct Target {
		std::string exalted;
		int         tier;
	};
	std::map<std::string, Target, std::less<>> target; // base code -> its family's exalted code
	for (ItemTable* items : { &bank.weapons, &bank.armor }) {
		for (const auto& row : items->table.rows) {
			const std::string code  = items->table.Get(row, items->cCode);
			const std::string elite = items->table.Get(row, items->cUltra);
			const auto        found = codes.find(elite);
			if (code.empty() || found == codes.end() || found->second.empty() || StartsWith(items->table.Get(row, items->cName), kRowMarker)) {
				continue;
			}
			// The game's own test: a base is of the tier whose code column names it.
			const int tier = code == elite ? TierElite : code == items->table.Get(row, items->cUber) ? TierExceptional : TierNormal;
			if ((tiers & tier) != 0) {
				target.emplace(code, Target { found->second, tier });
			}
		}
	}
	// Only rows the game's picker can take are listed. The picker (unique and set alike) skips a
	// row unless its "spawnable" flag is set and its "disabled" flag is not, and a flag is set
	// only by a cell that holds a number other than 0: a blank cell is "no". Mods use that for
	// uniques that are made in the cube and never drop. If such rows were counted here, an
	// exalted base would pose as a base that has nothing to give, and the roll would be lost.
	// ("enabled" is what "spawnable" was called in older table layouts.)
	std::map<std::string, Family> families; // by exalted code
	for (const auto& row : t.rows) {
		const std::string_view base  = t.Get(row, cCode);
		const auto             found = target.find(base);
		if (found == target.end() || t.Get(row, cIndex).empty() || ToInt(t.Get(row, cDisabled)) != 0 || (cSpawn >= 0 && ToInt(t.Get(row, cSpawn)) == 0)
			|| (cEnabled >= 0 && ToInt(t.Get(row, cEnabled)) == 0)) {
			continue;
		}
		Family& family = families[found->second.exalted];
		family.code    = found->second.exalted;
		family.bank    = bank.index;
		family.sets    = sets;
		FamilyBase* entry = nullptr;
		for (FamilyBase& known : family.bases) {
			if (known.code == base) {
				entry = &known;
			}
		}
		if (entry == nullptr) {
			family.bases.push_back({ std::string(base), found->second.tier, {} });
			entry = &family.bases.back();
		}
		entry->rows.push_back({ ToInt(t.Get(row, cLevel)), std::max(1, ToInt(t.Get(row, cRarity))) });
	}
	for (auto& entry : families) {
		bank.families.push_back(std::move(entry.second));
	}
}

// --- cube recipes ---------------------------------------------------------------------

bool ParseRecipeKey(const std::string& key, std::string& quality, std::string& qualityName, std::string& type, std::string& typeName) {
	static const char* const qualities[][3] = { { "unique", "uni", "Unique" }, { "set", "set", "Set" },         { "rare", "rar", "Rare" },
		                                        { "magic", "mag", "Magic" },   { "crafted", "crf", "Crafted" }, { "normal", "nor", "Normal" } };
	const size_t             split         = key.rfind('_');
	if (split == std::string::npos) {
		return false;
	}
	const std::string q = key.substr(0, split), ty = key.substr(split + 1);
	if (ty == "weapon") {
		type     = "weap";
		typeName = "Weapon";
	} else if (ty == "armor") {
		type     = "armo";
		typeName = "Armor";
	} else {
		return false;
	}
	for (const auto& entry : qualities) {
		if (q == entry[0]) {
			quality     = entry[1];
			qualityName = entry[2];
			return true;
		}
	}
	return false;
}

// Item codes separated by spaces, "+" or tabs.
std::vector<std::string> SplitMaterials(const std::string& text) {
	std::vector<std::string> materials;
	std::string              token;
	for (const char c : text + " ") {
		if (c == ' ' || c == '+' || c == '\t') {
			if (!token.empty()) {
				materials.push_back(token);
				token.clear();
			}
		} else {
			token += c;
		}
	}
	return materials;
}

// The word "crystal" in a list of materials stands for the Restoration Crystal, whose item
// code is only known here ("crystal,qty=2" works too). False when the list names the crystal
// and this bank has none.
bool ResolveCrystal(std::vector<std::string>& materials, const std::string& crystalCode) {
	const size_t length = strlen(kCrystalWord);
	for (std::string& material : materials) {
		if (material.compare(0, length, kCrystalWord) == 0 && (material.size() == length || material[length] == ',')) {
			if (crystalCode.empty()) {
				return false;
			}
			material = crystalCode + material.substr(length);
		}
	}
	return true;
}

// Back to elite. The cube's "eli" output switch turns an item into its row's ultracode, and an
// exalted row keeps its family's ultracode, which is the elite base. So "useitem,mod,eli" on
// an exalted item gives the same item (unique, set, rare, plain...) on its elite base.
//
// The game has no way to say "an exalted item" in a recipe (it is none of the three tiers),
// and a recipe that named a kind of item instead ("any weapon") would also lift normal and
// exceptional items to elite. So there is one recipe per exalted base, naming its code.
void AddDowngrades(Io& io, Bank& bank, const Config& config, const std::map<std::string, std::string>& codes) {
	Table& t = bank.cube;
	std::vector<std::string> materials = SplitMaterials(config.downgrade);
	if (!t.loaded || materials.empty()) {
		return;
	}
	if (!ResolveCrystal(materials, bank.crystalCode)) {
		Logf(io, "%srecipe 'downgrade' ignored: it names the crystal, which is turned off or could not be made", bank.dir.c_str());
		return;
	}
	const int cDesc = t.Col("description"), cEnabled = t.Col("enabled"), cVersion = t.Col("version"), cNum = t.Col("numinputs"), cOut = t.Col("output"), cInput = t.Col("input 1");
	if (cDesc < 0 || cNum < 0 || cOut < 0 || cInput < 0) {
		return; // AddRecipes has said so
	}
	if (materials.size() > 6) {
		Logf(io, "recipe 'downgrade' ignored: it needs 1 to 6 materials");
		return;
	}
	int         inputs = 1;
	std::string list;
	for (size_t i = 0; i < materials.size(); ++i) {
		const size_t qty = materials[i].find("qty=");
		inputs += qty != std::string::npos ? std::max(1, atoi(materials[i].c_str() + qty + 4)) : 1;
		list += (i != 0 ? " + " : "") + materials[i];
	}
	for (const ItemTable* items : { &bank.weapons, &bank.armor }) {
		for (const EliteFamily& family : items->families) {
			const auto found = codes.find(family.eliteCode);
			if (found == codes.end() || found->second.empty()) {
				continue;
			}
			// (the elite row's name; the exalted rows were added after these row numbers were taken)
			const std::string name = items->table.Get(items->table.rows[static_cast<size_t>(family.eliteRow)], items->cName);
			std::vector<std::string> row = t.NewRow();
			t.Set(row, cInput, found->second);
			for (size_t i = 0; i < materials.size(); ++i) {
				const std::string column = "input " + std::to_string(i + 2);
				t.Set(row, t.Col(column.c_str()), materials[i].find(',') != std::string::npos ? "\"" + materials[i] + "\"" : materials[i]);
			}
			t.Set(row, cDesc, std::string(kRowMarker) + "Exalted " + name + " -> " + name + " (" + list + ")");
			t.Set(row, cEnabled, "1");
			t.Set(row, cVersion, "100");
			t.Set(row, cNum, std::to_string(inputs));
			t.Set(row, cOut, "\"useitem,mod,eli\"");
			t.rows.push_back(std::move(row));
			++bank.downgradesAdded;
		}
	}
}

void AddRecipes(Io& io, Bank& bank, const Config& config) {
	Table& t = bank.cube;
	if (!t.loaded) {
		return;
	}
	const int cDesc = t.Col("description"), cEnabled = t.Col("enabled"), cVersion = t.Col("version"), cNum = t.Col("numinputs"), cOut = t.Col("output");
	const int cMod = t.Col("mod 1"), cModMin = t.Col("mod 1 min"), cModMax = t.Col("mod 1 max");
	if (cDesc < 0 || cNum < 0 || cOut < 0 || t.Col("input 1") < 0) {
		Logf(io, "%scubemain.txt: columns not recognised, no upgrade recipes added", bank.dir.c_str());
		return;
	}
	for (const Recipe& recipe : config.recipes) {
		std::string quality, qualityName, type, typeName;
		if (Trim(recipe.materials).empty()) {
			continue;
		}
		if (!ParseRecipeKey(recipe.key, quality, qualityName, type, typeName)) {
			Logf(io, "recipe '%s' ignored: the name must be <unique|set|rare|magic|crafted|normal>_<weapon|armor>", recipe.key.c_str());
			continue;
		}
		std::vector<std::string> materials;
		std::string              token;
		for (const char c : recipe.materials + " ") {
			if (c == ' ' || c == '+' || c == '\t') {
				if (!token.empty()) {
					materials.push_back(token);
					token.clear();
				}
			} else {
				token += c;
			}
		}
		if (materials.empty() || materials.size() > 6) {
			Logf(io, "recipe '%s' ignored: it needs 1 to 6 materials", recipe.key.c_str());
			continue;
		}
		if (!ResolveCrystal(materials, bank.crystalCode)) {
			Logf(io, "%srecipe '%s' ignored: it names the crystal, which is turned off or could not be made", bank.dir.c_str(), recipe.key.c_str());
			continue;
		}
		std::vector<std::string> row    = t.NewRow();
		int                      inputs = 1;
		std::string              list;
		t.Set(row, t.Col("input 1"), "\"" + type + ",eli," + quality + "\"");
		for (size_t i = 0; i < materials.size(); ++i) {
			const std::string& material = materials[i];
			const size_t       qty      = material.find("qty=");
			inputs += qty != std::string::npos ? std::max(1, atoi(material.c_str() + qty + 4)) : 1;
			const std::string column = "input " + std::to_string(i + 2);
			t.Set(row, t.Col(column.c_str()), material.find(',') != std::string::npos ? "\"" + material + "\"" : material);
			list += (i != 0 ? " + " : "") + material;
		}
		t.Set(row, cDesc, std::string(kRowMarker) + "Elite " + qualityName + " " + typeName + " -> Exalted " + qualityName + " " + typeName + " (" + list + ")");
		t.Set(row, cEnabled, "1");
		t.Set(row, cVersion, "100");
		t.Set(row, cNum, std::to_string(inputs));
		// "exc" swaps the item to its row's ubercode, which for an elite base is now the exalted base.
		t.Set(row, cOut, "\"useitem,mod,exc\"");
		if (config.upgradeLevelReq > 0 && cMod >= 0 && cModMin >= 0 && cModMax >= 0) {
			t.Set(row, cMod, "levelreq");
			t.Set(row, cModMin, std::to_string(config.upgradeLevelReq));
			t.Set(row, cModMax, std::to_string(config.upgradeLevelReq));
		}
		t.rows.push_back(std::move(row));
		++bank.recipesAdded;
	}
}

// --- loot rows above the game's own -----------------------------------------------------
//
// How the game's loot rows are laid out (TreasureClassEx):
//  * "Equip" rows are rungs of a ladder. Each lists a few neighbouring level tables with fixed
//    weights (2:1, 6:3, 14:7, then 2:1 for the table above its own) and falls back to a lower
//    rung with a large weight. The top rung, "Act 5 (H) Equip C", ends at table 87.
//  * Monster and chest rows (H2H, Cast, Champ, Unique, Chest ...) are ladders of their own.
//    Rows of one `group` with rising `level` form one ladder; the game takes the highest rung
//    whose level is not above the monster's. Each rung names the Equip rung it draws from.
//  * A few rows above the ladders already exist at levels 90, 93 and 96 (superuniques and the
//    terror zone bosses). They still name the Equip rungs A, B and C, the old top three.
//
// Exalted bases with an item level above 87 sit in level tables no rung reaches. So the top
// rungs of every ladder that leads to the top of the Equip ladder are repeated that many
// rungs higher: the same rows, with their level tables raised and their references moved up
// by the same number of rungs. Rows that the game itself places at those levels (the existing
// 90/93/96 rows, and the fixed rows of bosses whose own level is that high) are pointed at
// the new rungs. No existing row gets a new pick, and nothing below the old top changes.

struct TcCols {
	int name, group, level;
	int item[10], prob[10];
};

bool BindTc(const Table& t, TcCols& c) {
	c.name  = t.Col("Treasure Class");
	c.group = t.Col("group");
	c.level = t.Col("level");
	for (int i = 0; i < 10; ++i) {
		c.item[i] = t.Col(("Item" + std::to_string(i + 1)).c_str());
		c.prob[i] = t.Col(("Prob" + std::to_string(i + 1)).c_str());
	}
	return c.name >= 0 && c.group >= 0 && c.level >= 0 && c.item[0] >= 0 && c.prob[0] >= 0;
}

// What a loot row entry names: without quotes and without extras such as ",mul=128".
std::string EntryName(const std::string& cell, bool wholeCell) {
	std::string item = cell;
	if (!item.empty() && item.front() == '"') {
		item = item.substr(1, item.size() >= 2 ? item.size() - 2 : 0);
	}
	return wholeCell ? item : item.substr(0, item.find(','));
}

// The same entry naming something else; quotes and extras stay.
std::string Renamed(const std::string& cell, const std::string& oldName, const std::string& newName) {
	const size_t at = cell.find(oldName);
	return at == std::string::npos ? newName : cell.substr(0, at) + newName + cell.substr(at + oldName.size());
}

// Item types, and which of them the game builds level tables for ("weap", "armo" ...).
struct TypeTree {
	std::map<std::string, std::pair<std::string, std::string>, std::less<>> parents;
	std::set<std::string> tables;

	void Load(const RoTable& t) {
		const int cCode = t.Col("Code"), cFirst = t.Col("Equiv1"), cSecond = t.Col("Equiv2"), cTable = t.Col("TreasureClass");
		if (!t.loaded || cCode < 0) {
			return;
		}
		for (const auto& row : t.rows) {
			const std::string code(TrimView(t.Get(row, cCode)));
			if (code.empty()) {
				continue;
			}
			parents[code] = { std::string(TrimView(t.Get(row, cFirst))), std::string(TrimView(t.Get(row, cSecond))) };
			if (cTable >= 0 && t.Get(row, cTable) == "1") {
				tables.insert(code);
			}
		}
	}

	bool IsA(std::string_view type, const std::string& wanted, int depth = 0) const {
		if (type.empty() || depth > 12) {
			return false;
		}
		if (type == wanted) {
			return true;
		}
		const auto found = parents.find(type);
		return found != parents.end() && (IsA(found->second.first, wanted, depth + 1) || IsA(found->second.second, wanted, depth + 1));
	}
};

// Loot rows by name. Looked up tens of thousands of times (every entry of every row, and every
// loot row column of every monster), and never gone through in order.
struct TextHash {
	using is_transparent = void;
	size_t operator()(std::string_view text) const { return std::hash<std::string_view>()(text); }
};
using NameIndex = std::unordered_map<std::string, int, TextHash, std::equal_to<>>;

struct Ladder {
	std::vector<int> rows;        // row numbers, lowest rung first
	bool             grouped = false;
	bool             leaf    = false; // its rungs list level tables themselves
	bool             lifted  = false; // its top leads to the top of the Equip ladder
	bool             extend  = false; // gets new rungs
	std::vector<std::string> rungs; // names of the new rungs, lowest first
};

void ExtendLadders(Io& io, Bank& bank, const Config& config) {
	Table& t = bank.treasure;
	TcCols c {};
	if (!t.loaded) {
		return;
	}
	if (!BindTc(t, c)) {
		Logf(io, "%streasureclassex.txt: columns not recognised, no loot rows added for the highest exalted bases", bank.dir.c_str());
		return;
	}
	TypeTree types;
	types.Load(bank.itemTypes);
	if (types.tables.empty()) {
		types.tables = { "weap", "armo", "mele", "bow" };
	}
	const int count = static_cast<int>(t.rows.size());
	// Name, group and level of every row, read once. Nothing below changes these cells in the
	// existing rows (only their entries change, and new rows are kept apart until the end).
	std::vector<std::string> rowName(static_cast<size_t>(count)), rowGroup(static_cast<size_t>(count));
	std::vector<int>         rowLevel(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		const auto& row                   = t.rows[static_cast<size_t>(i)];
		rowName[static_cast<size_t>(i)]  = t.Get(row, c.name);
		rowLevel[static_cast<size_t>(i)] = ToInt(t.Get(row, c.level));
		std::string group                = Trim(t.Get(row, c.group));
		if (group != "0") {
			rowGroup[static_cast<size_t>(i)] = std::move(group);
		}
	}
	auto nameOf  = [&](int row) -> const std::string& { return rowName[static_cast<size_t>(row)]; };
	auto levelOf = [&](int row) { return rowLevel[static_cast<size_t>(row)]; };
	auto groupOf = [&](int row) -> const std::string& { return rowGroup[static_cast<size_t>(row)]; };

	NameIndex byName;
	byName.reserve(static_cast<size_t>(count) * 2);
	for (int i = 0; i < count; ++i) {
		byName.emplace(nameOf(i), i);
	}
	// The row an entry names (-1: an item, a level table, or nothing).
	auto refOf = [&](const std::string& cell, std::string* name) {
		for (const bool whole : { true, false }) {
			const std::string wanted = EntryName(cell, whole);
			const auto        found  = wanted.empty() ? byName.end() : byName.find(wanted);
			if (found != byName.end()) {
				if (name != nullptr) {
					*name = wanted;
				}
				return found->second;
			}
		}
		return -1;
	};
	// "weap87" -> weap, 87.
	auto tableOf = [&](const std::string& cell, std::string& stem, int& level) {
		const std::string head = EntryName(cell, false);
		size_t            digits = head.size();
		while (digits > 0 && head[digits - 1] >= '0' && head[digits - 1] <= '9') {
			--digits;
		}
		if (digits == 0 || digits == head.size() || byName.count(head) != 0) {
			return false;
		}
		stem  = head.substr(0, digits);
		level = ToInt(head.substr(digits));
		return types.tables.count(stem) != 0 && level > 0;
	};

	// 1. The highest level table any row uses, and which rows list level tables themselves.
	std::vector<char> lists(static_cast<size_t>(count), 0);
	int               stockTop = 0;
	for (int i = 0; i < count; ++i) {
		for (int k = 0; k < 10; ++k) {
			std::string stem;
			int         level = 0;
			if (tableOf(t.Get(t.rows[static_cast<size_t>(i)], c.item[k]), stem, level)) {
				lists[static_cast<size_t>(i)] = 1;
				stockTop                      = std::max(stockTop, level);
			}
		}
	}
	const int newTop = std::min(kDropLevelCeiling, (bank.levelHigh + 2) / 3 * 3);
	const int more   = stockTop > 0 ? (newTop - stockTop) / 3 : 0; // rungs to add
	if (more <= 0) {
		return; // the game's own rows already reach every table the exalted bases are in
	}

	// 2. Ladders: runs of one group with rising level, and blocks of ungrouped rows that list
	//    level tables (the terror zone heralds have an Equip ladder of that kind).
	std::vector<Ladder> ladders;
	std::vector<int>    ladderOf(static_cast<size_t>(count), -1), placeOf(static_cast<size_t>(count), -1);
	for (int i = 0; i < count;) {
		const std::string group   = groupOf(i);
		const bool        grouped = !group.empty();
		if (!grouped && lists[static_cast<size_t>(i)] == 0) {
			++i;
			continue;
		}
		Ladder ladder;
		ladder.grouped = grouped;
		int j          = i;
		for (; j < count; ++j) {
			if (j > i) {
				const std::string next = groupOf(j);
				if (grouped ? (next != group || levelOf(j) <= levelOf(j - 1)) : (!next.empty() || lists[static_cast<size_t>(j)] == 0)) {
					break;
				}
			}
			ladderOf[static_cast<size_t>(j)] = static_cast<int>(ladders.size());
			placeOf[static_cast<size_t>(j)]  = static_cast<int>(ladder.rows.size());
			ladder.rows.push_back(j);
		}
		ladder.leaf = lists[static_cast<size_t>(ladder.rows.back())] != 0;
		ladders.push_back(std::move(ladder));
		i = j;
	}

	// 3. Which ladders lead to the top. An Equip-like ladder does when its top rung lists the
	//    highest table; any other ladder does when its top rung names one of the top rungs of
	//    a ladder that does.
	int topLevel = 0; // monster level of the game's top rungs
	for (Ladder& ladder : ladders) {
		if (!ladder.leaf) {
			continue;
		}
		for (int k = 0; k < 10; ++k) {
			std::string stem;
			int         level = 0;
			if (tableOf(t.Get(t.rows[static_cast<size_t>(ladder.rows.back())], c.item[k]), stem, level) && level == stockTop) {
				ladder.lifted = true;
			}
		}
		if (ladder.lifted && ladder.grouped) {
			topLevel = std::max(topLevel, levelOf(ladder.rows.back()));
		}
	}
	if (topLevel <= 0) {
		Logf(io, "%streasureclassex.txt: no levelled loot rows use level table %d, so none were added for the tables above it", bank.dir.c_str(), stockTop);
		return;
	}
	auto nearTop = [&](int row) {
		const int ladder = ladderOf[static_cast<size_t>(row)];
		return ladder >= 0 && ladders[static_cast<size_t>(ladder)].lifted
		    && placeOf[static_cast<size_t>(row)] >= static_cast<int>(ladders[static_cast<size_t>(ladder)].rows.size()) - more;
	};
	for (bool changed = true; changed;) {
		changed = false;
		for (Ladder& ladder : ladders) {
			if (ladder.lifted || ladder.leaf || !ladder.grouped) {
				continue;
			}
			const int top = ladder.rows.back();
			for (int k = 0; k < 10 && !ladder.lifted; ++k) {
				const int ref = refOf(t.Get(t.rows[static_cast<size_t>(top)], c.item[k]), nullptr);
				if (ref >= 0 && ladderOf[static_cast<size_t>(ref)] != ladderOf[static_cast<size_t>(top)] && nearTop(ref)) {
					ladder.lifted = true;
					changed       = true;
				}
			}
		}
	}
	// Ladders that already have rows at the new levels (superuniques, terror zone bosses) get
	// no new rungs; their existing rows are pointed at the new rungs in step 5.
	const int firstLevel = topLevel + 3;
	int       laddersExtended = 0;
	for (Ladder& ladder : ladders) {
		ladder.extend = ladder.lifted && (!ladder.grouped || levelOf(ladder.rows.back()) < firstLevel);
		if (!ladder.extend) {
			continue;
		}
		++laddersExtended;
		const int size = static_cast<int>(ladder.rows.size());
		for (int j = 1; j <= more; ++j) {
			const int source = size >= more ? ladder.rows[static_cast<size_t>(size - more + j - 1)] : ladder.rows.back();
			ladder.rungs.push_back(std::string(kTcMarker) + nameOf(source) + " up " + std::to_string(size >= more ? more : j));
		}
	}
	// A rung of a ladder by its place, counting the new rungs on top of the old ones.
	auto rungAt = [&](const Ladder& ladder, int place) {
		const int size = static_cast<int>(ladder.rows.size());
		return place < size ? nameOf(ladder.rows[static_cast<size_t>(place)]) : ladder.rungs[static_cast<size_t>(std::min(place - size, more - 1))];
	};

	// How many spawnable bases the game puts in a level table (its own rule: the base's item
	// type belongs to the table's type, and its item level falls in the table's three levels).
	std::map<std::pair<std::string, int>, int> members;
	bool                                       membersKnown = !types.parents.empty();
	// Which of the table types an item type belongs to, as one bit per table type, worked out
	// once per item type (hundreds of bases share a few dozen types).
	const std::vector<std::string> stems(types.tables.begin(), types.tables.end());
	std::map<std::string, uint64_t, std::less<>> kindsByType;
	auto kindsOf = [&](std::string_view type) {
		auto found = kindsByType.find(type);
		if (found == kindsByType.end()) {
			uint64_t kinds = 0;
			for (size_t s = 0; s < stems.size() && s < 64; ++s) {
				kinds |= types.IsA(type, stems[s]) ? uint64_t { 1 } << s : 0;
			}
			found = kindsByType.emplace(std::string(type), kinds).first;
		}
		return found->second;
	};
	auto countMembers = [&](const auto& table) {
		const int cType = table.Col("type"), cType2 = table.Col("type2"), cLevel = table.Col("level"), cSpawn = table.Col("spawnable"), cQuest = table.Col("quest");
		if (!table.loaded || cType < 0 || cLevel < 0) {
			return;
		}
		for (const auto& row : table.rows) {
			const int  level = ToInt(table.Get(row, cLevel));
			const auto quest = table.Get(row, cQuest);
			if (level <= 0 || (cSpawn >= 0 && table.Get(row, cSpawn) != "1") || (!quest.empty() && quest != "0")) {
				continue;
			}
			const auto     type = table.Get(row, cType), type2 = table.Get(row, cType2);
			const uint64_t kinds = kindsOf(type) | kindsOf(type2);
			for (size_t s = 0; s < stems.size(); ++s) {
				// (the bits cover 64 table types; a mod with more gets the plain test for the rest)
				if (s < 64 ? (kinds >> s & 1) != 0 : types.IsA(type, stems[s]) || types.IsA(type2, stems[s])) {
					++members[{ stems[s], (level + 2) / 3 * 3 }];
				}
			}
		}
	};
	countMembers(bank.weapons.table);
	countMembers(bank.armor.table);
	countMembers(bank.misc);

	// Only in terror zones: a level table above the game's own holds nothing but exalted bases,
	// so outside a terror zone a roll that landed on it would give nothing. A new rung therefore
	// names such a table through a row of its own that carries the terror zone condition, the
	// way the game's loot rows name their terror zone rows. The game leaves a row whose
	// condition fails out of the roll, so outside a terror zone the rung is left with the
	// tables the game's own rows use and its way down to the rung below.
	const int cCondition = config.terrorZoneOnly ? EnsureCol(t, "ConditionCalc") : -1;
	const int cPicks     = t.Col("Picks");
	std::vector<std::vector<std::string>> zoneRows;
	std::set<std::string>                 zoneNames;
	auto zoneTable = [&](const std::string& table) {
		const std::string name = std::string(kTcMarker) + table + " terror zone";
		if (zoneNames.insert(name).second) {
			std::vector<std::string> row = t.NewRow();
			t.Set(row, c.name, name);
			t.Set(row, cPicks, "1");
			t.Set(row, c.item[0], table);
			t.Set(row, c.prob[0], "1");
			t.Set(row, cCondition, kTerrorZone);
			zoneRows.push_back(std::move(row));
		}
		return name;
	};

	// 4. The new rungs.
	std::vector<std::vector<std::vector<std::string>>> added(ladders.size());
	for (size_t l = 0; l < ladders.size(); ++l) {
		const Ladder& ladder = ladders[l];
		if (!ladder.extend) {
			continue;
		}
		const int size = static_cast<int>(ladder.rows.size());
		for (int j = 1; j <= more; ++j) {
			const int                source = size >= more ? ladder.rows[static_cast<size_t>(size - more + j - 1)] : ladder.rows.back();
			const int                lift   = size >= more ? more : j;
			std::vector<std::string> row    = t.rows[static_cast<size_t>(source)];
			t.Set(row, c.name, ladder.rungs[static_cast<size_t>(j - 1)]);
			if (ladder.grouped) {
				t.Set(row, c.level, std::to_string(topLevel + 3 * j));
			}
			for (int k = 0; k < 10; ++k) {
				const std::string cell = t.Get(row, c.item[k]);
				std::string       stem, name;
				int               level = 0;
				if (cell.empty()) {
					continue;
				}
				if (tableOf(cell, stem, level)) {
					int raised = std::min(level + 3 * lift, std::max(level, newTop));
					while (membersKnown && raised > level && members.count({ stem, raised }) == 0) {
						raised -= 3; // nothing of that type at that level: the nearest table below, as the game's rows do
					}
					const std::string table = stem + std::to_string(raised);
					t.Set(row, c.item[k], Renamed(cell, stem + std::to_string(level), cCondition >= 0 && raised > stockTop ? zoneTable(table) : table));
					continue;
				}
				const int ref = refOf(cell, &name);
				if (ref < 0) {
					continue;
				}
				const int other = ladderOf[static_cast<size_t>(ref)];
				if (other == static_cast<int>(l)) {
					t.Set(row, c.item[k], Renamed(cell, name, rungAt(ladder, size + j - 2))); // falls back to the rung below
				} else if (other >= 0 && ladders[static_cast<size_t>(other)].extend) {
					t.Set(row, c.item[k], Renamed(cell, name, rungAt(ladders[static_cast<size_t>(other)], placeOf[static_cast<size_t>(ref)] + lift)));
				}
			}
			added[l].push_back(std::move(row));
			++bank.rungs;
		}
	}

	// 5. Existing rows the game places at the new levels. A grouped row says its level itself.
	//    An ungrouped row is as high as the lowest thing that uses it: a row that names it, or a
	//    boss that drops it (bosses keep their monstats level; other monsters take the level of
	//    their area, which is not known here, so their fixed rows are left alone).
	const int         kUnused = 1 << 30;
	std::vector<int>  usedAt(static_cast<size_t>(count), kUnused);
	std::vector<std::vector<int>> users(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		if (!groupOf(i).empty()) {
			usedAt[static_cast<size_t>(i)] = levelOf(i);
		}
		for (int k = 0; k < 10; ++k) {
			const int ref = refOf(t.Get(t.rows[static_cast<size_t>(i)], c.item[k]), nullptr);
			if (ref >= 0 && ref != i) {
				users[static_cast<size_t>(ref)].push_back(i);
			}
		}
	}
	const int cMonsterId = bank.monStats.Col("Id"), cBoss = bank.monStats.Col("boss");
	std::map<std::string, const RoTable::Row*, std::less<>> monstersById;
	for (const auto& row : bank.monStats.rows) {
		monstersById.emplace(std::string(bank.monStats.Get(row, cMonsterId)), &row);
	}
	for (const RoTable* table : { &bank.monStats, &bank.superUniques }) {
		const bool own    = table == &bank.monStats;
		const int  cClass = table->Col("Class"); // a superunique's monster
		for (size_t col = 0; col < table->header.size(); ++col) {
			const std::string header = Lower(table->header[col]);
			if (own ? header.find("treasureclass") == std::string::npos : !StartsWith(header, "tc")) {
				continue;
			}
			const int cMonsterLevel = bank.monStats.Col(header.find("(h)") != std::string::npos ? "Level(H)" : header.find("(n)") != std::string::npos ? "Level(N)" : "Level");
			for (const auto& row : table->rows) {
				const auto found = byName.find(table->Get(row, static_cast<int>(col)));
				if (found == byName.end() || !groupOf(found->second).empty()) {
					continue;
				}
				const RoTable::Row* monster = &row;
				if (!own) {
					const auto byId = monstersById.find(table->Get(row, cClass));
					monster         = byId != monstersById.end() ? byId->second : nullptr;
				}
				const int level = monster != nullptr && bank.monStats.Get(*monster, cBoss) == "1" ? ToInt(bank.monStats.Get(*monster, cMonsterLevel)) : 0;
				usedAt[static_cast<size_t>(found->second)] = std::min(usedAt[static_cast<size_t>(found->second)], level);
			}
		}
	}
	for (bool changed = true; changed;) {
		changed = false;
		for (int i = 0; i < count; ++i) {
			if (!groupOf(i).empty()) {
				continue;
			}
			for (const int user : users[static_cast<size_t>(i)]) {
				if (usedAt[static_cast<size_t>(user)] < usedAt[static_cast<size_t>(i)]) {
					usedAt[static_cast<size_t>(i)] = usedAt[static_cast<size_t>(user)];
					changed                        = true;
				}
			}
		}
	}
	for (int i = 0; i < count; ++i) {
		const int level = usedAt[static_cast<size_t>(i)];
		const int own   = ladderOf[static_cast<size_t>(i)];
		if (level == kUnused || level < firstLevel || (own >= 0 && ladders[static_cast<size_t>(own)].extend)) {
			continue;
		}
		const int reach   = std::min(more, (level - topLevel) / 3); // new rungs at or below this row's level
		bool      touched = false;
		for (int k = 0; k < 10; ++k) {
			auto&             row  = t.rows[static_cast<size_t>(i)];
			const std::string cell = t.Get(row, c.item[k]);
			std::string       name;
			const int         ref   = refOf(cell, &name);
			const int         other = ref < 0 ? -1 : ladderOf[static_cast<size_t>(ref)];
			if (other < 0 || !ladders[static_cast<size_t>(other)].extend || ladders[static_cast<size_t>(other)].rows.back() > i) {
				continue;
			}
			// Only the old top rungs move, by as many rungs as were added, and never above the
			// rung for this row's own level.
			const Ladder& ladder = ladders[static_cast<size_t>(other)];
			const int     size   = static_cast<int>(ladder.rows.size());
			const int     place  = placeOf[static_cast<size_t>(ref)];
			const int     raised = std::min(place + more, size - 1 + reach);
			if (place < size - more || raised < size) {
				continue;
			}
			t.Set(row, c.item[k], Renamed(cell, name, rungAt(ladder, raised)));
			touched = true;
		}
		bank.repointed += touched ? 1 : 0;
	}

	// 6. Put the new rungs right above their ladders.
	//    The terror zone rows go in front of the first ladder that got new rungs (the game
	//    knows a row only once it has read it, so they must come before the rungs that name
	//    them), moved up to where the row before is not part of any ladder.
	int zoneAt = -1;
	if (!zoneRows.empty()) {
		for (const Ladder& ladder : ladders) {
			if (ladder.extend && (zoneAt < 0 || ladder.rows.front() < zoneAt)) {
				zoneAt = ladder.rows.front();
			}
		}
		while (zoneAt > 0 && ladderOf[static_cast<size_t>(zoneAt - 1)] >= 0) {
			--zoneAt;
		}
	}
	bank.zoneRows = static_cast<int>(zoneRows.size());
	std::vector<std::vector<std::string>> out;
	out.reserve(t.rows.size() + static_cast<size_t>(bank.rungs) + zoneRows.size());
	for (int i = 0; i < count; ++i) {
		if (i == zoneAt) {
			for (auto& row : zoneRows) {
				out.push_back(std::move(row));
			}
		}
		out.push_back(std::move(t.rows[static_cast<size_t>(i)]));
		const int ladder = ladderOf[static_cast<size_t>(i)];
		if (ladder >= 0 && ladders[static_cast<size_t>(ladder)].rows.back() == i) {
			for (auto& row : added[static_cast<size_t>(ladder)]) {
				out.push_back(std::move(row));
			}
		}
	}
	t.rows = std::move(out);
	if (bank.zoneRows > 0) {
		Logf(io, "%streasureclassex.txt: the %d level tables above %d are named through rows that only count in a terror zone", bank.dir.c_str(), bank.zoneRows, stockTop);
	}
	Logf(io, "%streasureclassex.txt: the game's loot rows end at level table %d (monster level %d); %d rungs added for tables up to %d (monster levels %d to %d): %d rows in %d ladders, and %d existing rows of level %d+ pointed at them",
	     bank.dir.c_str(), stockTop, topLevel, more, newTop, firstLevel, topLevel + 3 * more, bank.rungs, laddersExtended, bank.repointed, firstLevel);
}

// --- the Restoration Crystal -------------------------------------------------------------
//
// One more item of the game's "shard" kind, built like the exalted bases: a copy of a row the
// mod already has, with a code and a name of its own. Everything it does is plain table data:
//  * misc.txt: the item (a Worldstone Shard that cannot be used on its own).
//  * cubemain.txt: the word "crystal" in a recipe's materials; and one "restore" recipe per
//    exalted base, which adds the attributes of that base's kind.
//  * itemstatcost.txt / properties.txt: the stat a restored item is marked with. The restore
//    recipes ask for an item without it (cube `op` 18: "the input item's stat equals"), which
//    is how the cube tells "not restored yet", so an item can be restored once.
//  * treasureclassex.txt: the crystal beside the boss essences.

const char* const kCrystalTemplates[] = { "xa1", "tes" }; // Western Worldstone Shard; where the tables have none, an essence
const char* const kEssences[]         = { "tes", "ceh", "bet", "fed" };
const char* const kCrystalAsset       = "xlt_restoration_crystal";
const char* const kCrystalString      = "xlt_crystal";
const char* const kRestoredStat       = "xlt_restored";
const char* const kRestoredProperty   = "xlt-restored";
const char* const kRestoredString     = "xlt_restored";
const char* const kMarkerTemplate     = "item_indesctructible"; // (the game's spelling) a stat that is on an item or is not, with one line of text
const char* const kMiscSprites        = "hd/global/ui/items/misc/";
const char* const kMiscModels         = "hd/items/misc/";

// The row the crystal's row is a copy of: -1 when the table has none.
int CrystalTemplate(const Table& t) {
	const int cCode = t.Col("code"), cType = t.Col("type");
	if (!t.loaded || cCode < 0 || cType < 0) {
		return -1;
	}
	for (const char* wanted : kCrystalTemplates) {
		for (size_t i = 0; i < t.rows.size(); ++i) {
			// (the older tables keep an empty row for the shard: a row without a type is no item)
			if (t.Get(t.rows[i], cCode) == wanted && !Trim(t.Get(t.rows[i], cType)).empty()) {
				return static_cast<int>(i);
			}
		}
	}
	return -1;
}

void AddCrystal(Io& io, Bank& bank, const std::string& code, const std::string& nameKey, bool stashSlot) {
	Table&    t     = bank.miscTable;
	const int cName = t.Col("name"), cCode = t.Col("code"), cNameStr = t.Col("namestr");
	const int from  = CrystalTemplate(t);
	if (code.empty() || cName < 0 || cNameStr < 0 || from < 0) {
		Logf(io, "%smisc.txt: no Worldstone Shard or essence row to build the crystal from; this set of tables gets no crystal", bank.dir.c_str());
		return;
	}
	std::vector<std::string> row      = t.rows[static_cast<size_t>(from)];
	const std::string        fromCode = t.Get(row, cCode);
	t.Set(row, cName, std::string(kRowMarker) + "Restoration Crystal");
	t.Set(row, cCode, code);
	if (!nameKey.empty()) {
		t.Set(row, cNameStr, nameKey);
	}
	if (t.Get(row, t.Col("alternategfx")) == fromCode) {
		t.Set(row, t.Col("alternategfx"), code);
	}
	// It is not used by clicking on it: it only goes into the cube. In the stash it piles up in
	// a slot of its own in the stacking tabs when the stash layout has one for it (see
	// AddStashSlot); without a slot it is an ordinary item.
	t.Set(row, t.Col("useable"), "0");
	t.Set(row, t.Col("pSpell"), "-1");
	for (const char* column : { "calc1", "spelldesc", "spelldescstr", "spelldescstr2", "spelldesccalc", "spelldesccolor", "UsageConditionCalc", "AdvancedStashStackable", "UICatOverride", "EventItem" }) {
		t.Set(row, t.Col(column), "");
	}
	if (stashSlot && t.Col("AdvancedStashStackable") >= 0) {
		t.Set(row, t.Col("AdvancedStashStackable"), "1");
	}
	t.rows.push_back(std::move(row));
	bank.crystalCode = code;
}

// The stat and the property a restored item is marked with. See kRestoredStatId for why the
// stat has a fixed number.
void AddMarker(Io& io, Bank& bank, const Config& config) {
	Table&    s     = bank.stats;
	Table&    p     = bank.props;
	const int cStat = s.Col("Stat"), cId = s.Col("*ID");
	const int cCode = p.Col("code"), cFunc = p.Col("func1"), cPropStat = p.Col("stat1");
	if (!s.loaded || !p.loaded || cStat < 0 || cCode < 0 || cFunc < 0 || cPropStat < 0) {
		Logf(io, "%sitemstatcost.txt / properties.txt could not be read: items cannot be restored in this set of tables", bank.dir.c_str());
		return;
	}
	int from = -1;
	for (size_t i = 0; i < s.rows.size(); ++i) {
		const std::string name = Trim(s.Get(s.rows[i], cStat));
		if (name.empty()) {
			// The game numbers stats by their row, and a row without a name makes that count unsure.
			Logf(io, "%sitemstatcost.txt has a row without a name (row %zu): items cannot be restored in this set of tables", bank.dir.c_str(), i + 2);
			return;
		}
		if (name == kMarkerTemplate) {
			from = static_cast<int>(i);
		}
	}
	if (from < 0 || s.rows.size() > static_cast<size_t>(kRestoredStatId)) {
		Logf(io, from < 0 ? "%sitemstatcost.txt has no '%s' row to build the marker from: items cannot be restored in this set of tables"
		                  : "%sitemstatcost.txt already has more than %d stats, so the marker's number is taken: items cannot be restored in this set of tables",
		     bank.dir.c_str(), from < 0 ? kMarkerTemplate : std::to_string(kRestoredStatId).c_str());
		return;
	}
	std::vector<std::string> marker = s.rows[static_cast<size_t>(from)];
	while (s.rows.size() < static_cast<size_t>(kRestoredStatId)) {
		std::vector<std::string> filler = s.NewRow();
		s.Set(filler, cStat, "xlt_unused_" + std::to_string(s.rows.size()));
		s.Set(filler, cId, std::to_string(s.rows.size()));
		s.rows.push_back(std::move(filler));
	}
	s.Set(marker, cStat, kRestoredStat);
	s.Set(marker, cId, std::to_string(kRestoredStatId));
	if (config.restoreShowLine) {
		s.Set(marker, s.Col("descstrpos"), kRestoredString);
		s.Set(marker, s.Col("descstrneg"), kRestoredString);
	} else {
		for (const char* column : { "descpriority", "descfunc", "descval", "descstrpos", "descstrneg", "descstr2" }) {
			s.Set(marker, s.Col(column), "");
		}
	}
	s.rows.push_back(std::move(marker));

	std::vector<std::string> property = p.NewRow();
	p.Set(property, cCode, kRestoredProperty);
	p.Set(property, p.Col("*Enabled"), "1");
	p.Set(property, cFunc, "1"); // "add this much of the stat"
	p.Set(property, cPropStat, kRestoredStat);
	p.rows.push_back(std::move(property));
	bank.marker = true;
}

const RestoreKind* KindOf(const TypeTree& types, const std::string& type, const std::string& type2) {
	for (const RestoreKind& kind : kRestoreKinds) {
		for (const std::string& wanted : SplitMaterials(kind.types)) {
			if (types.IsA(type, wanted) || types.IsA(type2, wanted)) {
				return &kind;
			}
		}
	}
	return nullptr;
}

// Restoring. One recipe per exalted base, for the reason given at AddDowngrades: the cube
// cannot say "an exalted item" any other way. The item stays what it is ("useitem") and gets
// the marker and the attributes of its kind; the recipe only takes an item without the marker.
void AddRestores(Io& io, Bank& bank, const Config& config, const std::map<std::string, std::string>& codes) {
	Table& t = bank.cube;
	std::vector<std::string> materials = SplitMaterials(config.restoreMaterial);
	if (!t.loaded || !config.restoreEnabled || materials.empty()) {
		return;
	}
	if (materials.size() > 6) {
		Logf(io, "%srestoring is off: it needs 1 to 6 materials", bank.dir.c_str());
		return;
	}
	if (!ResolveCrystal(materials, bank.crystalCode)) {
		if (config.crystalEnabled) { // (turned off in the settings, restoring goes with it and needs no line)
			Logf(io, "%srestoring is off: its material is the crystal, which could not be made", bank.dir.c_str());
		}
		return;
	}
	if (!bank.marker) {
		return; // AddMarker has said why
	}
	const int cDesc = t.Col("description"), cEnabled = t.Col("enabled"), cVersion = t.Col("version"), cNum = t.Col("numinputs"), cOut = t.Col("output"), cInput = t.Col("input 1");
	const int cOp = t.Col("op"), cParam = t.Col("param"), cValue = t.Col("value");
	// The marker and up to four attributes: the five "mod" slots a cube recipe has.
	constexpr int kMods = 5;
	static_assert(std::size(kRestoreKinds[0].attributes) + 1 <= kMods, "a restore recipe has room for the marker and four attributes");
	int       cMod[kMods], cMin[kMods], cMax[kMods];
	bool      columns = cDesc >= 0 && cNum >= 0 && cOut >= 0 && cInput >= 0 && cOp >= 0 && cParam >= 0 && cValue >= 0;
	for (int i = 0; i < kMods; ++i) {
		const std::string name = "mod " + std::to_string(i + 1);
		cMod[i] = t.Col(name.c_str());
		cMin[i] = t.Col((name + " min").c_str());
		cMax[i] = t.Col((name + " max").c_str());
		columns = columns && cMod[i] >= 0 && cMin[i] >= 0 && cMax[i] >= 0;
	}
	if (!columns) {
		Logf(io, "%scubemain.txt: columns not recognised, no restore recipes added", bank.dir.c_str());
		return;
	}
	int         inputs = 1;
	std::string list;
	for (size_t i = 0; i < materials.size(); ++i) {
		const size_t qty = materials[i].find("qty=");
		inputs += qty != std::string::npos ? std::max(1, atoi(materials[i].c_str() + qty + 4)) : 1;
		list += (i != 0 ? " + " : "") + materials[i];
	}
	TypeTree types;
	types.Load(bank.itemTypes);
	const std::set<std::string> off(config.restoreOff.begin(), config.restoreOff.end());
	for (const ItemTable* items : { &bank.weapons, &bank.armor }) {
		const int cType = items->table.Col("type"), cType2 = items->table.Col("type2");
		for (const EliteFamily& family : items->families) {
			const auto found = codes.find(family.eliteCode);
			if (found == codes.end() || found->second.empty()) {
				continue;
			}
			const auto&        elite = items->table.rows[static_cast<size_t>(family.eliteRow)];
			const RestoreKind* kind  = KindOf(types, Trim(items->table.Get(elite, cType)), Trim(items->table.Get(elite, cType2)));
			if (kind == nullptr) {
				continue;
			}
			std::vector<const RestoreAttribute*> attributes;
			for (const RestoreAttribute& attribute : kind->attributes) {
				if (off.count(std::string(kind->key) + "." + attribute.key) == 0) {
					attributes.push_back(&attribute);
				}
			}
			if (attributes.empty()) {
				continue; // every attribute of this kind is turned off: nothing to restore
			}
			const std::string        name = items->table.Get(elite, items->cName);
			std::vector<std::string> row  = t.NewRow();
			t.Set(row, cInput, found->second);
			for (size_t i = 0; i < materials.size(); ++i) {
				const std::string column = "input " + std::to_string(i + 2);
				t.Set(row, t.Col(column.c_str()), materials[i].find(',') != std::string::npos ? "\"" + materials[i] + "\"" : materials[i]);
			}
			t.Set(row, cDesc, std::string(kRowMarker) + "Exalted " + name + " restored: " + kind->label + " (" + list + ")");
			t.Set(row, cEnabled, "1");
			t.Set(row, cVersion, "100");
			t.Set(row, cNum, std::to_string(inputs));
			t.Set(row, cOp, "18"); // the input item's stat `param` equals `value`: not restored yet
			t.Set(row, cParam, std::to_string(kRestoredStatId));
			t.Set(row, cValue, "0");
			t.Set(row, cOut, "useitem");
			t.Set(row, cMod[0], kRestoredProperty);
			t.Set(row, cMin[0], "1");
			t.Set(row, cMax[0], "1");
			for (size_t i = 0; i < attributes.size(); ++i) {
				t.Set(row, cMod[i + 1], attributes[i]->property);
				t.Set(row, cMin[i + 1], std::to_string(attributes[i]->min));
				t.Set(row, cMax[i + 1], std::to_string(attributes[i]->max));
			}
			t.rows.push_back(std::move(row));
			++bank.restoresAdded;
		}
	}
}

// Bosses only. The game's own boss-only item is the essence: each act boss's Hell loot rows
// list one. The crystal is listed beside it, in the same rows, with the essence's weight (or
// the share of it the settings ask for). No row is named here, so a mod's boss rows count too.
//
// Three more bosses drop it in Hell at the user's request: Blood Raven, the Countess and
// Nihlathak. Their rows hold no essence, so there the crystal gets the weight that gives it
// the same share of a pick as it has in the first act boss's row (Andariel's). Their rows
// are found from the monsters themselves (the loot rows monstats.txt / superuniques.txt name
// for Hell, the other rungs of the same ladder, and from those down to the rows that hold the
// boss's items, which are the ones that list gold).
const char* const kCrystalMonsters[]     = { "bloodraven", "nihlathakboss" };
const char* const kCrystalSuperUniques[] = { "The Countess" };
const char* const kMonsterHellRows[]     = { "TreasureClass(H)",           "TreasureClassChamp(H)",           "TreasureClassUnique(H)",          "TreasureClassQuest(H)",
	                                         "TreasureClassDesecrated(H)", "TreasureClassDesecratedChamp(H)", "TreasureClassDesecratedUnique(H)" };
const char* const kSuperUniqueHellRows[] = { "TC(H)", "TC(H) Desecrated" };

void AddCrystalDrops(Io& io, Bank& bank, const Config& config) {
	Table& t = bank.treasure;
	TcCols c {};
	if (bank.crystalCode.empty() || config.crystalDropPercent <= 0 || !t.loaded || !BindTc(t, c)) {
		return;
	}
	const int cNoDrop = t.Col("NoDrop");
	// Everything a pick of this row can land on, "nothing" included.
	auto total = [&](const std::vector<std::string>& row) {
		int sum = ToInt(t.Get(row, cNoDrop));
		for (int i = 0; i < 10; ++i) {
			sum += c.item[i] >= 0 && !t.Get(row, c.item[i]).empty() ? ToInt(t.Get(row, c.prob[i])) : 0;
		}
		return sum;
	};
	// The crystal as one more entry, after the row's last one. False when it is there already.
	auto add = [&](std::vector<std::string>& row, int weight) {
		int last = -1;
		for (int i = 0; i < 10; ++i) {
			const std::string cell = c.item[i] >= 0 ? t.Get(row, c.item[i]) : std::string();
			if (cell.empty()) {
				continue;
			}
			if (EntryName(cell, false) == bank.crystalCode) {
				return false;
			}
			last = i;
		}
		const int slot = last + 1;
		if (slot >= 10 || c.item[slot] < 0 || c.prob[slot] < 0) {
			Logf(io, "%sloot row '%s' has no room left for the crystal", bank.dir.c_str(), t.Get(row, c.name).c_str());
			return false;
		}
		t.Set(row, c.item[slot], bank.crystalCode);
		t.Set(row, c.prob[slot], std::to_string(std::max(1, weight)));
		++bank.crystalRows;
		return true;
	};

	// 1. Beside every essence.
	int shareWeight = 0, shareTotal = 0; // the crystal's share of a pick in the first essence row
	for (auto& row : t.rows) {
		int weight = 0;
		for (int i = 0; i < 10; ++i) {
			const std::string name = c.item[i] >= 0 ? EntryName(t.Get(row, c.item[i]), false) : std::string();
			for (const char* essence : kEssences) {
				if (name == essence) {
					weight = std::max(weight, ToInt(t.Get(row, c.prob[i])));
				}
			}
		}
		if (weight <= 0) {
			continue;
		}
		weight = std::max(1, (weight * config.crystalDropPercent + 50) / 100);
		if (shareTotal == 0) {
			shareWeight = weight;
			shareTotal  = total(row);
		}
		add(row, weight);
	}

	// 2. The three bosses without an essence.
	if (shareTotal <= 0) {
		return; // no act boss row to take the share from
	}
	NameIndex byName;
	for (size_t i = 0; i < t.rows.size(); ++i) {
		byName.emplace(t.Get(t.rows[i], c.name), static_cast<int>(i));
	}
	std::set<int> start;
	auto          named = [&](const RoTable& table, const char* idColumn, const char* id, const char* const* columns, size_t count) {
		const int cId = table.Col(idColumn);
		if (!table.loaded || cId < 0) {
			return;
		}
		for (const auto& row : table.rows) {
			if (table.Get(row, cId) != id) {
				continue;
			}
			for (size_t i = 0; i < count; ++i) {
				const auto found = byName.find(TrimView(table.Get(row, table.Col(columns[i]))));
				if (found != byName.end()) {
					start.insert(found->second);
				}
			}
		}
	};
	for (const char* id : kCrystalMonsters) {
		named(bank.monStats, "Id", id, kMonsterHellRows, std::size(kMonsterHellRows));
	}
	for (const char* id : kCrystalSuperUniques) {
		named(bank.superUniques, "Superunique", id, kSuperUniqueHellRows, std::size(kSuperUniqueHellRows));
	}
	// A row that is a rung of a ladder stands for the ladder from there up: the game takes the
	// rung that fits the monster's level. (The lower rungs of the same ladder belong to Normal
	// and Nightmare, which stay as they are.)
	std::map<std::string, int> groups; // ladder -> level of its lowest Hell rung
	for (const int index : start) {
		const auto&       row   = t.rows[static_cast<size_t>(index)];
		const std::string group = Trim(t.Get(row, c.group));
		if (!group.empty() && group != "0") {
			const int  level = ToInt(t.Get(row, c.level));
			const auto found = groups.find(group);
			if (found == groups.end()) {
				groups.emplace(group, level);
			} else {
				found->second = std::min(found->second, level);
			}
		}
	}
	for (size_t i = 0; i < t.rows.size() && !groups.empty(); ++i) {
		const auto found = groups.find(Trim(t.Get(t.rows[i], c.group)));
		if (found != groups.end() && ToInt(t.Get(t.rows[i], c.level)) >= found->second) {
			start.insert(static_cast<int>(i));
		}
	}
	std::set<int> items; // the rows that hold these bosses' items
	std::vector<std::pair<int, int>> open;
	for (const int index : start) {
		open.emplace_back(index, 0);
	}
	while (!open.empty()) {
		const auto [index, depth] = open.back();
		open.pop_back();
		const auto&      row = t.rows[static_cast<size_t>(index)];
		std::vector<int> below;
		bool             gold = false;
		for (int i = 0; i < 10; ++i) {
			const std::string name = c.item[i] >= 0 ? EntryName(t.Get(row, c.item[i]), false) : std::string();
			gold                   = gold || name == "gld";
			const auto found       = byName.find(name);
			if (!name.empty() && found != byName.end()) {
				below.push_back(found->second);
			}
		}
		if (gold) {
			items.insert(index);
		} else if (depth < 2) {
			for (const int next : below) {
				open.emplace_back(next, depth + 1);
			}
		}
	}
	for (const int index : items) {
		auto& row = t.rows[static_cast<size_t>(index)];
		add(row, static_cast<int>(std::lround(static_cast<double>(total(row)) * shareWeight / shareTotal)));
	}
}

// The crystal's picture: the shard's own, in purple. A sprite is "SpA1", a 40-byte header and
// then one red/green/blue/alpha byte quartet per pixel. Each pixel keeps its brightness and its
// alpha and takes the purple hue; grey pixels are given colour too. False when the data is not
// such a sprite (it is then left alone).
bool TintSprite(std::string& data) {
	constexpr size_t kHeader = 40;
	if (data.size() <= kHeader || (data.size() - kHeader) % 4 != 0 || Lower(data.substr(0, 4)) != "spa1") {
		return false;
	}
	static const double purple[3] = { 0.62, 0.22, 1.0 };
	for (size_t i = kHeader; i + 3 < data.size(); i += 4) {
		const int r = static_cast<unsigned char>(data[i]), g = static_cast<unsigned char>(data[i + 1]), b = static_cast<unsigned char>(data[i + 2]);
		if (data[i + 3] == 0) {
			continue;
		}
		const int    high   = std::max({ r, g, b }), low = std::min({ r, g, b });
		const double colour = 0.6 + 0.4 * (high > 0 ? static_cast<double>(high - low) / high : 0.0);
		for (int k = 0; k < 3; ++k) {
			data[i + static_cast<size_t>(k)] = static_cast<char>(std::min(255, static_cast<int>(high * (1.0 - colour + colour * purple[k]) + 0.5)));
		}
	}
	return true;
}

// --- the crystal's place in the stash -------------------------------------------------
//
// The stash's stacking tabs hold one pile per slot, and a slot is a widget in the stash layout
// that names an item code. An item goes there when its misc.txt row says AdvancedStashStackable
// and the layout has a slot for its code. So the crystal gets one more slot widget in the
// container that holds the Worldstone Shards' slots, the size of a shard's slot, in the free
// place nearest to the first shard.

const char* const kStashLayouts[] = { "global/ui/layouts/bankexpansionlayouthd.json", "global/ui/layouts/controller/bankexpansionlayouthd.json" };
const char* const kSlotWidget     = "AdvancedStashSlotWidget";

struct SlotRect {
	int x = 0, y = 0, w = 0, h = 0;
};

// The item code and place of a slot widget. False for any other widget.
bool SlotOf(const std::string& widget, std::string& itemCode, SlotRect& rect) {
	std::string_view type, fields;
	if (!ObjectValue(widget, "type", type) || UnquoteView(type) != kSlotWidget || !ObjectValue(widget, "fields", fields) || fields.empty()) {
		return false;
	}
	const std::string fieldText(fields);
	std::string_view  code, rawRect;
	if (!ObjectValue(fieldText, "itemCode", code) || code.empty() || !ObjectValue(fieldText, "rect", rawRect)) {
		return false;
	}
	itemCode = Lower(std::string(UnquoteView(code)));
	rect     = SlotRect();
	std::vector<Member> members;
	if (!rawRect.empty() && ObjectMembers(std::string(rawRect), members)) {
		for (const Member& member : members) {
			const int value = ToInt(member.value);
			if (member.key == "x") {
				rect.x = value;
			} else if (member.key == "y") {
				rect.y = value;
			} else if (member.key == "width") {
				rect.w = value;
			} else if (member.key == "height") {
				rect.h = value;
			}
		}
	}
	return rect.w > 0 && rect.h > 0;
}

// A place for one more slot the size of `from`: a column of one existing slot and a row of
// another, free of every slot. The place with the most slots right beside it (left, right,
// above, below) wins, so the new slot continues a row instead of standing alone; among those,
// the one nearest to `from`.
bool FreeSlotPlace(const std::vector<SlotRect>& slots, const SlotRect& from, SlotRect& out) {
	constexpr int kGap = 12, kShift = 8; // how far apart, and how far out of line, neighbours in the game's layouts are
	long long     best = -1;
	int           bestBeside = -1;
	for (const SlotRect& column : slots) {
		for (const SlotRect& line : slots) {
			const SlotRect place { column.x, line.y, from.w, from.h };
			bool           taken = false;
			for (const SlotRect& slot : slots) {
				if (place.x < slot.x + slot.w && slot.x < place.x + place.w && place.y < slot.y + slot.h && slot.y < place.y + place.h) {
					taken = true;
					break;
				}
			}
			if (taken) {
				continue;
			}
			int beside = 0;
			for (const SlotRect& slot : slots) {
				const int gapX = std::max(slot.x - (place.x + place.w), place.x - (slot.x + slot.w)), gapY = std::max(slot.y - (place.y + place.h), place.y - (slot.y + slot.h));
				if ((std::abs(slot.y - place.y) <= kShift && gapX >= 0 && gapX <= kGap) || (std::abs(slot.x - place.x) <= kShift && gapY >= 0 && gapY <= kGap)) {
					++beside;
				}
			}
			const long long dx = place.x - from.x, dy = place.y - from.y, distance = dx * dx + dy * dy;
			if (best < 0 || beside > bestBeside || (beside == bestBeside && (distance < best || (distance == best && (place.y < out.y || (place.y == out.y && place.x < out.x)))))) {
				best       = distance;
				bestBeside = beside;
				out        = place;
			}
		}
	}
	return best >= 0;
}

// The text of a slot widget for `code` at `place`: the widget it is modelled on, with its
// name, item code and place changed and anything else it carries kept.
std::string SlotWidgetFor(const std::string& model, const std::string& code, const SlotRect& place) {
	std::vector<Member> members, fields;
	ObjectMembers(model, members);
	std::string out = "{";
	for (const Member& member : members) {
		out += (out.size() > 1 ? "," : "") + ("\"" + member.key + "\":");
		if (member.key == "name") {
			out += "\"" + code + "\"";
		} else if (member.key == "fields" && ObjectMembers(member.value, fields)) {
			std::string inner = "{";
			for (const Member& field : fields) {
				inner += (inner.size() > 1 ? "," : "") + ("\"" + field.key + "\":");
				if (field.key == "rect") {
					inner += "{\"width\":" + std::to_string(place.w) + ",\"height\":" + std::to_string(place.h) + ",\"x\":" + std::to_string(place.x) + ",\"y\":" + std::to_string(place.y) + "}";
				} else if (field.key == "itemCode") {
					inner += "\"" + code + "\"";
				} else {
					inner += field.value;
				}
			}
			out += inner + "}";
		} else {
			out += member.value;
		}
	}
	return out + "}";
}

enum class SlotResult { None, Added, Present, NoRoom };

// Called where the slot goes in: the name of the container widget, the place of the slot it is
// modelled on and the new slot's place: the moment to give the slot its frame. Returns the
// text of a widget to put in before the new slot, or "" for none (always "" today: the game
// takes every widget in a slot container for a slot).
using FrameMaker = std::function<std::string(const std::string& container, const SlotRect& model, const SlotRect& placed)>;

// Copies one part of a one-picture sprite ("SpA1": 40-byte header, then RGBA rows) to another
// place in the same picture.
bool CopyInSprite(std::string& sprite, int x0, int y0, int x1, int y1, int toX, int toY) {
	constexpr size_t kHeader = 40;
	if (sprite.size() <= kHeader || Lower(sprite.substr(0, 4)) != "spa1") {
		return false;
	}
	uint32_t width = 0, height = 0;
	memcpy(&width, sprite.data() + 8, 4);
	memcpy(&height, sprite.data() + 12, 4);
	const int w = x1 - x0, h = y1 - y0;
	if (x0 < 0 || y0 < 0 || w <= 0 || h <= 0 || toX < 0 || toY < 0 || static_cast<uint32_t>(x1) > width || static_cast<uint32_t>(y1) > height || static_cast<uint32_t>(toX + w) > width
	    || static_cast<uint32_t>(toY + h) > height || sprite.size() < kHeader + static_cast<size_t>(width) * height * 4) {
		return false;
	}
	const std::string original = sprite.substr(kHeader + static_cast<size_t>(y0) * width * 4, static_cast<size_t>(h) * width * 4); // the rows read from
	for (int y = 0; y < h; ++y) {
		memcpy(sprite.data() + kHeader + (static_cast<size_t>(toY) + y) * width * 4 + static_cast<size_t>(toX) * 4, original.data() + static_cast<size_t>(y) * width * 4 + static_cast<size_t>(x0) * 4,
		       static_cast<size_t>(w) * 4);
	}
	return true;
}

// Gives the layout widget `widget` (raw text of a JSON object), or the first widget below it
// that holds a slot for one of the crystal's model items, a slot for `code`.
SlotResult AddStashSlot(std::string& widget, const std::string& code, SlotRect& placed, const FrameMaker& frame, int depth = 0) {
	std::string_view rawChildren;
	if (depth > 16 || !ObjectValue(widget, "children", rawChildren) || rawChildren.empty()) {
		return SlotResult::None;
	}
	const size_t at = static_cast<size_t>(rawChildren.data() - widget.data()), length = rawChildren.size();
	JsonArray    children;
	children.Parse(std::string(rawChildren));
	if (!children.valid) {
		return SlotResult::None;
	}
	std::vector<SlotRect> slots;
	int                   model = -1, modelRank = 0;
	SlotRect              modelRect;
	for (size_t i = 0; i < children.elements.size(); ++i) {
		std::string itemCode;
		SlotRect    rect;
		if (!SlotOf(children.elements[i], itemCode, rect)) {
			continue;
		}
		if (itemCode == code) {
			placed = rect;
			return SlotResult::Present;
		}
		slots.push_back(rect);
		int rank = 0;
		for (const char* wanted : kCrystalTemplates) {
			if (itemCode == wanted && (model < 0 || rank < modelRank)) {
				model     = static_cast<int>(i);
				modelRank = rank;
				modelRect = rect;
			}
			++rank;
		}
	}
	if (model >= 0) {
		if (!FreeSlotPlace(slots, modelRect, placed)) {
			return SlotResult::NoRoom;
		}
		std::string_view  rawName;
		ObjectValue(widget, "name", rawName);
		const std::string behind = frame ? frame(Lower(std::string(UnquoteView(rawName))), modelRect, placed) : std::string();
		const std::string slot   = SlotWidgetFor(children.elements[static_cast<size_t>(model)], code, placed);
		// After the last slot, the frame first: it is drawn before the slot, so behind it.
		size_t last = static_cast<size_t>(model);
		for (size_t i = last + 1; i < children.elements.size(); ++i) {
			std::string itemCode;
			SlotRect    rect;
			if (SlotOf(children.elements[i], itemCode, rect)) {
				last = i;
			}
		}
		children.elements.insert(children.elements.begin() + static_cast<std::ptrdiff_t>(last) + 1, slot);
		if (!behind.empty()) {
			children.elements.insert(children.elements.begin() + static_cast<std::ptrdiff_t>(last) + 1, behind);
		}
		widget.replace(at, length, children.Serialize());
		return SlotResult::Added;
	}
	for (std::string& child : children.elements) {
		const SlotResult result = AddStashSlot(child, code, placed, frame, depth + 1);
		if (result == SlotResult::Added) {
			widget.replace(at, length, children.Serialize());
		}
		if (result != SlotResult::None) {
			return result;
		}
	}
	return SlotResult::None;
}

// --- names and models -----------------------------------------------------------------

struct JsonFile {
	std::string rel;
	bool        loaded = false;
	std::string original;
	JsonArray   array;
};

JsonFile LoadJson(Io& io, const char* rel) {
	JsonFile file;
	file.rel = rel;
	if (ReadAny(io, rel, file.original) && !file.original.empty()) {
		file.array.Parse(file.original);
		file.loaded = file.array.valid;
	}
	return file;
}

}

// ---------------------------------------------------------------------------------------

Summary Run(Io& io, const Config& settings) {
	Summary summary;
	Config  config      = settings;
	config.dropLevelMax = std::min(std::max(config.dropLevelMax, kDropLevelFloor), kDropLevelCeiling);

	// 1. Load both data banks and take out everything a previous run generated.
	std::vector<Bank>     banks;
	std::set<std::string> oldCodes;
	for (const char* dir : kBanks) {
		Bank bank;
		bank.dir               = dir;
		bank.weapons.table     = LoadTable(io, bank.dir, "weapons.txt");
		bank.weapons.isWeapons = true;
		bank.armor.table       = LoadTable(io, bank.dir, "armor.txt");
		bank.weapons.Bind();
		bank.armor.Bind();
		if (!bank.Usable()) {
			if (bank.weapons.table.loaded || bank.armor.table.loaded) {
				Logf(io, "%s: armor.txt/weapons.txt are missing expected columns, bank skipped", dir);
			}
			continue;
		}
		bank.misc      = LoadRoTable(io, bank.dir, "misc.txt");
		bank.itemTypes = LoadRoTable(io, bank.dir, "itemtypes.txt");
		bank.cube      = LoadTable(io, bank.dir, "cubemain.txt", "description"); // only its description column is read
		bank.treasure  = LoadTable(io, bank.dir, "treasureclassex.txt");
		bank.uniques   = LoadRoTable(io, bank.dir, "uniqueitems.txt");
		bank.setItems  = LoadRoTable(io, bank.dir, "setitems.txt");
		bank.monStats  = LoadRoTable(io, bank.dir, "monstats.txt");
		bank.superUniques = LoadRoTable(io, bank.dir, "superuniques.txt");
		bank.index     = dir == kBanks[0] ? 0 : 1;
		if (config.enabled && config.crystalEnabled) {
			bank.miscTable = LoadTable(io, bank.dir, "misc.txt");
			bank.stats     = LoadTable(io, bank.dir, "itemstatcost.txt");
			bank.props     = LoadTable(io, bank.dir, "properties.txt");
			StripMarked(bank.miscTable, "name", kRowMarker);
		}
		StripMarked(bank.uniques, "*ItemName", kRowMarker);
		bank.weapons.Strip(oldCodes);
		bank.armor.Strip(oldCodes);
		StripMarked(bank.cube, "description", kRowMarker);
		StripMarked(bank.treasure, "Treasure Class", kTcMarker);
		banks.push_back(std::move(bank));
	}
	if (banks.empty()) {
		summary.error = "armor.txt and weapons.txt could not be read from the mod or from the game data";
		return summary;
	}
	summary.banks = static_cast<int>(banks.size());

	JsonFile names  = LoadJson(io, kNamesFile);
	JsonFile models = LoadJson(io, kModelsFile);
	std::vector<Member> members;
	std::string_view    rawKey;
	if (names.loaded) {
		auto& e = names.array.elements;
		e.erase(std::remove_if(e.begin(), e.end(), [&](const std::string& element) { return ObjectValue(element, "Key", rawKey) && StartsWith(UnquoteView(rawKey), kStringMarker); }), e.end());
	}
	if (models.loaded && !oldCodes.empty()) {
		auto& e = models.array.elements;
		e.erase(std::remove_if(e.begin(), e.end(), [&](const std::string& element) { return ObjectMembers(element, members) && members.size() == 1 && oldCodes.count(members[0].key) != 0; }),
		        e.end());
	}

	std::map<std::string, std::string> codes;    // elite code -> exalted code
	std::map<std::string, std::string> nameKeys; // elite code -> string key of the exalted name
	std::string crystalCode, crystalNameKey;
	std::vector<std::pair<std::string, std::string>> extraFiles; // adjusted files that are not tables: the crystal's picture and model
	std::vector<std::string> keptFiles;                          // the same, already stored by an earlier start

	summary.terrorZoneOnly = config.enabled && config.terrorZoneOnly;
	if (config.enabled) {
		// 2. A code for every elite base. Sorted, so the result does not depend on file order.
		std::set<std::string> taken(std::begin(kReservedWords), std::end(kReservedWords));
		std::map<std::string, std::string> eliteNameStr;
		for (Bank& bank : banks) {
			AddCodes(bank.weapons.table, "code", taken);
			AddCodes(bank.armor.table, "code", taken);
			AddCodes(bank.misc, "code", taken);
			AddCodes(bank.itemTypes, "Code", taken);
			for (ItemTable* items : { &bank.weapons, &bank.armor }) {
				items->FindFamilies();
				const int cLevel = items->table.Col("level");
				for (const EliteFamily& family : items->families) {
					const auto& elite = items->table.rows[static_cast<size_t>(family.eliteRow)];
					const int   level = ToInt(items->table.Get(elite, cLevel));
					codes.emplace(family.eliteCode, "");
					eliteNameStr.emplace(family.eliteCode, items->table.Get(elite, items->cNameStr));
					if (level > 0) {
						bank.eliteLow  = bank.eliteLow == 0 ? level : std::min(bank.eliteLow, level);
						bank.eliteHigh = std::max(bank.eliteHigh, level);
					}
				}
			}
		}
		// A saved item names its base by code, so a family that has a code keeps it for good.
		// Only a family seen for the first time gets a code worked out from its elite code.
		const std::map<std::string, std::string> saved(config.savedCodes.begin(), config.savedCodes.end());
		for (auto& entry : codes) {
			const auto found = saved.find(entry.first);
			if (found == saved.end()) {
				continue;
			}
			const std::string& code  = found->second;
			const bool         valid = code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); });
			if (valid && taken.insert(code).second) {
				entry.second = code;
			} else {
				Logf(io, "the exalted base of '%s' cannot keep its code '%s' (another item uses it now). It gets a new code; items saved with the old one are lost.", entry.first.c_str(), code.c_str());
			}
		}
		// The crystal's code is kept the same way. Only a set of tables that has a row to build
		// it from gets one (see AddCrystal).
		bool wantCrystal = false;
		for (const Bank& bank : banks) {
			wantCrystal = wantCrystal || CrystalTemplate(bank.miscTable) >= 0;
		}
		if (wantCrystal) {
			const auto found = saved.find(kCrystalKey);
			if (found != saved.end()) {
				const std::string& code  = found->second;
				const bool         valid = code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); });
				if (valid && taken.insert(code).second) {
					crystalCode = code;
				} else {
					Logf(io, "the Restoration Crystal cannot keep its code '%s' (another item uses it now). It gets a new code; crystals saved with the old one are lost.", code.c_str());
				}
			}
		} else if (config.crystalEnabled) {
			io.Log("misc.txt has no Worldstone Shard or essence row to build the Restoration Crystal from: there is no crystal");
		}
		// A base that is not in the mod right now keeps its code too: nothing else may take it.
		for (const auto& entry : saved) {
			taken.insert(entry.second);
		}
		for (auto& entry : codes) {
			if (entry.second.empty()) {
				entry.second = MakeCode(entry.first, taken);
			}
			summary.codes.emplace_back(entry.first, entry.second);
		}
		if (wantCrystal) {
			if (crystalCode.empty()) {
				crystalCode = MakeCode("restoration crystal", taken);
			}
			summary.codes.emplace_back(kCrystalKey, crystalCode);
			summary.crystalCode = crystalCode;
		}
		summary.bases = static_cast<int>(codes.size());

		// 3. Names: a copy of each elite base's name, in every language, with the prefix in front.
		if (names.loaded) {
			// Ids start above everything in use (the old generated names are already gone here,
			// so the same ids come out on every run).
			long nextId = std::max({ 300000L, MaxStringId(names.array.prefix), MaxStringId(names.array.suffix) });
			for (const std::string& element : names.array.elements) {
				nextId = std::max(nextId, MaxStringId(element));
			}
			std::vector<std::string> stringFiles;
			io.ListMod(kStringsDir, stringFiles);
			for (const std::string& file : stringFiles) {
				std::string text;
				const std::string lower = Lower(file);
				if (lower.size() > 5 && lower.rfind(".json") == lower.size() - 5 && lower != "item-names.json" && io.ReadMod(std::string(kStringsDir) + "/" + file, text)) {
					nextId = std::max(nextId, MaxStringId(text));
				}
			}
			std::map<std::string, size_t> byKey;
			for (size_t i = 0; i < names.array.elements.size(); ++i) {
				if (ObjectValue(names.array.elements[i], "Key", rawKey)) {
					byKey.emplace(std::string(UnquoteView(rawKey)), i);
				}
			}
			const std::string prefix = JsonEscape(config.namePrefix);
			for (const auto& entry : codes) {
				const auto found = byKey.find(eliteNameStr[entry.first]);
				if (entry.second.empty() || found == byKey.end() || !ObjectMembers(names.array.elements[found->second], members)) {
					continue;
				}
				const std::string key = kStringMarker + entry.second;
				std::string       object = "{";
				for (size_t i = 0; i < members.size(); ++i) {
					const Member& member = members[i];
					object += (i != 0 ? ",\"" : "\"") + member.key + "\":";
					if (member.key == "id") {
						object += std::to_string(++nextId);
					} else if (member.key == "Key") {
						object += "\"" + key + "\"";
					} else if (member.value.size() >= 2 && member.value.front() == '"') {
						// Grammar tags such as "[fs]" stay at the very front.
						size_t at = 1;
						while (at < member.value.size() && member.value[at] == '[') {
							const size_t close = member.value.find(']', at);
							if (close == std::string::npos || close - at > 6) {
								break;
							}
							at = close + 1;
						}
						object += member.value.substr(0, at) + prefix + member.value.substr(at);
					} else {
						object += member.value;
					}
				}
				object += "}";
				names.array.elements.push_back(object);
				nameKeys[entry.first] = key;
				++summary.names;
			}
			// The crystal's name, and the line a restored item shows: the same text in every language.
			if (!crystalCode.empty() && !names.array.elements.empty()) {
				const auto        shard    = byKey.find(kCrystalTemplates[0]);
				const std::string pattern  = names.array.elements[shard != byKey.end() ? shard->second : 0];
				auto              addString = [&](const char* key, const std::string& text) {
					if (!ObjectMembers(pattern, members)) {
						return false;
					}
					std::string object = "{";
					for (size_t i = 0; i < members.size(); ++i) {
						const Member& member = members[i];
						object += (i != 0 ? ",\"" : "\"") + member.key + "\":";
						if (member.key == "id") {
							object += std::to_string(++nextId);
						} else if (member.key == "Key") {
							object += std::string("\"") + key + "\"";
						} else if (member.value.size() >= 2 && member.value.front() == '"') {
							object += "\"" + JsonEscape(text) + "\"";
						} else {
							object += member.value;
						}
					}
					names.array.elements.push_back(object + "}");
					return true;
				};
				const std::string colour = config.crystalColor != 0 ? std::string("\xC3\xBF" "c") + config.crystalColor : std::string();
				if (addString(kCrystalString, colour + config.crystalName)) {
					crystalNameKey = kCrystalString;
				}
				if (config.restoreEnabled && config.restoreShowLine) {
					addString(kRestoredString, "Restored");
				}
			}
		} else {
			io.Log("item-names.json could not be read: exalted bases will show their elite names");
		}

		// 4. Models: the exalted base looks like its elite base.
		if (models.loaded) {
			std::map<std::string, std::string> byCode;
			for (const std::string& element : models.array.elements) {
				if (ObjectMembers(element, members) && members.size() == 1) {
					byCode.emplace(members[0].key, members[0].value);
				}
			}
			for (const auto& entry : codes) {
				const auto found = byCode.find(entry.first);
				if (!entry.second.empty() && found != byCode.end() && byCode.count(entry.second) == 0) {
					models.array.elements.push_back("{\"" + entry.second + "\":" + found->second + "}");
					++summary.models;
				}
			}
			// The crystal looks like the Worldstone Shard it is built from. Its picture is that
			// shard's, read from the mod or the game and tinted purple; on the ground it is the
			// shard's own model. When any of that cannot be read it simply shares the shard's look.
			std::string_view rawAsset;
			const auto       shard = byCode.find(kCrystalTemplates[0]);
			if (!crystalCode.empty() && byCode.count(crystalCode) == 0 && shard != byCode.end() && ObjectValue(shard->second, "asset", rawAsset)) {
				const std::string asset(UnquoteView(rawAsset));
				const size_t      slash = asset.rfind('/');
				const std::string own   = (slash != std::string::npos ? asset.substr(0, slash + 1) : std::string()) + kCrystalAsset;
				const std::string files[3][2] = { { std::string(kMiscSprites) + asset + ".sprite", std::string(kMiscSprites) + own + ".sprite" },
					                              { std::string(kMiscModels) + asset + ".json", std::string(kMiscModels) + own + ".json" },
					                              { std::string(kMiscSprites) + asset + ".lowend.sprite", std::string(kMiscSprites) + own + ".lowend.sprite" } };
				bool tinted = false;
				if (io.HasCopy(files[0][1]) && io.HasCopy(files[1][1])) {
					// Made by an earlier start: the game's art is not read again.
					tinted = true;
					for (const auto& file : files) {
						if (io.HasCopy(file[1])) {
							keptFiles.push_back(file[1]);
						}
					}
				} else {
					std::string picture, model, small;
					if (ReadAny(io, files[0][0], picture) && TintSprite(picture) && ReadAny(io, files[1][0], model) && !model.empty()) {
						tinted = true;
						extraFiles.emplace_back(files[0][1], std::move(picture));
						extraFiles.emplace_back(files[1][1], std::move(model));
						if (ReadAny(io, files[2][0], small) && TintSprite(small)) {
							extraFiles.emplace_back(files[2][1], std::move(small));
						}
					} else {
						io.Log("the Worldstone Shard's picture could not be read: the Restoration Crystal looks like that shard, untinted");
					}
				}
				models.array.elements.push_back("{\"" + crystalCode + "\":{\"asset\":\"" + (tinted ? own : asset) + "\"}}");
				summary.crystalTinted = tinted;
			}
		} else {
			io.Log("hd/items/items.json could not be read: exalted bases may have no model on the ground");
		}

		// The crystal's slot in the stash's stacking tabs, in the layout for mouse and keyboard
		// and in the one for a controller.
		if (!crystalCode.empty() && config.crystalStashSlot) {
			for (const char* rel : kStashLayouts) {
				std::string text;
				if (!ReadAny(io, rel, text) || text.empty()) {
					Logf(io, "%s could not be read: no stash slot for the Restoration Crystal there", rel);
					continue;
				}
				const size_t bom  = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF ? 3 : 0;
				std::string  body = text.substr(bom);
				SlotRect     place;
				bool         framed = false;
				// The frame around the slot. The game's slots have theirs painted into the stash
				// background, so the new slot gets a picture of its own behind it: the frame of
				// the slot it is modelled on, cut out of that background.
				const FrameMaker frame = [&](const std::string& container, const SlotRect& model, const SlotRect& placed) -> std::string {
					// Which background the container is shown on, and where that picture's corner is.
					static const char* const kContainers[] = { "advancedstash_gems", "advancedstash_materials", "advancedstash_runes" };
					int                      tab           = -1;
					for (int i = 0; i < 3; ++i) {
						tab = container == kContainers[i] ? i + 2 : tab;
					}
					std::string_view rawFields, rawFiles;
					JsonArray        files;
					if (tab < 0 || !ObjectValue(body, "fields", rawFields) || rawFields.empty()) {
						return {};
					}
					const std::string panelFields(rawFields);
					if (!ObjectValue(panelFields, "backgroundFile", rawFiles) || rawFiles.empty()) {
						return {};
					}
					files.Parse(std::string(rawFiles));
					if (!files.valid || files.elements.size() <= static_cast<size_t>(tab)) {
						return {};
					}
					const std::string background(UnquoteView(files.elements[static_cast<size_t>(tab)])); // as written: "PANEL\\Stash\\Name" or "Controller/Panel/Stash/V2/Name"
					const auto        asPath  = [](std::string name) {
						std::string path;
						for (size_t i = 0; i < name.size(); ++i) {
							if (name[i] == '\\') {
								path += '/';
								i += i + 1 < name.size() && name[i + 1] == '\\' ? 1 : 0;
							} else {
								path += name[i];
							}
						}
						return std::string("hd/global/ui/") + Lower(path);
					};
					// The background widget's place: in this layout, or in the one it is based on.
					int  backX = 0, backY = 0;
					bool known = false;
					const auto backgroundPlace = [&](const std::string& layout) {
						std::string_view rawChildren;
						JsonArray        children;
						if (!ObjectValue(layout, "children", rawChildren) || rawChildren.empty()) {
							return;
						}
						children.Parse(std::string(rawChildren));
						for (const std::string& child : children.elements) {
							std::string_view name, fields, rect;
							if (!ObjectValue(child, "name", name) || UnquoteView(name) != "background" || !ObjectValue(child, "fields", fields) || fields.empty()) {
								continue;
							}
							const std::string fieldText(fields);
							if (ObjectValue(fieldText, "rect", rect) && !rect.empty() && rect.front() == '{') {
								std::vector<Member> members;
								ObjectMembers(std::string(rect), members);
								for (const Member& member : members) {
									if (member.key == "x") {
										backX = ToInt(member.value);
									} else if (member.key == "y") {
										backY = ToInt(member.value);
									}
								}
								known = true;
							}
						}
					};
					backgroundPlace(body);
					if (!known) {
						std::string_view rawBase;
						std::string      baseText;
						if (ObjectValue(body, "basedOn", rawBase) && !rawBase.empty() && ReadAny(io, std::string("global/ui/layouts/") + Lower(std::string(UnquoteView(rawBase))), baseText)) {
							backgroundPlace(baseText.substr(baseText.size() >= 3 && static_cast<unsigned char>(baseText[0]) == 0xEF ? 3 : 0));
						}
					}
					if (!known) {
						return {};
					}
					// The model slot with its border, on even pixels (the small picture is half the
					// size). A painted frame starts a pixel or two before the slot's corner and
					// ends inside the slot's size; the next slot's frame begins right after it.
					constexpr int kBorder = 2;
					const int     x0 = (model.x - backX - kBorder) & ~1, y0 = (model.y - backY - kBorder) & ~1;
					const int     x1 = (model.x - backX + model.w) & ~1, y1 = (model.y - backY + model.h) & ~1;
					// Painted into a copy of the background, where the game's own frames are. (A
					// picture widget beside the slots is not possible: the game takes every widget
					// in that container for a slot and fails on anything else.)
					const std::string big = asPath(background) + ".sprite", small = asPath(background) + ".lowend.sprite";
					const int         dx = placed.x - model.x, dy = placed.y - model.y;
					if (io.HasCopy(big)) {
						keptFiles.push_back(big);
						if (io.HasCopy(small)) {
							keptFiles.push_back(small);
						}
					} else {
						std::string picture;
						if (!ReadAny(io, big, picture) || !CopyInSprite(picture, x0, y0, x1, y1, x0 + dx, y0 + dy)) {
							return {};
						}
						extraFiles.emplace_back(big, std::move(picture));
						if (ReadAny(io, small, picture) && CopyInSprite(picture, x0 / 2, y0 / 2, x1 / 2, y1 / 2, (x0 + dx) / 2, (y0 + dy) / 2)) {
							extraFiles.emplace_back(small, std::move(picture));
						}
					}
					framed = true;
					return {};
				};
				switch (AddStashSlot(body, crystalCode, place, frame)) {
				case SlotResult::Added:
					extraFiles.emplace_back(rel, text.substr(0, bom) + body);
					++summary.crystalSlots;
					Logf(io, "%s: stash slot for the Restoration Crystal at x %d, y %d (%d x %d), %s", rel, place.x, place.y, place.w, place.h,
					     framed ? "its frame painted into a copy of the stash background" : "without a frame (the stash background could not be read)");
					break;
				case SlotResult::Present:
					++summary.crystalSlots;
					Logf(io, "%s: already has a stash slot for the Restoration Crystal; left as it is", rel);
					break;
				case SlotResult::NoRoom:
					Logf(io, "%s: no free place beside the Worldstone Shards' slots: no stash slot for the Restoration Crystal there", rel);
					break;
				default:
					Logf(io, "%s: no slot of a Worldstone Shard or essence found: no stash slot for the Restoration Crystal there", rel);
					break;
				}
			}
			if (summary.crystalSlots == 0) {
				io.Log("the Restoration Crystal has no slot in the stash's stacking tabs: it is kept as an ordinary item");
			}
		}

		// 5. Rows, families, recipes and loot rows for each bank.
		for (Bank& bank : banks) {
			AddItems(bank, bank.weapons, config, codes, nameKeys);
			AddItems(bank, bank.armor, config, codes, nameKeys);
			FindFamilies(io, bank, false, config.uniqueTiers, codes);
			FindFamilies(io, bank, true, config.setTiers, codes);
			int uniques = 0, sets = 0;
			for (Family& family : bank.families) {
				(family.sets ? sets : uniques) += 1;
				summary.families.push_back(family);
			}
			if (!crystalCode.empty()) {
				AddCrystal(io, bank, crystalCode, crystalNameKey, summary.crystalSlots > 0);
				if (config.restoreEnabled && !bank.crystalCode.empty()) {
					AddMarker(io, bank, config);
				}
			}
			AddRecipes(io, bank, config);
			AddDowngrades(io, bank, config, codes);
			AddRestores(io, bank, config, codes);
			ExtendLadders(io, bank, config);
			AddCrystalDrops(io, bank, config);
			summary.restores = std::max(summary.restores, bank.restoresAdded);
			if (bank.index == 0 || summary.crystalRows == 0) {
				summary.crystalRows = bank.crystalRows;
			}
			if (!bank.crystalCode.empty()) {
				Logf(io, "%s: Restoration Crystal '%s', %d restore recipes, listed in %d boss loot rows", bank.dir.c_str(), bank.crystalCode.c_str(), bank.restoresAdded, bank.crystalRows);
			}
			summary.uniques = std::max(summary.uniques, uniques);
			summary.sets    = std::max(summary.sets, sets);
			summary.recipes = std::max(summary.recipes, bank.recipesAdded);
			summary.downgrades = std::max(summary.downgrades, bank.downgradesAdded);
			if (bank.index == 0 || summary.levelHigh == 0) {
				summary.levelLow  = bank.levelLow;
				summary.levelHigh = bank.levelHigh;
				summary.rungs     = bank.rungs;
				summary.repointed = bank.repointed;
				summary.zoneRows  = bank.zoneRows;
			}
			Logf(io, "%s: %d weapon and %d armor bases at item levels %d to %d, %d with unique drops, %d with set drops, %d recipes, %d loot rows added%s", bank.dir.c_str(),
			     static_cast<int>(bank.weapons.families.size()), static_cast<int>(bank.armor.families.size()), bank.levelLow, bank.levelHigh, uniques, sets, bank.recipesAdded, bank.rungs,
			     config.terrorZoneOnly ? "; exalted bases drop only in terror zones" : "");
		}
	}

	// 6. Hand over every file that differs from the mod's own. Nothing is stored until
	//    everything above has worked. Unique and set tables are never among them.
	std::vector<std::pair<std::string, std::string>> writes;
	for (const Bank& bank : banks) {
		if (bank.cube.readPastSplit) {
			summary.error = "internal error: " + bank.cube.rel + " was read past the columns that are taken apart";
			return summary;
		}
	}
	for (Bank& bank : banks) {
		for (Table* table : { &bank.weapons.table, &bank.armor.table, &bank.cube, &bank.treasure, &bank.miscTable, &bank.stats, &bank.props }) {
			if (table->loaded) {
				std::string text = table->Serialize();
				if (text != table->original) {
					writes.emplace_back(table->rel, std::move(text));
				}
			}
		}
	}
	for (JsonFile* file : { &names, &models }) {
		if (file->loaded) {
			std::string text = file->array.Serialize();
			if (text != file->original) {
				writes.emplace_back(file->rel, std::move(text));
			}
		}
	}
	for (auto& extra : extraFiles) {
		writes.push_back(std::move(extra));
	}
	for (const auto& write : writes) {
		if (!io.Write(write.first, write.second)) {
			summary.error = "could not write " + write.first;
			return summary;
		}
		++summary.filesWritten;
		summary.files.push_back(write.first);
	}
	for (const std::string& kept : keptFiles) {
		summary.files.push_back(kept);
	}
	summary.ok = true;
	return summary;
}

#ifdef EXALTED_SELF_TEST
// Checks of the table and JSON reading against the plain way of doing the same (test/core_test.cpp).
// `text` is the content of a tab-separated table or of a JSON string file; the result is the
// number of differences found, each one reported through `report`.
int SelfTestTable(const std::string& text, void (*report)(const char*)) {
	int  failures = 0;
	char line[200];
	auto fail = [&](const char* what, size_t row, int col) {
		snprintf(line, sizeof(line), "%s (row %zu, column %d)", what, row, col);
		report(line);
		++failures;
	};
	Table whole;
	whole.Parse(std::string(text));
	RoTable view;
	view.Parse(std::string(text));
	if (whole.loaded != view.loaded || whole.header != view.header || whole.lowerHeader != view.lowerHeader || whole.rows.size() != view.rows.size()) {
		fail("the read-only table differs in header or row count", 0, 0);
		return failures;
	}
	if (whole.loaded && whole.Serialize() != text) {
		fail("a table that was only read does not come out as it went in", 0, 0);
	}
	const int columns = static_cast<int>(whole.header.size());
	for (size_t r = 0; r < whole.rows.size() && failures < 20; ++r) {
		const int cells = static_cast<int>(whole.rows[r].size());
		if (static_cast<int>(view.rows[r].count) != cells) {
			fail("cell count differs", r, cells);
			continue;
		}
		for (int col = -1; col <= std::max(cells, columns) + 1; ++col) {
			const std::string expected = whole.Get(whole.rows[r], col);
			const auto        got      = view.Get(view.rows[r], col);
			if (got != expected) {
				fail("cell differs", r, col);
			}
			if (ToInt(got) != ToInt(expected) || std::string(TrimView(got)) != Trim(expected)) {
				fail("number or trimmed text differs", r, col);
			}
		}
	}
	for (const std::string& name : whole.header) {
		std::string plain = name;
		if (!plain.empty() && plain.back() == '\r') {
			plain.pop_back();
		}
		if (whole.Col(plain.c_str()) != view.Col(plain.c_str())) {
			fail("column lookup differs", 0, whole.Col(plain.c_str()));
		}
	}
	// Rows taken apart only through one column: the same cells up to there, the same text out.
	for (int through = 0; through < columns && through < 6 && failures < 20; ++through) {
		std::string name = whole.header[static_cast<size_t>(through)];
		if (!name.empty() && name.back() == '\r') {
			name.pop_back();
		}
		if (name.empty() || whole.Col(name.c_str()) != through) {
			continue; // a name that is there twice, or not at all
		}
		Table part;
		part.Parse(std::string(text), name.c_str());
		if (part.splitThrough != through || part.rows.size() != whole.rows.size() || part.header != whole.header) {
			fail("the partly read table differs in shape", 0, through);
			continue;
		}
		if (part.Serialize() != text) {
			fail("the partly read table does not come out as it went in", 0, through);
		}
		for (size_t r = 0; r < whole.rows.size(); ++r) {
			for (int col = 0; col <= through; ++col) {
				if (part.Get(part.rows[r], col) != whole.Get(whole.rows[r], col) && static_cast<size_t>(col) + 1 != whole.rows[r].size()) {
					fail("a cell of the partly read table differs", r, col);
				}
			}
		}
		if (part.readPastSplit) {
			fail("reading up to the split was taken for reading past it", 0, through);
		}
		if (!part.rows.empty() && static_cast<int>(part.rows[0].size()) < columns && columns > through + 1) {
			static_cast<void>(part.Get(part.rows[0], through + 1));
			if (!part.readPastSplit) {
				fail("reading past the split went unnoticed", 0, through);
			}
		}
		// A row added later is whole and can be written anywhere.
		Table added;
		added.Parse(std::string(text), name.c_str());
		std::vector<std::string> fresh = added.NewRow();
		added.Set(fresh, columns - 1, "x");
		if (added.readPastSplit) {
			fail("writing a new row was taken for reading past the split", 0, through);
		}
	}
	return failures;
}

int SelfTestJson(const std::string& text, void (*report)(const char*)) {
	int       failures = 0;
	JsonArray array;
	array.Parse(text);
	if (!array.valid) {
		return 0;
	}
	if (array.Serialize() != text) {
		report("a JSON file that was only read does not come out as it went in");
		++failures;
	}
	long                highest = std::max(MaxStringId(array.prefix), MaxStringId(array.suffix));
	std::vector<Member> members;
	for (const std::string& element : array.elements) {
		highest = std::max(highest, MaxStringId(element));
		for (const char* key : { "Key", "id", "enUS", "missing" }) {
			std::string_view  got;
			const bool        ok       = ObjectValue(element, key, got);
			const bool        expected = ObjectMembers(element, members);
			std::string       value;
			for (const Member& member : members) {
				if (member.key == key) {
					value = member.value;
					break;
				}
			}
			if (ok != expected || (ok && got != value)) {
				report("a member read on its own differs from the member list");
				++failures;
			}
		}
	}
	if (highest != MaxStringId(text)) {
		report("the highest id found piece by piece differs from the one found in the whole text");
		++failures;
	}
	return failures;
}
#endif

}
