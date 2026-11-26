// Exalted Items: the "take a restore off again" logic against a small model of the game's
// property routine (the first function of a row rolls, function 3 repeats that amount for
// another stat), with property rows read from generated properties.txt / itemstatcost.txt.
//
//   strip_test <generated excel dir>
#include "../restore_strip.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace exalted;

struct Tables {
	std::vector<std::vector<uint8_t>>  rows; // compiled rows
	std::map<std::string, int>         propertyId, statId;
	std::vector<std::vector<int>>      funcs, stats;
};

std::vector<std::vector<std::string>> ReadTable(const std::string& path) {
	std::vector<std::vector<std::string>> out;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::vector<std::string> cells;
		std::stringstream s(line);
		std::string cell;
		while (std::getline(s, cell, '\t')) cells.push_back(cell);
		out.push_back(cells);
	}
	return out;
}

int Col(const std::vector<std::string>& header, const char* name) {
	for (size_t i = 0; i < header.size(); ++i) if (header[i] == name) return static_cast<int>(i);
	return -1;
}

struct Item {
	int version = 3;
	std::map<int, int> stats;
};

struct Model {
	Tables* tables;
	Item*   item;
	int     applied = 0;
	int Version(void*) { return item->version; }
	int PropertyCount(int) { return static_cast<int>(tables->rows.size()); }
	const uint8_t* Property(int, int id) { return id >= 0 && id < static_cast<int>(tables->rows.size()) ? tables->rows[static_cast<size_t>(id)].data() : nullptr; }
	int Stat(void*, int stat) { auto f = item->stats.find(stat); return f == item->stats.end() ? 0 : f->second; }
	void Apply(const PropertyCall& call) {
		++applied;
		const auto& funcs = tables->funcs[static_cast<size_t>(call.id)];
		const auto& stats = tables->stats[static_cast<size_t>(call.id)];
		const int amount = call.min + (call.max > call.min ? rand() % (call.max - call.min + 1) : 0);
		for (size_t i = 0; i < funcs.size(); ++i) {
			if (funcs[i] == 0) continue;
			if (funcs[i] == 7) { item->stats[21] += amount; item->stats[22] += amount; continue; } // damage %: two stats, none named in the row
			if (stats[i] >= 0) item->stats[stats[i]] += amount;
		}
		for (auto it = item->stats.begin(); it != item->stats.end();) it = it->second == 0 ? item->stats.erase(it) : std::next(it);
	}
};

int main(int argc, char** argv) {
	if (argc < 2) return 2;
	const std::string dir = argv[1];
	Tables tables;
	const auto stats = ReadTable(dir + "/itemstatcost.txt");
	for (size_t i = 1; i < stats.size(); ++i) tables.statId[stats[i][0]] = static_cast<int>(i - 1);
	const auto props = ReadTable(dir + "/properties.txt");
	int id = 0;
	for (size_t i = 1; i < props.size(); ++i) {
		if (props[i].empty() || props[i][0] == "Expansion") continue;
		std::vector<uint8_t> row(0x30, 0);
		std::vector<int> funcs, statIds;
		for (int n = 0; n < 7; ++n) {
			const int cf = Col(props[0], ("func" + std::to_string(n + 1)).c_str()), cs = Col(props[0], ("stat" + std::to_string(n + 1)).c_str());
			const std::string f = cf < static_cast<int>(props[i].size()) ? props[i][static_cast<size_t>(cf)] : "", s = cs < static_cast<int>(props[i].size()) ? props[i][static_cast<size_t>(cs)] : "";
			const int func = atoi(f.c_str());
			const int16_t stat = static_cast<int16_t>(tables.statId.count(s) ? tables.statId[s] : -1);
			row[kPropertyFuncs + static_cast<size_t>(n)] = static_cast<uint8_t>(func);
			memcpy(row.data() + kPropertyStats + static_cast<size_t>(n) * 2, &stat, 2);
			funcs.push_back(func);
			statIds.push_back(stat);
		}
		tables.propertyId[props[i][0]] = id++;
		tables.rows.push_back(row);
		tables.funcs.push_back(funcs);
		tables.stats.push_back(statIds);
	}
	int failed = 0;
	auto check = [&](bool ok, const char* what) { if (!ok) { printf("  FAILED: %s\n", what); ++failed; } };
	StatIds ids; // by name, as the plugin gets them from the generator
	ids.marker = tables.statId.count("xlt_restored") ? tables.statId["xlt_restored"] : -1;
	for (int k = 0; k < kRollStatCount; ++k) ids.roll[k] = tables.statId.count("xlt_roll_" + std::to_string(k)) ? tables.statId["xlt_roll_" + std::to_string(k)] : -1;
	check(ids.marker >= 0 && ids.roll[0] >= 0 && ids.roll[12] >= 0, "the marker and roll stats are there");
	const int kRestoredStatId = ids.marker;
	check(tables.propertyId.count("xlt-r0") && tables.propertyId.count("xlt-r12"), "the twin properties are there");
	const int marker = tables.propertyId["xlt-restored"];
	TwinTable twins;
	srand(7);
	for (int round = 0; round < 200; ++round) {
		// An item with attributes of its own, restored as the recipes do it, then turned back.
		Item item;
		item.stats[39] = 20; item.stats[16] = 150; item.stats[93] = 10; item.stats[91] = -15; item.stats[21] = 40; item.stats[22] = 40;
		const Item before = item;
		Model model { &tables, &item };
		PropertyCall sign { marker, 0, 0, 0 };
		check(IsStripSign(model, &item, sign, ids), "the sign is recognised");
		check(!IsStripSign(model, &item, PropertyCall { marker, 0, 1, 1 }, ids), "the marker itself is not the sign");
		check(!IsStripSign(model, &item, PropertyCall { tables.propertyId["res-all"], 0, 0, 0 }, ids), "another property with no amount is not the sign");
		check(StripRestore(model, &item, sign, twins, ids) == 0 && item.stats == before.stats && model.applied == 0, "an item that was never restored is left alone");
		model.Apply(PropertyCall { marker, 0, 1, 1 });
		const char* kinds[4][4] = { { "xlt-r0", "xlt-r1", "xlt-r2", "xlt-r12" }, { "xlt-r7", "xlt-r8", "xlt-r9", "xlt-r12" }, { "xlt-r11", "xlt-r7", "xlt-r8", "xlt-r12" }, { "xlt-r10", "xlt-r6", "xlt-r0", "xlt-r12" } };
		const int   ranges[13][2] = { { 4, 7 }, { 30, 40 }, { 3, 4 }, { 5, 10 }, { 5, 10 }, { 5, 10 }, { 5, 5 }, { 5, 10 }, { 5, 10 }, { 5, 10 }, { 5, 10 }, { 15, 30 }, { -10, -5 } };
		int used = 0;
		for (const char* code : kinds[round % 4]) {
			if (round % 7 == 3 && used == 1) { ++used; continue; } // one attribute turned off
			const int k = atoi(code + 5);
			model.Apply(PropertyCall { tables.propertyId[code], 0, ranges[k][0], ranges[k][1] });
			++used;
		}
		check(item.stats != before.stats && item.stats.count(kRestoredStatId), "the restore changed the item");
		const int taken = StripRestore(model, &item, sign, twins, ids);
		check(taken >= 3 && item.stats == before.stats, "turning it back leaves exactly the item it was before the restore");
		check(StripRestore(model, &item, sign, twins, ids) == 0, "a second time there is nothing to take off");
	}
	// Restored before the rolls were written down: marker and attributes, no hidden stats.
	Item old;
	old.stats[kRestoredStatId] = 1; old.stats[39] = 6;
	const Item oldBefore = old;
	Model oldModel { &tables, &old };
	check(StripRestore(oldModel, &old, PropertyCall { marker, 0, 0, 0 }, twins, ids) == 0 && old.stats == oldBefore.stats, "an item restored by an older version keeps its attributes and its marker");
	printf(failed == 0 ? "  strip test: 200 restored items turned back, each exactly as before its restore -> ok\n" : "  strip test: %d checks failed\n", failed);
	return failed == 0 ? 0 : 1;
}
