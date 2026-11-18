// Stand-in loader: loads the plugin DLL the way D2RLoader does, without the game, to check
// the DLL end to end (run under Wine):
//
//   fake_host.exe <plugin dll> <game dir> <mod name> <config toml> <image dump | search:<image dump> | none> [exalted name]
//
// With an image dump the stand-in image also gets the dump's header stamp, so the plugin sees
// "the game image I know" and confirms the pickers where it knows them to be. "search:" leaves
// the stamp out, which makes the plugin search the image as it does for any other game build.
//
// <game dir>\mods\<mod>\<mod>.mpq\data must hold a copy of a mod's data files.
//
// What it stands in for:
//  * the loader's plugin API (log, config, inline hooks, expected-byte checks);
//  * a game image: an empty block of memory into which the two picker routines are copied from
//    a real image dump at their real offsets, with their calls to the item row and item level
//    lookups pointed at functions of this program;
//  * the game's item table: every item of the (adjusted) weapons, armor and misc tables.
// Then it presses the plugin's hooks with made-up items and reports which base each pick saw.
#include <D2RLPlugin/api.h>

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kSetPick = 0x58c2f0, kUniquePick = 0x443910; // in the dump
constexpr uint64_t kRowThunk = 0x314110, kLevelThunk = 0x36ccf0; // where the game's own lookups are in the dump
constexpr size_t   kImageSize = 0x600000, kRowSize = 0x1c0, kCodeAt = 0x80, kVersionAt = 0x106;

uint8_t*    g_image = nullptr;
std::string g_config;
int         g_failures = 0;

struct Hooked {
	uint64_t rva;
	void*    target;
};
std::vector<Hooked>      g_hooks;
std::vector<uint8_t>     g_rows;      // fake item rows
std::vector<std::string> g_codes;     // item number -> code
int                      g_itemLevel = 1;
uint32_t                 g_seenClass = 0; // the item number the game's picker saw
int                      g_originalCalls = 0;
bool                     g_pickNothing   = false; // the game's picker finds nothing (as when a unique has already dropped)
int                      g_consoleLines  = 0, g_missLines = 0, g_setMissLogs = 0;
D2RL::ConsoleCommandCallback g_console         = nullptr;
void*                        g_consoleUserData = nullptr;

// --- stand-ins for "take a restore off" (plugin.cpp, restore_strip.h) -------------------------
// The game's property routine and its helpers, at the places the plugin knows them: each
// starts with the game's own first bytes (the plugin checks those) and then leaves for a
// function of this program. Property rows come from the plugin's stored tables.
constexpr uint64_t kApplyProperty = 0x3d4620, kItemVersion = 0x34a0e0, kPropertyRow = 0x3d3210, kPropertyCount = 0x3d32d0, kUnitStat = 0x2f5020;
struct PropertyCall {
	int32_t id, param, min, max;
};
struct ModelItem {
	std::map<int, int> stats;
};
std::vector<std::vector<uint8_t>> g_propertyRows;
int                               g_applyCalls = 0, g_applyBadArgs = 0;

uint8_t        HostItemVersion(void*) { return 3; }
const uint8_t* HostPropertyRow(uint8_t, int id) { return id >= 0 && id < static_cast<int>(g_propertyRows.size()) ? g_propertyRows[static_cast<size_t>(id)].data() : nullptr; }
int            HostPropertyCount(uint8_t) { return static_cast<int>(g_propertyRows.size()); }
int32_t        HostUnitStat(void* unit, uint32_t stat, uint16_t) {
	const auto& stats = static_cast<ModelItem*>(unit)->stats;
	const auto  found = stats.find(static_cast<int>(stat));
	return found == stats.end() ? 0 : found->second;
}
// The game's routine in small: the first function of the row rolls, function 3 repeats the amount.
uint64_t ModelApply(int kind, void* item, void* third, void* fourth, int index, int sixth, PropertyCall* call, int state, int flags, void* tenth) {
	++g_applyCalls;
	if (kind != 6 || third != reinterpret_cast<void*>(0x1111) || fourth != reinterpret_cast<void*>(0x2222) || index != 3 || sixth != 4 || state != 0xab || flags != 0x40 || tenth != reinterpret_cast<void*>(0x3333)) {
		++g_applyBadArgs;
	}
	const uint8_t* row = HostPropertyRow(3, call->id);
	if (row == nullptr) return 0;
	auto&     stats  = static_cast<ModelItem*>(item)->stats;
	const int amount = call->min + (call->max > call->min ? rand() % (call->max - call->min + 1) : 0);
	for (int slot = 0; slot < 7; ++slot) {
		int16_t stat;
		memcpy(&stat, row + 0x0a + slot * 2, 2);
		const int func = row[0x18 + slot];
		if (func == 7) {
			stats[21] += amount;
			stats[22] += amount;
		} else if (func != 0 && stat >= 0) {
			stats[stat] += amount;
		}
	}
	for (auto it = stats.begin(); it != stats.end();) it = it->second == 0 ? stats.erase(it) : std::next(it);
	return static_cast<uint64_t>(amount);
}

void Fail(const char* what) {
	printf("FAIL: %s\n", what);
	++g_failures;
}

std::wstring Wide(const std::string& s) {
	return std::wstring(s.begin(), s.end());
}

bool ReadAll(const std::wstring& path, std::string& out) {
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size {};
	GetFileSizeEx(file, &size);
	out.resize(static_cast<size_t>(size.QuadPart));
	DWORD read = 0;
	const bool ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 && read == out.size());
	CloseHandle(file);
	return ok;
}

// --- the loader's API ----------------------------------------------------------------------

void __cdecl Log(const D2RL::PluginContext*, const char* message) noexcept {
	printf("  [plugin] %s\n", message);
	if (strstr(message, "the game picked no set piece") != nullptr) ++g_setMissLogs;
}
bool __cdecl EnsureConfig(const D2RL::PluginContext*, const char*) noexcept {
	return true;
}
bool __cdecl ReadConfig(const D2RL::PluginContext*, char* output, uint32_t outputSize, uint32_t* requiredSize) noexcept {
	if (requiredSize != nullptr) *requiredSize = static_cast<uint32_t>(g_config.size() + 1);
	if (output == nullptr || outputSize <= g_config.size()) return false;
	memcpy(output, g_config.c_str(), g_config.size() + 1);
	return true;
}
bool __cdecl WriteConfig(const D2RL::PluginContext*, const char* text) noexcept {
	g_config = text != nullptr ? text : "";
	return true;
}
bool __cdecl PatchBytes(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, const void*, uint32_t) noexcept {
	return false;
}
bool __cdecl PatchFill(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, uint8_t, uint32_t) noexcept {
	return false;
}
bool __cdecl PatchWrite(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, uint64_t, uint32_t) noexcept {
	return false;
}
bool __cdecl PatchRel32(const D2RL::PluginContext*, uint64_t, const void*, uint32_t, uint64_t, uint32_t, D2RL::Rel32PatchKind) noexcept {
	return false;
}
bool __cdecl CheckExpectedBytes(const D2RL::PluginContext*, uint64_t rva, const void* expected, uint32_t size) noexcept {
	return g_image != nullptr && rva + size <= kImageSize && memcmp(g_image + rva, expected, size) == 0;
}
bool __cdecl RegisterConsoleCommand(const D2RL::PluginContext*, const D2RL::ConsoleCommandRegistration* command) noexcept {
	if (command != nullptr) {
		g_console         = command->callback;
		g_consoleUserData = command->userData;
	}
	return true;
}
void __cdecl WriteConsoleMessage(const D2RL::PluginContext*, const char* message, D2RL::ConsoleMessageKind) noexcept {
	printf("  [console] %.300s\n", message);
	++g_consoleLines;
	if (strstr(message, "last roll the game picked nothing for") != nullptr) ++g_missLines;
}
D2RL::ServiceQueryResult __cdecl QueryService(const D2RL::PluginContext*, D2RL::ServiceId, uint32_t, const void** service) noexcept {
	if (service != nullptr) *service = nullptr;
	return D2RL::ServiceQueryResult::UnknownService;
}

// What the game's own picker does in this test: note which base the item shows, say "picked".
uint64_t OriginalPick(void*, void* item, void*) {
	g_seenClass = static_cast<uint32_t*>(item)[1];
	++g_originalCalls;
	return g_pickNothing ? 0 : 1;
}

bool __cdecl InstallInlineHook(const D2RL::PluginContext*, const D2RL::InlineHookRegistration* hook) noexcept {
	if (hook == nullptr || hook->target == nullptr || hook->rva + hook->expectedSize > kImageSize || memcmp(g_image + hook->rva, hook->expected, hook->expectedSize) != 0) {
		printf("  [host] hook refused (bytes do not match)\n");
		return false;
	}
	g_hooks.push_back({ hook->rva, hook->target });
	if (hook->original != nullptr) *hook->original = hook->rva == kApplyProperty ? reinterpret_cast<void*>(&ModelApply) : reinterpret_cast<void*>(&OriginalPick);
	printf("  [host] hook installed at 0x%llx (%u entry bytes)\n", static_cast<unsigned long long>(hook->rva), hook->expectedSize);
	return true;
}

// --- the game's lookups ----------------------------------------------------------------------

const void* ItemRow(uint8_t, uint32_t number) {
	return number < g_codes.size() ? g_rows.data() + number * kRowSize : nullptr;
}
int32_t ItemLevel(void*) {
	return g_itemLevel;
}

void Thunk(uint64_t rva, void* function) { // mov rax, function; jmp rax
	uint8_t* p = g_image + rva;
	p[0]       = 0x48;
	p[1]       = 0xb8;
	memcpy(p + 2, &function, 8);
	p[10] = 0xff;
	p[11] = 0xe0;
}
void PointCall(uint64_t callRva, uint64_t targetRva) {
	const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(targetRva) - static_cast<int64_t>(callRva + 5));
	memcpy(g_image + callRva + 1, &rel, 4);
}

bool BuildImage(const char* dumpPath) {
	g_image = static_cast<uint8_t*>(VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (g_image == nullptr) return false;
	if (strcmp(dumpPath, "none") == 0) return true; // an image without the pickers
	const bool withStamp = strncmp(dumpPath, "search:", 7) != 0;
	if (!withStamp) dumpPath += 7;
	FILE* f = fopen(dumpPath, "rb");
	if (f == nullptr) return false;
	if (withStamp) {
		// The start of a program header, with the dump's build stamp in it.
		IMAGE_DOS_HEADER  dos {};
		IMAGE_NT_HEADERS64 nt {};
		if (fread(&dos, sizeof(dos), 1, f) != 1 || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0xE00) return false;
		fseek(f, dos.e_lfanew, SEEK_SET);
		if (fread(&nt, sizeof(nt), 1, f) != 1 || nt.Signature != IMAGE_NT_SIGNATURE) return false;
		IMAGE_DOS_HEADER  ownDos {};
		IMAGE_NT_HEADERS64 ownNt {};
		ownDos.e_magic                = IMAGE_DOS_SIGNATURE;
		ownDos.e_lfanew               = 0x100;
		ownNt.Signature               = IMAGE_NT_SIGNATURE;
		ownNt.FileHeader.TimeDateStamp = nt.FileHeader.TimeDateStamp;
		memcpy(g_image, &ownDos, sizeof(ownDos));
		memcpy(g_image + 0x100, &ownNt, sizeof(ownNt));
		printf("  [host] stand-in image carries build stamp %08lx\n", static_cast<unsigned long>(nt.FileHeader.TimeDateStamp));
	}
	for (const auto& part : { std::pair<uint64_t, size_t> { kSetPick, 0x400 }, std::pair<uint64_t, size_t> { kUniquePick, 0x800 } }) {
		fseek(f, static_cast<long>(part.first), SEEK_SET);
		if (fread(g_image + part.first, 1, part.second, f) != part.second) return false;
	}
	fclose(f);
	Thunk(kRowThunk, reinterpret_cast<void*>(&ItemRow));
	Thunk(kLevelThunk, reinterpret_cast<void*>(&ItemLevel));
	PointCall(kSetPick + 94, kRowThunk);
	PointCall(kSetPick + 140, kLevelThunk);
	PointCall(kUniquePick + 64, kRowThunk);

	// "Take a restore off": the first bytes of the game's routines, then a way out to this program.
	const auto put = [](uint64_t rva, std::initializer_list<uint8_t> bytes) {
		memcpy(g_image + rva, bytes.begin(), bytes.size());
		return rva + bytes.size();
	};
	f = fopen(dumpPath, "rb");
	if (f == nullptr) return false;
	fseek(f, static_cast<long>(kApplyProperty), SEEK_SET);
	const bool entry = fread(g_image + kApplyProperty, 1, 23, f) == 23; // never run here: the host hands the plugin ModelApply as "the original"
	fclose(f);
	if (!entry) return false;
	put(kItemVersion, { 0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xc9, 0x75, 0x1a });                  // sub rsp,28h; test rcx,rcx; jne +1Ah
	Thunk(put(kItemVersion + 9 + 0x1a, { 0x48, 0x83, 0xc4, 0x28 }), reinterpret_cast<void*>(&HostItemVersion)); // add rsp,28h; leave
	Thunk(put(put(kPropertyRow, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x63, 0xda }),
	          { 0x48, 0x83, 0xc4, 0x30, 0x5f, 0x48, 0x8b, 0x5c, 0x24, 0x08 }),                         // add rsp,30h; pop rdi; mov rbx,[rsp+8]; leave
	      reinterpret_cast<void*>(&HostPropertyRow));
	put(kPropertyCount, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xe8, 0, 0, 0, 0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3 }); // push rbx; sub rsp,20h; call ...; add rsp,20h; pop rbx; ret
	PointCall(kPropertyCount + 6, kPropertyCount + 0x40);
	Thunk(kPropertyCount + 0x40, reinterpret_cast<void*>(&HostPropertyCount));
	put(kUnitStat, { 0xff, 0x25, 0x10, 0, 0, 0 });                                                 // jmp [rip+10h]
	void* stat = reinterpret_cast<void*>(&HostUnitStat);
	memcpy(g_image + kUnitStat + 6 + 0x10, &stat, 8);
	return true;
}

// Codes of a table, in row order (the "Expansion" divider is not a row).
void AddItems(const std::wstring& path, std::map<std::string, std::string>* names) {
	std::string text;
	if (!ReadAll(path, text)) {
		printf("  [host] cannot read %ls\n", path.c_str());
		return;
	}
	size_t pos = 0, line = 0;
	int    codeCol = -1, nameCol = -1;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::vector<std::string> cells;
		size_t                   start = pos;
		for (size_t i = pos; i <= end; ++i) {
			if (i == end || text[i] == '\t') {
				std::string cell = text.substr(start, i - start);
				if (!cell.empty() && cell.back() == '\r') cell.pop_back();
				cells.push_back(cell);
				start = i + 1;
			}
		}
		if (line == 0) {
			for (size_t i = 0; i < cells.size(); ++i) {
				if (cells[i] == "code") codeCol = static_cast<int>(i);
				if (cells[i] == "name") nameCol = static_cast<int>(i);
			}
		} else if (codeCol >= 0 && static_cast<size_t>(codeCol) < cells.size() && !cells[static_cast<size_t>(codeCol)].empty()) {
			g_codes.push_back(cells[static_cast<size_t>(codeCol)]);
			if (names != nullptr && nameCol >= 0) (*names)[cells[static_cast<size_t>(nameCol)]] = g_codes.back();
		}
		++line;
		pos = end + 1;
	}
}

uint32_t NumberOf(const std::string& code) {
	for (size_t i = 0; i < g_codes.size(); ++i) {
		if (g_codes[i] == code) return static_cast<uint32_t>(i);
	}
	return 0xFFFFFFFFu;
}

using PickFn = uint64_t (*)(void* game, void* item, void* drop);

void Press(const char* what, PickFn hook, uint32_t number, int level, int times) {
	std::vector<uint8_t> game(0x200, 0);
	game[kVersionAt] = 2;
	uint32_t item[8] = { 4, number, 0, 0, 0, 0, 0, 0 };
	std::map<std::string, int> seen;
	g_itemLevel = level;
	for (int i = 0; i < times; ++i) {
		g_seenClass = 0xFFFFFFFFu;
		hook(game.data(), item, nullptr);
		if (item[1] != number) {
			Fail("the item's base was not put back after the pick");
			return;
		}
		seen[g_seenClass < g_codes.size() ? g_codes[g_seenClass] : "?"] += 1;
	}
	printf("  %-7s item level %3d: the game's picker saw", what, level);
	for (const auto& entry : seen) printf("  %s %.1f%%", entry.first.c_str(), 100.0 * entry.second / times);
	printf("\n");
}

}

int main(int argc, char** argv) {
	if (argc < 6) {
		printf("usage: fake_host <plugin dll> <game dir> <mod name> <config toml> <image dump | none> [exalted name]\n");
		return 2;
	}
	const std::string gameDir = argv[2], mod = argv[3];
	const char*       wanted  = argc > 6 ? argv[6] : "[xlt] Shako";
	if (!ReadAll(Wide(argv[4]), g_config)) {
		printf("cannot read the config %s\n", argv[4]);
		return 2;
	}
	if (!BuildImage(argv[5])) {
		printf("cannot build the stand-in game image from %s\n", argv[5]);
		return 2;
	}
	const std::wstring modDir  = Wide(gameDir + "\\mods\\" + mod);
	// FAKE_SCOPE=global: the plugin is installed for every mod (<game>\d2rloader\plugins), so its
	// files belong in <game>\d2rloader\exalted-items\<mod>. FAKE_MPQ=<name>: the mod's .mpq folder
	// has a name of its own.
	const char*        scopeEnv  = getenv("FAKE_SCOPE");
	const char*        mpqEnv    = getenv("FAKE_MPQ");
	const bool         global    = scopeEnv != nullptr && strcmp(scopeEnv, "global") == 0;
	const std::wstring dataDir   = modDir + L"\\" + Wide(mpqEnv != nullptr && mpqEnv[0] != 0 ? std::string(mpqEnv) : mod + ".mpq") + L"\\data";
	const std::wstring storeDir  = global ? Wide(gameDir + "\\d2rloader\\exalted-items\\" + mod) : modDir + L"\\d2rloader\\exalted-items";
	const std::wstring genDir    = storeDir + L"\\data";
	const std::wstring pluginDir = Wide(gameDir + "\\d2rloader\\plugins");

	static D2RL::PluginApi api {};
	api.apiSize                = D2RL::PluginApiSize;
	api.logInfo = api.logWarn = api.logError = Log;
	api.ensureConfig           = EnsureConfig;
	api.readConfig             = ReadConfig;
	api.writeConfig            = WriteConfig;
	api.patchBytes             = PatchBytes;
	api.patchFill              = PatchFill;
	api.patchWrite             = PatchWrite;
	api.patchRel32             = PatchRel32;
	api.checkExpectedBytes     = CheckExpectedBytes;
	api.registerConsoleCommand = RegisterConsoleCommand;
	api.writeConsoleMessage    = WriteConsoleMessage;
	api.installInlineHook      = InstallInlineHook;
	api.queryService           = QueryService;
	static D2RL::PluginContext context {};
	context.contextSize  = static_cast<uint32_t>(sizeof(context));
	context.abiVersion   = D2RL_PLUGIN_ABI_VERSION;
	context.api          = &api;
	context.loadScope    = global ? D2RL::LoadScope::Global : D2RL::LoadScope::Mod;
	if (global) {
		context.pluginDirectory = pluginDir.c_str();
	}
	context.exeBase      = reinterpret_cast<uintptr_t>(g_image);
	context.pluginId     = "exalted-items";
	context.activeMod    = mod.c_str();
	context.modDirectory = modDir.c_str();
	context.buildVersion = "stand-in";

	const HMODULE dll = LoadLibraryA(argv[1]);
	if (dll == nullptr) {
		printf("cannot load %s (error %lu)\n", argv[1], GetLastError());
		return 2;
	}
	const auto getInfo = reinterpret_cast<D2RL::GetPluginInfoFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D2RLoaderGetPluginInfo")));
	const auto load    = reinterpret_cast<D2RL::LoadPluginFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D2RLoaderLoadPlugin")));
	const auto unload  = reinterpret_cast<D2RL::UnloadPluginFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D2RLoaderUnloadPlugin")));
	if (getInfo == nullptr || load == nullptr || unload == nullptr) {
		printf("the DLL lacks a plugin export\n");
		return 2;
	}
	printf("plugin %s %s\n", getInfo()->id, getInfo()->version);
	if (!load(&context)) Fail("D2RLoaderLoadPlugin returned false");

	// 1. The file redirect: a mod file the plugin adjusted is answered with the copy; the
	//    unique and set tables are not adjusted and stay the mod's own.
	std::string viaMod, copy, uniques, uniquesCopy;
	const bool  hasCopy = ReadAll(genDir + L"\\global\\excel\\armor.txt", copy);
	ReadAll(dataDir + L"\\global\\excel\\armor.txt", viaMod);
	WIN32_FIND_DATAW found {};
	const HANDLE     find = FindFirstFileW((dataDir + L"\\global\\excel\\armor.txt").c_str(), &found);
	const size_t     onDisk = find != INVALID_HANDLE_VALUE ? found.nFileSizeLow : 0;
	if (find != INVALID_HANDLE_VALUE) FindClose(find);
	printf("armor.txt: %zu bytes on disk in the mod, %zu bytes when opened through the mod path, %zu bytes in the plugin's copy\n", onDisk, viaMod.size(), copy.size());
	if (hasCopy && viaMod != copy) Fail("opening the mod's armor.txt did not give the adjusted copy");
	if (hasCopy && viaMod.size() == onDisk) Fail("the adjusted armor.txt is the same size as the mod's (nothing was added?)");
	ReadAll(dataDir + L"\\global\\excel\\uniqueitems.txt", uniques);
	if (ReadAll(genDir + L"\\global\\excel\\uniqueitems.txt", uniquesCopy)) Fail("an adjusted uniqueitems.txt exists");
	if (uniques.find("[xlt]") != std::string::npos) Fail("uniqueitems.txt read through the mod path holds generated rows");
	if (ReadAll(genDir + L"\\global\\excel\\setitems.txt", uniquesCopy)) Fail("an adjusted setitems.txt exists");
	std::string codes;
	if (hasCopy && !ReadAll(storeDir + L"\\codes.txt", codes)) Fail("codes.txt was not written");
	printf("codes.txt: %zu bytes; hooks installed: %zu\n", codes.size(), g_hooks.size());
	int pickerHooks = 0;
	for (const Hooked& hook : g_hooks) pickerHooks += hook.rva == kSetPick || hook.rva == kUniquePick ? 1 : 0;

	// 1b. Turning a restored item back to elite: the plugin's hook on the property routine is
	//     given the sign and has to take off exactly what a restore put on.
	void* applyHook = nullptr;
	for (const Hooked& hook : g_hooks) {
		if (hook.rva == kApplyProperty) applyHook = hook.target;
	}
	if (applyHook != nullptr) {
		std::map<std::string, int> statIds, propertyIds;
		std::string                text;
		const auto lines = [](const std::string& data) {
			std::vector<std::vector<std::string>> out;
			size_t at = 0;
			while (at < data.size()) {
				size_t end = data.find('\n', at);
				if (end == std::string::npos) end = data.size();
				std::string line = data.substr(at, end - at);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				std::vector<std::string> cells;
				size_t c = 0;
				while (true) {
					const size_t tab = line.find('\t', c);
					cells.push_back(line.substr(c, tab == std::string::npos ? std::string::npos : tab - c));
					if (tab == std::string::npos) break;
					c = tab + 1;
				}
				out.push_back(cells);
				at = end + 1;
			}
			return out;
		};
		ReadAll(dataDir + L"\\global\\excel\\itemstatcost.txt", text);
		const auto statRows = lines(text);
		for (size_t i = 1; i < statRows.size(); ++i) statIds[statRows[i][0]] = static_cast<int>(i - 1);
		ReadAll(dataDir + L"\\global\\excel\\properties.txt", text);
		const auto propRows = lines(text);
		const auto column   = [&](const std::string& name) {
			for (size_t i = 0; i < propRows[0].size(); ++i) if (propRows[0][i] == name) return static_cast<int>(i);
			return -1;
		};
		g_propertyRows.clear();
		for (size_t i = 1; i < propRows.size(); ++i) {
			std::vector<uint8_t> row(0x30, 0);
			for (int n = 0; n < 7; ++n) {
				const int         cf = column("func" + std::to_string(n + 1)), cs = column("stat" + std::to_string(n + 1));
				const std::string fn = cf >= 0 && cf < static_cast<int>(propRows[i].size()) ? propRows[i][static_cast<size_t>(cf)] : "";
				const std::string st = cs >= 0 && cs < static_cast<int>(propRows[i].size()) ? propRows[i][static_cast<size_t>(cs)] : "";
				const int16_t     id = static_cast<int16_t>(statIds.count(st) ? statIds[st] : -1);
				row[0x18 + static_cast<size_t>(n)] = static_cast<uint8_t>(atoi(fn.c_str()));
				memcpy(row.data() + 0x0a + n * 2, &id, 2);
			}
			propertyIds[propRows[i][0]] = static_cast<int>(g_propertyRows.size());
			g_propertyRows.push_back(row);
		}
		using ApplyFn = uint64_t (*)(int, void*, void*, void*, int, int, PropertyCall*, int, int, void*);
		const auto hooked = reinterpret_cast<ApplyFn>(applyHook);
		const auto apply  = [&](ModelItem& item, PropertyCall call) { return hooked(6, &item, reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222), 3, 4, &call, 0xab, 0x40, reinterpret_cast<void*>(0x3333)); };
		bool ok = propertyIds.count("xlt-restored") != 0 && propertyIds.count("xlt-r0") != 0 && propertyIds.count("xlt-r12") != 0 && statIds["xlt_restored"] == 500;
		srand(11);
		for (int round = 0; ok && round < 50; ++round) {
			ModelItem item;
			item.stats = { { 39, 20 }, { 16, 150 }, { 91, -15 }, { 21, 40 }, { 22, 40 } };
			const auto before = item.stats;
			apply(item, { propertyIds["xlt-restored"], 0, 0, 0 }); // the sign on an item that was never restored
			ok = ok && item.stats == before;
			apply(item, { propertyIds["xlt-restored"], 0, 1, 1 });
			apply(item, { propertyIds[round % 2 ? "xlt-r0" : "xlt-r11"], 0, round % 2 ? 4 : 15, round % 2 ? 7 : 30 });
			apply(item, { propertyIds["xlt-r1"], 0, 30, 40 });
			apply(item, { propertyIds["xlt-r7"], 0, 5, 10 });
			apply(item, { propertyIds["xlt-r12"], 0, -10, -5 });
			ok = ok && item.stats != before && item.stats.count(500) != 0 && item.stats.count(499 - 1) != 0;
			apply(item, { propertyIds["xlt-restored"], 0, 0, 0 }); // the sign
			ok = ok && item.stats == before;
		}
		ModelItem old; // restored by a version that wrote nothing down
		old.stats = { { 500, 1 }, { 39, 6 } };
		const auto oldBefore = old.stats;
		apply(old, { propertyIds["xlt-restored"], 0, 0, 0 });
		ok = ok && old.stats == oldBefore && g_applyBadArgs == 0;
		printf("back to elite: %s (%d calls reached the game's routine, %d with changed arguments)\n", ok ? "the hook took 50 restores off, each item exactly as before" : "WRONG", g_applyCalls, g_applyBadArgs);
		if (!ok) Fail("turning a restored item back did not give the item it was before the restore");
	}

	// 2. The stand-in item table (read through the mod path, so the adjusted copies are used).
	std::map<std::string, std::string> names;
	AddItems(dataDir + L"\\global\\excel\\weapons.txt", &names);
	AddItems(dataDir + L"\\global\\excel\\armor.txt", &names);
	AddItems(dataDir + L"\\global\\excel\\misc.txt", nullptr);
	g_rows.assign(g_codes.size() * kRowSize, 0);
	for (size_t i = 0; i < g_codes.size(); ++i) {
		char code[4] = { ' ', ' ', ' ', ' ' };
		memcpy(code, g_codes[i].data(), g_codes[i].size() < 4 ? g_codes[i].size() : 4);
		memcpy(g_rows.data() + i * kRowSize + kCodeAt, code, 4);
	}
	const std::string exalted = names.count(wanted) != 0 ? names[wanted] : "";
	printf("stand-in item table: %zu items; '%s' has code '%s'\n", g_codes.size(), wanted, exalted.c_str());

	// 3. Press the hooks.
	for (const Hooked& hook : g_hooks) {
		if (hook.rva != kSetPick && hook.rva != kUniquePick) continue; // (the property routine's hook is pressed in 1b)
		const char*  what = hook.rva == kSetPick ? "set" : hook.rva == kUniquePick ? "unique" : "?";
		const PickFn pick = reinterpret_cast<PickFn>(hook.target);
		if (!exalted.empty()) {
			for (const int level : { 3, 10, 25, 40, 50, 95 }) Press(what, pick, NumberOf(exalted), level, 20000);
		}
		// an ordinary item is left alone
		const uint32_t plain = NumberOf("cap");
		std::vector<uint8_t> game(0x200, 0);
		uint32_t item[8] = { 4, plain, 0, 0, 0, 0, 0, 0 };
		g_itemLevel = 95;
		pick(game.data(), item, nullptr);
		if (g_seenClass != plain || item[1] != plain) Fail("an ordinary item was not left alone");
		// something that is not an item is left alone
		uint32_t monster[8] = { 1, NumberOf(exalted), 0, 0, 0, 0, 0, 0 };
		pick(game.data(), monster, nullptr);
		if (monster[1] != NumberOf(exalted)) Fail("a unit that is not an item was changed");
		// the game's picker finds nothing for the base posed as: the roll is reported as failed,
		// the exalted base is put back, and the base posed as is remembered for the console
		if (!exalted.empty()) {
			uint32_t missed[8] = { 4, NumberOf(exalted), 0, 0, 0, 0, 0, 0 };
			game[kVersionAt]   = 2;
			g_itemLevel        = 95;
			g_pickNothing      = true;
			for (int i = 0; i < 3; ++i) {
				if (pick(game.data(), missed, nullptr) != 0) Fail("a roll the game's picker failed was reported as made");
				if (missed[1] != NumberOf(exalted)) Fail("the item's base was not put back after a failed pick");
			}
			g_pickNothing = false;
		}
	}
	// The console command: what was generated, and what the hooks did.
	if (g_console != nullptr) {
		D2RL::ConsoleCommandContext command {};
		command.contextSize = D2RL::ConsoleCommandContextSize;
		command.input       = "exalted";
		command.name        = "exalted";
		command.args        = "";
		command.plugin      = &context;
		g_console(nullptr, &command, g_consoleUserData);
		if (g_consoleLines == 0) Fail("the console command wrote nothing");
		// 'exalted legend' writes legend.txt beside codes.txt, and replaces it the next time.
		if (hasCopy) {
			DeleteFileW((storeDir + L"\\legend.txt").c_str());
			command.input = "exalted legend";
			command.args  = " legend";
			for (int i = 0; i < 2; ++i) g_console(nullptr, &command, g_consoleUserData);
			std::string legend;
			if (!ReadAll(storeDir + L"\\legend.txt", legend)) Fail("'exalted legend' wrote no legend.txt");
			else if (legend.find("Exalted Items") != 0 || legend.find("Materials") == std::string::npos || legend.find("Body armor") == std::string::npos) Fail("legend.txt lacks its parts");
			else printf("legend.txt: %zu bytes\n", legend.size());
			command.input = "exalted";
			command.args  = "";
		}
		if (!exalted.empty() && g_missLines != pickerHooks) Fail("the console command does not show the failed picks");
		const bool setHooked = [] { for (const Hooked& hook : g_hooks) if (hook.rva == kSetPick) return true; return false; }();
		if (!exalted.empty() && g_setMissLogs != (setHooked ? 1 : 0)) Fail("a failed set pick is logged once, a failed unique pick never");
	}
	unload();
	printf("%s\n", g_failures == 0 ? "stand-in loader: ok" : "stand-in loader: FAILED");
	return g_failures == 0 ? 0 : 1;
}
