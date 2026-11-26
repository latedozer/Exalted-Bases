// Exalted Items: D2RLoader plugin.
//
// While the plugin loads (before the game reads its data tables) it runs the generator in
// exalted_core.cpp against the active mod's files. The mod's files are only READ. Adjusted
// copies go to a folder of the plugin's own (see FindStorage) and the game is pointed at a
// copy whenever it opens the matching mod file (see "File redirect" below).

#include <D2RLPlugin/api.h>

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <CascLib.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "exalted_core.h"
#include "game_code.h"
#include "save_check.h"
#include "redirect_match.h"
#include "restore_strip.h"

namespace {

constexpr const char* kPluginVersion = "1.3.0-pre.1";
// The build: its date and a hash of its sources (build.sh), so builds that carry the same
// version can be told apart in the log and on the console.
#ifndef EXALTED_BUILD_ID
#define EXALTED_BUILD_ID "unknown build"
#endif
constexpr const char* kBuildId = EXALTED_BUILD_ID;

const D2RL::PluginContext* g_ctx = nullptr;
char                       g_status[2000] = "Exalted Items: not run yet.";
std::vector<std::string>   g_alerts; // loud lines: on the console at the start, and again with 'exalted'
void                       Alert(const std::string& text);
// What 'exalted legend' writes: this start's bases and materials, and where the file goes
// (beside codes.txt). Empty until a start has built the tables.
exalted::Summary           g_legend;
exalted::Config            g_legendConfig;
std::wstring               g_legendPath;
exalted::Config            g_config;

void LogF(const char* format, ...) {
	char    text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (g_ctx != nullptr) {
		g_ctx->LogInfo(text);
	}
}

// Milliseconds on the high-resolution clock, for the start-up report in the log.
double Clock() {
	static LARGE_INTEGER frequency {};
	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}
	LARGE_INTEGER now {};
	QueryPerformanceCounter(&now);
	return frequency.QuadPart != 0 ? static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
}

std::wstring Widen(const std::string& text) {
	std::wstring out;
	for (const char c : text) {
		out += c == '/' ? L'\\' : static_cast<wchar_t>(static_cast<unsigned char>(c));
	}
	return out;
}

std::string Narrow(const std::wstring& text) {
	std::string out;
	const int   size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (size > 0) {
		out.resize(static_cast<size_t>(size));
		WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
	}
	return out;
}

bool IsDirectory(const std::wstring& path) {
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsFile(const std::wstring& path) {
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring GameDirectory() {
	wchar_t      path[1024] = {};
	const DWORD  length     = GetModuleFileNameW(nullptr, path, 1000);
	std::wstring out(path, length < 1000 ? length : 0);
	const size_t slash = out.find_last_of(L'\\');
	return slash != std::wstring::npos ? out.substr(0, slash) : out;
}

struct GameIo : exalted::Io {
	std::wstring dataRoot; // the active mod's "data" folder (read only)
	std::wstring genRoot;  // where adjusted copies are kept
	int          rewritten = 0;
	double       readMs    = 0; // time spent reading the mod's (and the game's) files
	double       writeMs   = 0; // time spent comparing and storing the adjusted copies
	HANDLE       storage      = nullptr;
	bool         storageTried = false;

	~GameIo() override {
		if (storage != nullptr) {
			CascCloseStorage(storage);
		}
	}

	bool ReadMod(const std::string& rel, std::string& out) override {
		const double started = Clock();
		const bool   ok      = ReadWhole(dataRoot + L"\\" + Widen(rel), out);
		readMs += Clock() - started;
		return ok;
	}

	bool ReadBase(const std::string& rel, std::string& out) override {
		const double started = Clock();
		const bool   ok      = ReadFromGame(rel, out);
		readMs += Clock() - started;
		return ok;
	}

	bool ReadFromGame(const std::string& rel, std::string& out) {
		if (!storageTried) {
			storageTried             = true;
			const std::wstring game = GameDirectory();
			if (!CascOpenStorage(game.c_str(), 0, &storage)) {
				storage = nullptr;
				LogF("could not open the game's own data in %s (error %lu): files the mod does not have cannot be filled in", Narrow(game).c_str(), GetCascError());
			}
		}
		if (storage == nullptr) {
			return false;
		}
		std::string name = "data:data\\" + rel;
		for (char& c : name) {
			if (c == '/') {
				c = '\\';
			}
		}
		HANDLE file = nullptr;
		if (!CascOpenFile(storage, name.c_str(), 0, CASC_OPEN_BY_NAME, &file)) {
			return false;
		}
		ULONGLONG size = 0;
		bool      ok   = CascGetFileSize64(file, &size) && size > 0 && size <= 256ULL * 1024 * 1024;
		if (ok) {
			out.resize(static_cast<size_t>(size));
			DWORD read = 0;
			ok         = CascReadFile(file, out.data(), static_cast<DWORD>(size), &read) && read == size;
		}
		CascCloseFile(file);
		return ok;
	}

	static bool ReadWhole(const std::wstring& path, std::string& out) {
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return false;
		}
		LARGE_INTEGER size {};
		bool          ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart <= 256LL * 1024 * 1024;
		if (ok) {
			out.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			ok         = size.QuadPart == 0 || (ReadFile(file, out.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) != 0 && read == size.QuadPart);
		}
		CloseHandle(file);
		return ok;
	}

	// Adjusted copies only ever go below genRoot. A copy that is already identical is left
	// alone, so its timestamp stays put and the loader's table cache stays valid.
	bool Write(const std::string& rel, const std::string& data) override {
		const double started = Clock();
		const bool   ok      = Store(rel, data);
		writeMs += Clock() - started;
		return ok;
	}

	bool Store(const std::string& rel, const std::string& data) {
		const std::wstring path = genRoot + L"\\" + Widen(rel);
		std::string        existing;
		if (ReadWhole(path, existing) && existing == data) {
			return true;
		}
		// Write beside the target, then swap, so a failed write never leaves half a table.
		const std::wstring temp = path + L".xlt-tmp";
		HANDLE             file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
			// The folder is not there yet (first run, or the copies were removed): make it.
			for (size_t i = 3; i < path.size(); ++i) {
				if (path[i] == L'\\') {
					CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
				}
			}
			file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		}
		if (file == INVALID_HANDLE_VALUE) {
			LogF("cannot write %s (error %lu)", Narrow(temp).c_str(), GetLastError());
			return false;
		}
		DWORD      written = 0;
		const bool ok      = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0 && written == data.size();
		CloseHandle(file);
		if (!ok || MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
			LogF("cannot write %s (error %lu)", Narrow(path).c_str(), GetLastError());
			DeleteFileW(temp.c_str());
			return false;
		}
		++rewritten;
		return true;
	}

	bool HasCopy(const std::string& rel) override { return IsFile(genRoot + L"\\" + Widen(rel)); }
	bool ReadCopy(const std::string& rel, std::string& out) override { return ReadWhole(genRoot + L"\\" + Widen(rel), out); }

	void ListMod(const std::string& relDir, std::vector<std::string>& names) override {
		WIN32_FIND_DATAW   found {};
		const std::wstring pattern = dataRoot + L"\\" + Widen(relDir) + L"\\*";
		const HANDLE       find    = FindFirstFileW(pattern.c_str(), &found);
		if (find == INVALID_HANDLE_VALUE) {
			return;
		}
		do {
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
				names.push_back(Narrow(found.cFileName));
			}
		} while (FindNextFileW(find, &found) != 0);
		FindClose(find);
	}

	void Log(const char* text) override { LogF("%s", text); }
};

// The folder the game reads the active mod's loose files from: mods/<mod>/<mod>.mpq/data.
// The mod can have any name (the loader says which one is active). Should its folder hold no
// "<mod>.mpq" folder, a single other "*.mpq" folder with a "data" folder inside is taken.
bool FindDataRoot(const D2RL::PluginContext* context, std::wstring& root, std::string& problem) {
	const std::string modName = context->activeMod != nullptr ? context->activeMod : "";
	if (modName.empty()) {
		problem = "no mod is active. Exalted items are built from a mod's files, so start the game with a mod (any name).";
		return false;
	}
	const std::wstring mod = Widen(modName);
	std::vector<std::wstring> folders, modFolders;
	if (context->modDirectory != nullptr && context->modDirectory[0] != L'\0') {
		std::wstring base = context->modDirectory;
		while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
			base.pop_back();
		}
		folders.push_back(base + L"\\" + mod + L".mpq");
		folders.push_back(base);
		modFolders.push_back(base);
	}
	folders.push_back(GameDirectory() + L"\\mods\\" + mod + L"\\" + mod + L".mpq");
	modFolders.push_back(GameDirectory() + L"\\mods\\" + mod);
	for (const std::wstring& folder : folders) {
		if (IsDirectory(folder + L"\\data")) {
			root = folder + L"\\data";
			return true;
		}
	}
	for (const std::wstring& folder : modFolders) {
		std::vector<std::wstring> found;
		WIN32_FIND_DATAW          entry {};
		const HANDLE              find = FindFirstFileW((folder + L"\\*.mpq").c_str(), &entry);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && IsDirectory(folder + L"\\" + entry.cFileName + L"\\data")) {
				found.push_back(folder + L"\\" + entry.cFileName + L"\\data");
			}
		} while (FindNextFileW(find, &entry) != 0);
		FindClose(find);
		if (found.size() == 1) {
			root = found[0];
			return true;
		}
		if (found.size() > 1) {
			problem = "the mod '" + modName + "' has more than one .mpq folder and none is named " + modName + ".mpq, so it is not clear which one the game reads.";
			return false;
		}
	}
	for (const std::wstring& folder : folders) {
		if (IsFile(folder)) {
			problem = "the mod '" + modName + "' is a packed .mpq file. Exalted items need the folder form: a folder named " + modName + ".mpq with a data folder inside.";
			return false;
		}
	}
	const DWORD error = GetLastError();
	problem = "could not find the data folder of the mod '" + modName + "' (expected mods\\" + modName + "\\" + modName + ".mpq\\data; Windows error " + std::to_string(error) + ").";
	return false;
}

std::wstring LowerPath(std::wstring path) {
	for (wchar_t& c : path) {
		c = c == L'/' ? L'\\' : c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
	}
	return path;
}

// Creates a folder and every folder above it that is missing.
void MakeFolders(const std::wstring& path) {
	for (size_t at = path.find(L'\\', 3); ; at = path.find(L'\\', at + 1)) {
		CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
		if (at == std::wstring::npos) {
			break;
		}
	}
}

// Where the plugin keeps its own files (the adjusted copies and codes.txt): beside the loader
// folder it was installed in.
//   installed for one mod (mods\<mod>\d2rloader\plugins):  mods\<mod>\d2rloader\exalted-items
//   installed for every mod (<game>\d2rloader\plugins):    <game>\d2rloader\exalted-items\<mod>
// The second has a folder per mod because the copies, and the codes saved items go by, belong
// to one mod's files. `other` is the place the other kind of install would use.
struct Storage {
	std::wstring folder, other;
	std::wstring loader; // <game>\\d2rloader
	bool         global = false;
};

Storage FindStorage(const D2RL::PluginContext* context, const std::wstring& dataRoot) {
	Storage storage;
	std::wstring modFolder = dataRoot.substr(0, dataRoot.find_last_of(L'\\'));
	modFolder              = modFolder.substr(0, modFolder.find_last_of(L'\\'));
	const std::wstring mod = Widen(context->activeMod != nullptr ? context->activeMod : "");
	std::wstring pluginDirectory;
	if (context->contextSize >= offsetof(D2RL::PluginContext, pluginDirectory) + sizeof(context->pluginDirectory) && context->pluginDirectory != nullptr) {
		pluginDirectory = context->pluginDirectory;
		while (!pluginDirectory.empty() && (pluginDirectory.back() == L'\\' || pluginDirectory.back() == L'/')) {
			pluginDirectory.pop_back();
		}
	}
	storage.global = context->loadScope == D2RL::LoadScope::Global;
	if (!pluginDirectory.empty()) {
		// Where the DLL really is says it best (drive letters can differ, so only the part from "\mods\" on is compared).
		storage.global = LowerPath(pluginDirectory).find(L"\\mods\\" + LowerPath(mod) + L"\\") == std::wstring::npos;
	}
	// The game's own loader folder: <game>\d2rloader, where <game> is the folder "mods" is in.
	std::wstring       loaderFolder = GameDirectory() + L"\\d2rloader";
	const std::wstring modsFolder   = modFolder.substr(0, modFolder.find_last_of(L'\\'));
	const std::wstring lowerMods    = LowerPath(modsFolder);
	if (lowerMods.size() > 5 && lowerMods.compare(lowerMods.size() - 5, 5, L"\\mods") == 0) {
		loaderFolder = modsFolder.substr(0, modsFolder.size() - 5) + L"\\d2rloader";
	}
	const std::wstring lower = LowerPath(pluginDirectory);
	if (storage.global && lower.size() > 8 && lower.compare(lower.size() - 8, 8, L"\\plugins") == 0) {
		loaderFolder = pluginDirectory.substr(0, pluginDirectory.size() - 8);
	}
	const std::wstring perMod = modFolder + L"\\d2rloader\\exalted-items";
	const std::wstring shared = loaderFolder + L"\\exalted-items\\" + mod;
	storage.folder            = storage.global ? shared : perMod;
	storage.other             = storage.global ? perMod : shared;
	storage.loader            = loaderFolder;
	return storage;
}

// --- File redirect ---------------------------------------------------------------------
//
// Every Win32 file call (CreateFile, GetFileAttributes, fopen, std::ifstream...) ends in one of
// four ntdll functions, reached through the import table of the calling system DLL. Those
// import slots are pointed at the functions below. A read-only open of
//     ...\mods\<mod>\<mod>.mpq\data\<file>
// for one of the adjusted files is answered with the copy in the plugin's own folder.
// Every other call goes straight through, and nothing on disk is changed.

struct Redirect {
	std::wstring   suffix; // lower case, e.g. \mods\d2rmm\d2rmm.mpq\data\global\excel\armor.txt
	std::wstring   target; // NT path of the adjusted copy
	UNICODE_STRING name {};
	volatile LONG  hits = 0;
	std::string    rel;
};

constexpr int               kMaxRedirects = exalted::SuffixSet<wchar_t>::kMax;
Redirect                    g_redirects[kMaxRedirects];
int                         g_redirectCount = 0;
exalted::SuffixSet<wchar_t> g_redirectPaths; // the suffixes of g_redirects, in the same order

using NtCreateFileFn              = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtOpenFileFn                = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using NtQueryAttributesFileFn     = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
using NtQueryFullAttributesFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);

NtCreateFileFn              g_ntCreateFile              = nullptr;
NtOpenFileFn                g_ntOpenFile                = nullptr;
NtQueryAttributesFileFn     g_ntQueryAttributesFile     = nullptr;
NtQueryFullAttributesFileFn g_ntQueryFullAttributesFile = nullptr;

// Runs for every file the game opens or asks about, so it has to say "not one of ours" fast
// (see redirect_match.h).
Redirect* MatchRedirect(const OBJECT_ATTRIBUTES* attributes) {
	if (attributes == nullptr || attributes->RootDirectory != nullptr || attributes->ObjectName == nullptr || attributes->ObjectName->Buffer == nullptr) {
		return nullptr;
	}
	const int index = g_redirectPaths.Match(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
	return index >= 0 ? &g_redirects[index] : nullptr;
}

constexpr ACCESS_MASK kWriteAccess = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | GENERIC_WRITE | GENERIC_ALL | WRITE_DAC | WRITE_OWNER;

NTSTATUS NTAPI HookNtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK status, PLARGE_INTEGER allocation, ULONG fileAttributes, ULONG share,
                                ULONG disposition, ULONG options, PVOID ea, ULONG eaLength) {
	Redirect* redirect = (access & kWriteAccess) == 0 ? MatchRedirect(attributes) : nullptr;
	if (redirect != nullptr) {
		OBJECT_ATTRIBUTES copy = *attributes;
		copy.ObjectName        = &redirect->name;
		const NTSTATUS result  = g_ntCreateFile(handle, access, &copy, status, allocation, fileAttributes, share, disposition, options, ea, eaLength);
		if (result >= 0) {
			InterlockedIncrement(&redirect->hits);
			return result;
		}
	}
	return g_ntCreateFile(handle, access, attributes, status, allocation, fileAttributes, share, disposition, options, ea, eaLength);
}

NTSTATUS NTAPI HookNtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK status, ULONG share, ULONG options) {
	Redirect* redirect = (access & kWriteAccess) == 0 ? MatchRedirect(attributes) : nullptr;
	if (redirect != nullptr) {
		OBJECT_ATTRIBUTES copy = *attributes;
		copy.ObjectName        = &redirect->name;
		const NTSTATUS result  = g_ntOpenFile(handle, access, &copy, status, share, options);
		if (result >= 0) {
			InterlockedIncrement(&redirect->hits);
			return result;
		}
	}
	return g_ntOpenFile(handle, access, attributes, status, share, options);
}

NTSTATUS NTAPI HookNtQueryAttributesFile(POBJECT_ATTRIBUTES attributes, PVOID info) {
	if (Redirect* redirect = MatchRedirect(attributes)) {
		OBJECT_ATTRIBUTES copy = *attributes;
		copy.ObjectName        = &redirect->name;
		const NTSTATUS result  = g_ntQueryAttributesFile(&copy, info);
		if (result >= 0) {
			return result;
		}
	}
	return g_ntQueryAttributesFile(attributes, info);
}

NTSTATUS NTAPI HookNtQueryFullAttributesFile(POBJECT_ATTRIBUTES attributes, PVOID info) {
	if (Redirect* redirect = MatchRedirect(attributes)) {
		OBJECT_ATTRIBUTES copy = *attributes;
		copy.ObjectName        = &redirect->name;
		const NTSTATUS result  = g_ntQueryFullAttributesFile(&copy, info);
		if (result >= 0) {
			return result;
		}
	}
	return g_ntQueryFullAttributesFile(attributes, info);
}

struct PatchedSlot {
	void** slot;
	void*  original;
};
std::vector<PatchedSlot> g_patched;
HMODULE                  g_self = nullptr;

bool Readable(const void* address, size_t size) {
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	return static_cast<const uint8_t*>(address) + size <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
}

// The same question for one address after another: the stretch of memory around the last
// address is remembered, so walking a table asks Windows once per stretch, not once per entry.
struct ReadableSpan {
	const uint8_t* begin = nullptr;
	const uint8_t* end   = nullptr;

	bool Covers(const void* address, size_t size) {
		const auto* at = static_cast<const uint8_t*>(address);
		if (at >= begin && at + size <= end) {
			return true;
		}
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}
		begin = static_cast<const uint8_t*>(info.BaseAddress);
		end   = begin + info.RegionSize;
		return at + size <= end;
	}
};

// Points every import slot of one module that holds `real` at `hook`.
int PatchModuleImports(HMODULE module, void* const* real, void* const* hook, int count) {
	const auto* base = reinterpret_cast<const uint8_t*>(module);
	if (!Readable(base, 0x1000)) {
		return 0;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0xF00) {
		return 0;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return 0;
	}
	const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
		return 0;
	}
	int          patched = 0;
	ReadableSpan descriptors, slots;
	for (const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);; ++descriptor) {
		if (!descriptors.Covers(descriptor, sizeof(*descriptor)) || descriptor->FirstThunk == 0 || descriptor->Name == 0) {
			break;
		}
		for (auto** slot = reinterpret_cast<void**>(const_cast<uint8_t*>(base) + descriptor->FirstThunk);; ++slot) {
			if (!slots.Covers(slot, sizeof(*slot)) || *slot == nullptr) {
				break;
			}
			for (int i = 0; i < count; ++i) {
				DWORD old = 0;
				if (*slot == real[i] && VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old) != 0) {
					g_patched.push_back({ slot, *slot });
					*slot = hook[i];
					VirtualProtect(slot, sizeof(*slot), old, &old);
					++patched;
				}
			}
		}
	}
	return patched;
}

// Returns the number of import slots redirected (0 = the redirect is not active).
int InstallRedirects(const GameIo& io, const std::vector<std::string>& files) {
	// The part of the path every drive alias agrees on: from "\mods\" onwards.
	std::wstring lower = io.dataRoot;
	for (wchar_t& c : lower) {
		c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
	}
	size_t start = lower.rfind(L"\\mods\\");
	if (start == std::wstring::npos) {
		start = lower.find(L'\\');
	}
	if (start == std::wstring::npos) {
		return 0;
	}
	g_redirectCount = 0;
	g_redirectPaths.Clear();
	for (const std::string& rel : files) {
		if (g_redirectCount >= kMaxRedirects) {
			break;
		}
		Redirect& redirect = g_redirects[g_redirectCount++];
		redirect.rel       = rel;
		redirect.suffix    = lower.substr(start) + L"\\" + Widen(rel);
		for (wchar_t& c : redirect.suffix) {
			c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
		}
		if (g_redirectPaths.Add(redirect.suffix) != g_redirectCount - 1) {
			--g_redirectCount; // never expected; keeps the two lists in step
			continue;
		}
		redirect.target             = L"\\??\\" + io.genRoot + L"\\" + Widen(rel);
		redirect.name.Buffer        = redirect.target.data();
		redirect.name.Length        = static_cast<USHORT>(redirect.target.size() * sizeof(wchar_t));
		redirect.name.MaximumLength = static_cast<USHORT>(redirect.name.Length + sizeof(wchar_t));
		redirect.hits               = 0;
	}

	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	g_ntCreateFile              = reinterpret_cast<NtCreateFileFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
	g_ntOpenFile                = reinterpret_cast<NtOpenFileFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtOpenFile")));
	g_ntQueryAttributesFile     = reinterpret_cast<NtQueryAttributesFileFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryAttributesFile")));
	g_ntQueryFullAttributesFile = reinterpret_cast<NtQueryFullAttributesFileFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryFullAttributesFile")));
	if (g_ntCreateFile == nullptr || g_ntOpenFile == nullptr || g_ntQueryAttributesFile == nullptr || g_ntQueryFullAttributesFile == nullptr) {
		return 0;
	}
	void* const real[] = { reinterpret_cast<void*>(g_ntCreateFile), reinterpret_cast<void*>(g_ntOpenFile), reinterpret_cast<void*>(g_ntQueryAttributesFile),
		                   reinterpret_cast<void*>(g_ntQueryFullAttributesFile) };
	void* const hook[] = { reinterpret_cast<void*>(&HookNtCreateFile), reinterpret_cast<void*>(&HookNtOpenFile), reinterpret_cast<void*>(&HookNtQueryAttributesFile),
		                   reinterpret_cast<void*>(&HookNtQueryFullAttributesFile) };

	int          patched  = 0;
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W entry {};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Module32FirstW(snapshot, &entry); more != 0; more = Module32NextW(snapshot, &entry)) {
			if (entry.hModule != ntdll && entry.hModule != g_self) {
				patched += PatchModuleImports(entry.hModule, real, hook, 4);
			}
		}
		CloseHandle(snapshot);
	}
	if (patched == 0) { // no module list: the two DLLs every Win32 file call passes through
		for (const wchar_t* name : { L"kernelbase.dll", L"kernel32.dll" }) {
			if (const HMODULE module = GetModuleHandleW(name)) {
				patched += PatchModuleImports(module, real, hook, 4);
			}
		}
	}
	return patched;
}

void RemoveRedirects() {
	for (const PatchedSlot& patch : g_patched) {
		DWORD old = 0;
		if (VirtualProtect(patch.slot, sizeof(*patch.slot), PAGE_READWRITE, &old) != 0) {
			*patch.slot = patch.original;
			VirtualProtect(patch.slot, sizeof(*patch.slot), old, &old);
		}
	}
	g_patched.clear();
}

// Deletes copies left over from an earlier run that this run did not produce.
void RemoveStale(const std::wstring& folder, const std::wstring& genRoot, const std::vector<std::string>& keep) {
	WIN32_FIND_DATAW found {};
	const HANDLE     find = FindFirstFileW((folder + L"\\*").c_str(), &found);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		const std::wstring name = found.cFileName;
		if (name == L"." || name == L"..") {
			continue;
		}
		const std::wstring path = folder + L"\\" + name;
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			RemoveStale(path, genRoot, keep);
			continue;
		}
		std::string rel = Narrow(path.substr(genRoot.size() + 1));
		for (char& c : rel) {
			c = c == '\\' ? '/' : c;
		}
		bool wanted = false;
		for (const std::string& file : keep) {
			wanted = wanted || _stricmp(file.c_str(), rel.c_str()) == 0;
		}
		if (!wanted) {
			DeleteFileW(path.c_str());
		}
	} while (FindNextFileW(find, &found) != 0);
	FindClose(find);
}

// --- config ---------------------------------------------------------------------------

const char* TierWords(int tiers) {
	static const char* const words[8] = { "off", "normal", "exceptional", "normal, exceptional", "elite", "normal, elite", "exceptional, elite", "all" };
	return words[tiers & exalted::TierAll];
}

// "all", "off", or any of normal / exceptional / elite, separated by commas or spaces.
// Returns -1 when a word is not one of those.
int ParseTiers(const std::string& text) {
	int         tiers = 0;
	bool        any   = false;
	std::string word;
	for (const char raw : text + ",") {
		const char c = raw >= 'A' && raw <= 'Z' ? static_cast<char>(raw - 'A' + 'a') : raw;
		if (c != ',' && c != ' ' && c != '\t' && c != '+' && c != ';' && c != '\r') {
			word += c;
			continue;
		}
		if (word.empty()) {
			continue;
		}
		if (word == "all") {
			tiers |= exalted::TierAll;
		} else if (word == "normal") {
			tiers |= exalted::TierNormal;
		} else if (word == "exceptional") {
			tiers |= exalted::TierExceptional;
		} else if (word == "elite") {
			tiers |= exalted::TierElite;
		} else if (word != "off" && word != "none") {
			return -1;
		}
		any = true;
		word.clear();
	}
	return any ? tiers : -1;
}

// Name colours, as the game's colour code characters.
struct ColourName {
	const char* name;
	char        code;
};
const ColourName  kColours[]  = { { "none", '\0' }, { "white", '0' }, { "red", '1' },    { "green", '2' },  { "blue", '3' },      { "gold", '4' },  { "grey", '5' },
	                              { "gray", '5' },  { "tan", '7' },   { "orange", '8' }, { "yellow", '9' }, { "darkgreen", ':' }, { "purple", ';' } };
const char* const kColourList = "white, red, green, blue, gold, grey, tan, orange, yellow, darkgreen, purple";

// -1 when the word is not a colour.
char ColourCode(std::string word) {
	for (char& c : word) {
		c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
	}
	word.erase(std::remove(word.begin(), word.end(), ' '), word.end());
	for (const ColourName& colour : kColours) {
		if (word == colour.name) {
			return colour.code;
		}
	}
	return word.empty() ? '\0' : static_cast<char>(-1);
}

const char* ColourWord(char code) {
	for (const ColourName& colour : kColours) {
		if (colour.code == code) {
			return colour.name;
		}
	}
	return "orange";
}

exalted::Config LoadConfig() {
	exalted::Config config;
	static char     text[32768];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		LogF("config: could not read exalted-items.toml, using the built-in defaults with no recipes");
		return config;
	}
	std::string section;
	const char* cursor = text;
	while (*cursor != '\0') {
		const char* end = strchr(cursor, '\n');
		std::string line(cursor, end != nullptr ? static_cast<size_t>(end - cursor) : strlen(cursor));
		cursor = end != nullptr ? end + 1 : cursor + line.size();

		size_t start = 0;
		while (start < line.size() && (line[start] == ' ' || line[start] == '\t' || static_cast<unsigned char>(line[start]) >= 0x80)) {
			++start; // also skips a UTF-8 byte-order mark
		}
		line = line.substr(start);
		if (line.empty() || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			const size_t close = line.find(']');
			section            = close != std::string::npos ? line.substr(1, close - 1) : "";
			continue;
		}
		const size_t equals = line.find('=');
		if (equals == std::string::npos) {
			continue;
		}
		std::string key = line.substr(0, equals), value = line.substr(equals + 1);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
			key.pop_back();
		}
		size_t valueStart = 0;
		while (valueStart < value.size() && (value[valueStart] == ' ' || value[valueStart] == '\t')) {
			++valueStart;
		}
		value = value.substr(valueStart);
		std::string textValue;
		if (!value.empty() && value[0] == '"') {
			for (size_t i = 1; i < value.size() && value[i] != '"'; ++i) {
				if (value[i] == '\\' && i + 1 < value.size()) {
					++i;
				}
				textValue += value[i];
			}
		} else {
			textValue = value.substr(0, value.find('#'));
			while (!textValue.empty() && (textValue.back() == ' ' || textValue.back() == '\t' || textValue.back() == '\r')) {
				textValue.pop_back();
			}
		}
		const int number = atoi(textValue.c_str());

		const bool on = textValue == "true" || number != 0;
		if (section == "crystal") {
			if (key == "enabled") {
				config.crystalEnabled = on;
			} else if (key == "name") {
				config.crystalName = textValue;
			} else if (key == "drop_weight_percent") {
				config.crystalDropPercent = number;
			} else if (key == "stash_slot") {
				config.crystalStashSlot = on;
			} else if (key == "stash_slot_right") {
				config.crystalSlotRight = number;
			} else if (key == "stash_slot_down") {
				config.crystalSlotDown = number;
			} else if (key == "name_color") {
				const char code = ColourCode(textValue);
				if (code < 0) {
					LogF("config: name_color = \"%s\" is not understood (use %s, or none); orange is used", textValue.c_str(), kColourList);
				} else {
					config.crystalColor = code;
				}
			}
		} else if (section == "desecration") {
			if (key == "enabled") {
				config.desecrationEnabled = on;
			} else if (key == "name") {
				config.desecrationName = textValue;
			} else if (key == "drop_weight_percent") {
				config.desecrationDropPercent = number;
			} else if (key == "stash_slot") {
				config.desecrationStashSlot = on;
			} else if (key == "stash_slot_right") {
				config.desecrationSlotRight = number;
			} else if (key == "stash_slot_down") {
				config.desecrationSlotDown = number;
			} else if (key == "name_color") {
				const char code = ColourCode(textValue);
				if (code < 0) {
					LogF("config: name_color = \"%s\" is not understood (use %s, or none); orange is used", textValue.c_str(), kColourList);
				} else {
					config.desecrationColor = code;
				}
			}
		} else if (section == "loot_filter") {
			if (key == "exalted_box") {
				config.filterBox = on;
			} else if (key == "label") {
				config.filterLabel = textValue;
			}
		} else if (section == "restore") {
			if (key == "enabled") {
				config.restoreEnabled = on;
			} else if (key == "material") {
				config.restoreMaterial = textValue;
			} else if (key == "show_line") {
				config.restoreShowLine = on;
			} else if (key == "strip_on_downgrade") {
				config.restoreStrip = on;
			}
		} else if (section.compare(0, 8, "restore.") == 0) {
			// [restore.<kind>]: one true/false line per attribute; only "off" needs remembering.
			const std::string name = section.substr(8) + "." + key;
			auto&             off  = config.restoreOff;
			off.erase(std::remove(off.begin(), off.end(), name), off.end());
			if (!on) {
				off.push_back(name);
			}
		} else if (section == "recipes" && key == "downgrade") {
			config.downgrade = textValue;
		} else if (section == "recipes") {
			config.recipes.push_back({ key, textValue });
		} else if (section == "exalted-items") {
			if (key == "enabled") {
				config.enabled = textValue == "true" || number != 0;
			} else if (key == "terror_zone_only") {
				config.terrorZoneOnly = textValue == "true" || number != 0;
			} else if (key == "drop_level_max") {
				config.dropLevelMax = number;
			} else if (key == "name_prefix") {
				config.namePrefix = textValue;
			} else if (key == "stat_step_percent") {
				config.statStepPercent = number;
			} else if (key == "level_req_cap") {
				config.levelReqCap = number;
			} else if (key == "unique_drops" || key == "set_drops") {
				const int tiers = ParseTiers(textValue);
				if (tiers < 0) {
					LogF("config: %s = \"%s\" is not understood (use off, normal, exceptional, elite, all, or a list such as \"exceptional, elite\"); all is used", key.c_str(),
					     textValue.c_str());
				} else {
					(key == "unique_drops" ? config.uniqueTiers : config.setTiers) = tiers;
				}
			}
		}
	}
	auto clamp = [](int& value, int low, int high) { value = value < low ? low : value > high ? high : value; };
	clamp(config.dropLevelMax, exalted::kDropLevelFloor, exalted::kDropLevelCeiling);
	clamp(config.statStepPercent, 0, 300);
	clamp(config.levelReqCap, 1, 110);
	clamp(config.crystalDropPercent, 0, 1000);
	clamp(config.desecrationDropPercent, 0, 1000);
	clamp(config.crystalSlotRight, -20, 20);
	clamp(config.crystalSlotDown, -20, 20);
	clamp(config.desecrationSlotRight, -20, 20);
	clamp(config.desecrationSlotDown, -20, 20);
	if (config.crystalName.empty() || config.crystalName.size() > 60) {
		config.crystalName = "Restoration Crystal";
	}
	if (config.namePrefix.size() > 40) {
		config.namePrefix.resize(40);
	}
	return config;
}

uint64_t g_random = 0x9E3779B97F4A7C15ULL;

uint32_t Random(uint32_t below) {
	g_random ^= g_random << 13;
	g_random ^= g_random >> 7;
	g_random ^= g_random << 17;
	return below != 0 ? static_cast<uint32_t>((g_random >> 20) % below) : 0;
}

uint32_t FourCC(const std::string& code) {
	char c[4] = { ' ', ' ', ' ', ' ' };
	for (size_t i = 0; i < code.size() && i < 4; ++i) {
		c[i] = code[i];
	}
	return D2RL::Items::MakeItemCode(c[0], c[1], c[2], c[3]);
}

// --- Unique and set items on exalted bases -----------------------------------------------
//
// The game picks a new item's unique row, and its set row, in one routine each. Both read the
// item's base, go through every row written for that base's code, drop the rows above the
// item's level, and pick by rarity. An exalted base has a code of its own, so they find
// nothing for it.
//
// Rows are not repeated for the exalted code (a repeated unique is a separate unique to the
// game and to a grail; a repeated set row is an extra member of its set). Instead both
// routines are hooked, and for an exalted base the hook lets the routine see a base of the
// same family for the length of that one call, then puts the exalted base back. Level check,
// rarity, the "already dropped in this game" rule for uniques and the item's properties stay
// the game's own work, and the item is saved as the real row.
//
// Which tiers of the family may be used is a setting. With more than one, the base is chosen
// by exalted::FamilyWeights, which gives every row the chance it would have in one merged
// list. Only the item being made is touched; the game's tables are never written to.
//
// The two routines are found by pattern (game_code.h). Layout facts used besides what is read
// from the matched code: an item unit starts with { u32 type (4 = item), u32 item number }.

using PickFn      = uint64_t (*)(void* game, void* item, void* drop);
using ItemRowFn   = const void* (*)(uint8_t version, uint32_t number);
using ItemLevelFn = int32_t (*)(void* item);

constexpr uint32_t kNoItem = 0xFFFFFFFFu;

// The addresses in the one game build this was written against (image stamp 6ab3782c), used
// only when the pattern search finds nothing and the bytes there still match.
constexpr uint32_t kKnownImageStamp = 0x6ab3782c;           // build stamp in the header of that image
constexpr uint64_t kKnownPick[2] = { 0x443910, 0x58c2f0 }; // unique, set
constexpr uint64_t kKnownItemRow = 0x314110, kKnownItemLevel = 0x36ccf0;
constexpr uint32_t kKnownVersionOffset = 0x106, kKnownCodeOffset = 0x80;
const uint8_t      kKnownUniqueEntry[25] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40 };
const uint8_t      kKnownSetEntry[23]    = { 0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x60, 0x08, 0x00, 0x00 };
const uint8_t      kKnownItemRowEntry[16]   = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x8b, 0xfa, 0xe8, 0x73, 0xc9, 0xfe, 0xff, 0x3b, 0xb8, 0xa8 };
const uint8_t      kKnownItemLevelEntry[14] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9, 0x74, 0x2d };

// The item numbers of the families in one data version (the data banks number their items
// differently, and the tables can be loaded again).
struct ItemNumbers {
	uint32_t                           revision = 0;
	std::vector<int32_t>               familyOf[2]; // item number -> family; -1 = not an exalted base with drops of that kind
	std::vector<std::vector<uint32_t>> bases[2];    // family -> item number of each of its bases
};

std::vector<exalted::Family> g_families[2]; // [exalted::PickUnique], [exalted::PickSet]
exalted::GameCode            g_code;
PickFn                       g_originalPick[2] = { nullptr, nullptr };
ItemRowFn                    g_itemRow         = nullptr;
ItemLevelFn                  g_itemLevel       = nullptr;
std::atomic<ItemNumbers*>    g_itemNumbers[256];
ItemNumbers*                 g_retiredNumbers[256]; // the list each one replaced (guarded by g_numbersLock)
std::atomic<uint32_t>        g_tablesRevision { 1 };
CRITICAL_SECTION             g_numbersLock;
std::atomic<int>             g_pickRolls[2], g_pickMade[2], g_pickEmpty[2];
char                         g_pickState[2][200] = { "off", "off" };
double                       g_pickerFindMs = 0, g_pickerHookMs = 0; // start-up report

// The last time the game picked nothing for the base an exalted base posed as (for the console
// command). For a unique this also happens by the game's own rule, when the unique it chose has
// already dropped in that game. For a set piece it should never happen: the family lists hold
// only rows the picker can take, so a miss there means the lists and the game disagree.
struct PickMiss {
	std::atomic<uint32_t> code { 0 }; // item code of the base posed as; 0 = none yet
	std::atomic<int32_t>  level { 0 };
};
PickMiss          g_pickMiss[2];
std::atomic<bool> g_setMissLogged { false };

void CodeText(uint32_t code, char out[5]) {
	for (int i = 0; i < 4; ++i) {
		const char c = static_cast<char>(code >> (8 * i));
		out[i]       = c > ' ' && c < 127 ? c : '\0';
	}
	out[4] = '\0';
}

// Reads every item row of one data version through the game's own lookup. This is also the
// check that the lookup and the code offset are what they seem: the codes have to look like
// item codes, and the exalted bases have to be among them.
ItemNumbers* ReadItemNumbers(uint8_t version, uint32_t revision) {
	auto* numbers     = new ItemNumbers();
	numbers->revision = revision;
	std::vector<std::pair<uint32_t, uint32_t>> codes; // code, item number
	size_t                                     odd = 0;
	for (uint32_t number = 0; number < 200000; ++number) {
		const void* row = g_itemRow(version, number);
		if (row == nullptr) {
			break;
		}
		uint32_t code = 0;
		memcpy(&code, static_cast<const uint8_t*>(row) + g_code.codeOffset, sizeof(code));
		bool plain = true;
		for (int i = 0; i < 4; ++i) {
			const uint8_t c = static_cast<uint8_t>(code >> (8 * i));
			plain           = plain && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == 0);
		}
		odd += plain ? 0 : 1;
		codes.emplace_back(code, number);
	}
	if (codes.size() < 100 || odd * 20 > codes.size()) {
		LogF("unique and set drops: what the game's item lookup returned for data version %u does not look like item rows (%d rows, %d odd codes); exalted bases will not roll unique or set there",
		     version, static_cast<int>(codes.size()), static_cast<int>(odd));
		return numbers;
	}
	const size_t count = codes.size();
	std::sort(codes.begin(), codes.end());
	auto numberOf = [&codes](uint32_t code) {
		const auto found = std::lower_bound(codes.begin(), codes.end(), std::make_pair(code, 0u));
		return found != codes.end() && found->first == code ? found->second : kNoItem;
	};
	int present = 0, absent = 0;
	for (int kind = 0; kind < 2; ++kind) {
		numbers->familyOf[kind].assign(count, -1);
		numbers->bases[kind].resize(g_families[kind].size());
		for (size_t f = 0; f < g_families[kind].size(); ++f) {
			const uint32_t own = numberOf(FourCC(g_families[kind][f].code));
			if (own == kNoItem || own >= count) {
				++absent;
				continue;
			}
			numbers->familyOf[kind][own] = static_cast<int32_t>(f);
			for (const exalted::FamilyBase& base : g_families[kind][f].bases) {
				numbers->bases[kind][f].push_back(numberOf(FourCC(base.code)));
			}
			++present;
		}
	}
	LogF("unique and set drops: %d item rows read for data version %u; %d exalted base entries found, %d not in this data version", static_cast<int>(count), version, present, absent);
	return numbers;
}

const ItemNumbers* ItemNumbersFor(uint8_t version) {
	const uint32_t revision = g_tablesRevision.load(std::memory_order_acquire);
	ItemNumbers*   numbers  = g_itemNumbers[version].load(std::memory_order_acquire);
	if (numbers != nullptr && numbers->revision == revision) {
		return numbers;
	}
	EnterCriticalSection(&g_numbersLock);
	numbers = g_itemNumbers[version].load(std::memory_order_acquire);
	if (numbers == nullptr || numbers->revision != revision) {
		ItemNumbers* const fresh = ReadItemNumbers(version, revision);
		g_itemNumbers[version].store(fresh, std::memory_order_release);
		// The list just replaced may still be in use by a pick that started before the tables
		// were loaded again, so it is kept. The one it replaced in its turn is two table loads
		// old; no pick lasts that long, and it is freed.
		delete g_retiredNumbers[version];
		g_retiredNumbers[version] = numbers;
		numbers                   = fresh;
	}
	LeaveCriticalSection(&g_numbersLock);
	return numbers;
}

uint64_t PickHook(int kind, void* game, void* item, void* drop) {
	const PickFn original = g_originalPick[kind];
	auto*        fields   = static_cast<uint32_t*>(item);
	if (game == nullptr || item == nullptr || fields[0] != 4) {
		return original(game, item, drop);
	}
	const uint8_t      version = *(static_cast<const uint8_t*>(game) + g_code.versionOffset);
	const ItemNumbers* numbers = ItemNumbersFor(version);
	const uint32_t     own     = fields[1];
	if (numbers == nullptr || own >= numbers->familyOf[kind].size() || numbers->familyOf[kind][own] < 0) {
		return original(game, item, drop); // not an exalted base
	}
	const size_t                 index  = static_cast<size_t>(numbers->familyOf[kind][own]);
	const exalted::Family&       family = g_families[kind][index];
	const std::vector<uint32_t>& bases  = numbers->bases[kind][index];
	// The picker only takes rows the item's level allows, so only those count here.
	const int level = g_itemLevel != nullptr ? g_itemLevel(item) : 1 << 20;
	uint32_t  weights[exalted::kMaxFamilyBases];
	exalted::FamilyWeights(family, level, weights);
	uint32_t total = 0;
	for (size_t i = 0; i < static_cast<size_t>(exalted::kMaxFamilyBases); ++i) {
		if (i >= bases.size() || bases[i] == kNoItem) {
			weights[i] = 0;
		}
		total += weights[i];
	}
	++g_pickRolls[kind];
	const int choice = total != 0 ? exalted::FamilyChoice(weights, Random(total)) : -1;
	if (choice < 0) {
		// Nothing in the family for an item of this level: the game falls back as it does for
		// any base without rows (a unique roll becomes a rare, a set roll a magic item).
		++g_pickEmpty[kind];
		return original(game, item, drop);
	}
	const uint32_t posed  = bases[static_cast<size_t>(choice)];
	fields[1]             = posed;
	const uint64_t result = original(game, item, drop);
	fields[1]             = own;
	if ((result & 0xff) != 0) {
		++g_pickMade[kind];
	} else if (const void* row = g_itemRow(version, posed)) {
		uint32_t code = 0;
		memcpy(&code, static_cast<const uint8_t*>(row) + g_code.codeOffset, sizeof(code));
		g_pickMiss[kind].code.store(code, std::memory_order_relaxed);
		g_pickMiss[kind].level.store(level, std::memory_order_relaxed);
		if (kind == exalted::PickSet && !g_setMissLogged.exchange(true)) {
			char text[5];
			CodeText(code, text);
			LogF("set drops: the game picked no set piece for base '%s' at item level %d, although the tables list one for it there. The roll fell back as the game does. (Logged once per game start; the 'exalted' console command shows the latest.)",
			     text, static_cast<int>(level));
		}
	}
	return result;
}

uint64_t UniquePickHook(void* game, void* item, void* drop) {
	return PickHook(exalted::PickUnique, game, item, drop);
}

uint64_t SetPickHook(void* game, void* item, void* drop) {
	return PickHook(exalted::PickSet, game, item, drop);
}

void __cdecl OnTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
	++g_tablesRevision; // item numbers are read again on the next pick
}

// The parts of the game image that hold code.
std::vector<exalted::CodeRange> CodeRanges(uintptr_t base) {
	std::vector<exalted::CodeRange> ranges;
	MEMORY_BASIC_INFORMATION        info {};
	for (const uint8_t* at = reinterpret_cast<const uint8_t*>(base); VirtualQuery(at, &info, sizeof(info)) != 0 && info.AllocationBase == reinterpret_cast<void*>(base);
	     at                = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize) {
		if (info.State != MEM_COMMIT || (info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 || (info.Protect & PAGE_GUARD) != 0) {
			continue;
		}
		const uint8_t* begin = static_cast<const uint8_t*>(info.BaseAddress);
		const uint8_t* end   = begin + info.RegionSize;
		if (!ranges.empty() && ranges.back().end == begin) {
			ranges.back().end = end;
		} else {
			ranges.push_back({ begin, end });
		}
	}
	if (!ranges.empty()) {
		return ranges;
	}
	// No region list for the image: go by its section table.
	const auto* bytes = reinterpret_cast<const uint8_t*>(base);
	if (!Readable(bytes, 0x1000)) {
		return ranges;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0xE00) {
		return ranges;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections > 64) {
		return ranges;
	}
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
		const uint8_t* begin = bytes + section->VirtualAddress;
		if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && section->Misc.VirtualSize != 0 && Readable(begin, 1) && Readable(begin + section->Misc.VirtualSize - 1, 1)) {
			ranges.push_back({ begin, begin + section->Misc.VirtualSize });
		}
	}
	return ranges;
}

// The build stamp in the header of the game image; 0 when there is no header to read.
uint32_t ImageStamp(uintptr_t base) {
	const auto* bytes = reinterpret_cast<const uint8_t*>(base);
	if (base == 0 || !Readable(bytes, 0x1000)) {
		return 0;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0xE00) {
		return 0;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

// Hooks the game's two pickers for the families the settings allow. What happened to each
// ends up in g_pickState, for the status line and the console command.
void InstallPickers(const D2RL::PluginContext* context, const std::vector<exalted::Family>& families) {
	static const char* const kindName[2] = { "unique", "set" };
	const int                tiers[2]    = { g_config.uniqueTiers, g_config.setTiers };
	for (auto& list : g_families) {
		list.clear();
	}
	for (const exalted::Family& family : families) {
		auto& list  = g_families[family.sets ? exalted::PickSet : exalted::PickUnique];
		bool  known = false;
		for (const exalted::Family& other : list) {
			known = known || other.code == family.code;
		}
		if (!known) {
			list.push_back(family); // otherwise the first data bank has already described this base
		}
	}
	for (int kind = 0; kind < 2; ++kind) {
		snprintf(g_pickState[kind], sizeof(g_pickState[kind]), "%s", tiers[kind] == 0 ? "off" : "none of the bases has any");
	}
	if (g_families[0].empty() && g_families[1].empty()) {
		return;
	}
	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	g_random ^= static_cast<uint64_t>(counter.QuadPart) * 0x2545F4914F6CDD1DULL;

	const uintptr_t   base = context->exeBase;
	exalted::GameCode code;
	if (base != 0) {
		const double                          started = Clock();
		const std::vector<exalted::CodeRange> ranges  = CodeRanges(base);
		size_t                                bytes   = 0;
		for (const exalted::CodeRange& range : ranges) {
			bytes += static_cast<size_t>(range.end - range.begin);
		}
		// In the one game image this was written against, the pickers are where they are known
		// to be. That is checked there (the same test a search result gets) instead of going
		// through the whole image. Any other image, or anything unexpected, is searched.
		const auto* image = reinterpret_cast<const uint8_t*>(base);
		if (ImageStamp(base) == kKnownImageStamp) {
			code = exalted::FindGameCode(image, ranges, kKnownPick);
			if (code.searched || code.itemRowRva != kKnownItemRow || code.itemLevelRva != kKnownItemLevel || code.versionOffset != kKnownVersionOffset || code.codeOffset != kKnownCodeOffset) {
				code = exalted::FindGameCode(image, ranges);
			}
		} else {
			code = exalted::FindGameCode(image, ranges);
		}
		g_pickerFindMs = Clock() - started;
		if (code.searched) {
			LogF("game code (%d MB searched in %.1f ms): %s", static_cast<int>(bytes >> 20), g_pickerFindMs, code.notes.c_str());
		} else {
			LogF("game code (known game image, both pickers confirmed where they are known to be, no search, %.2f ms): %s", g_pickerFindMs, code.notes.c_str());
		}
	}
	// The one known build, in case the search came up empty there.
	if (base != 0 && (code.pickRva[0] == 0 || code.pickRva[1] == 0) && context->CheckExpectedBytes(kKnownItemRow, kKnownItemRowEntry, sizeof(kKnownItemRowEntry))
		&& (code.itemRowRva == 0 || code.itemRowRva == kKnownItemRow)) {
		const bool unique = code.pickRva[exalted::PickUnique] == 0 && context->CheckExpectedBytes(kKnownPick[exalted::PickUnique], kKnownUniqueEntry, sizeof(kKnownUniqueEntry));
		const bool set    = code.pickRva[exalted::PickSet] == 0 && context->CheckExpectedBytes(kKnownPick[exalted::PickSet], kKnownSetEntry, sizeof(kKnownSetEntry));
		if (unique || set) {
			code.itemRowRva    = kKnownItemRow;
			code.versionOffset = kKnownVersionOffset;
			code.codeOffset    = kKnownCodeOffset;
			if (code.itemLevelRva == 0 && context->CheckExpectedBytes(kKnownItemLevel, kKnownItemLevelEntry, sizeof(kKnownItemLevelEntry))) {
				code.itemLevelRva = kKnownItemLevel;
			}
			if (unique) {
				code.pickRva[exalted::PickUnique]       = kKnownPick[exalted::PickUnique];
				code.pickEntrySize[exalted::PickUnique] = sizeof(kKnownUniqueEntry);
			}
			if (set) {
				code.pickRva[exalted::PickSet]       = kKnownPick[exalted::PickSet];
				code.pickEntrySize[exalted::PickSet] = sizeof(kKnownSetEntry);
			}
			LogF("game code: the pattern search missed a picker, but this is the known game build, so its known addresses are used");
		}
	}
	g_code      = code;
	g_itemRow   = code.itemRowRva != 0 ? reinterpret_cast<ItemRowFn>(base + code.itemRowRva) : nullptr;
	g_itemLevel = code.itemLevelRva != 0 ? reinterpret_cast<ItemLevelFn>(base + code.itemLevelRva) : nullptr;
	InitializeCriticalSection(&g_numbersLock);

	static uint8_t entry[2][32];
	const PickFn   hooks[2]    = { &UniquePickHook, &SetPickHook };
	const double   hookStarted = Clock();
	for (int kind = 0; kind < 2; ++kind) {
		if (g_families[kind].empty()) {
			continue;
		}
		const uint32_t size = code.pickEntrySize[kind];
		if (g_itemRow == nullptr || code.pickRva[kind] == 0 || size == 0 || size > sizeof(entry[kind])) {
			snprintf(g_pickState[kind], sizeof(g_pickState[kind]), "NOT active: the game's %s picker was not recognised in this game build (see the log)", kindName[kind]);
			LogF("%s drops: the game's %s picker was not recognised in this game build, so exalted bases cannot roll %s (they fall back as the game does for a base without %s items)",
			     kindName[kind], kindName[kind], kindName[kind], kindName[kind]);
			continue;
		}
		memcpy(entry[kind], reinterpret_cast<const void*>(base + code.pickRva[kind]), size);
		if (!context->InstallInlineHook(code.pickRva[kind], entry[kind], size, hooks[kind], &g_originalPick[kind]) || g_originalPick[kind] == nullptr) {
			g_originalPick[kind] = nullptr;
			snprintf(g_pickState[kind], sizeof(g_pickState[kind]), "NOT active: the loader refused the hook on the game's %s picker", kindName[kind]);
			LogF("%s drops: the loader refused the hook at 0x%llx", kindName[kind], static_cast<unsigned long long>(code.pickRva[kind]));
			continue;
		}
		snprintf(g_pickState[kind], sizeof(g_pickState[kind]), "%s (%d bases)%s", TierWords(tiers[kind]), static_cast<int>(g_families[kind].size()),
		         g_itemLevel == nullptr ? ", item level not readable so tiers are weighed without it" : "");
	}
	g_pickerHookMs = Clock() - hookStarted;
}

// --- Taking a restore off again (restore_strip.h) ---------------------------------------
//
// One hook on the game's "apply one property to an item" routine. Every call but one passes
// straight through. The exception is the sign the exalted -> elite recipe carries (the marker
// property with an amount of nothing): there the plugin takes the item's restore off, by
// calling the same routine with the opposite of each amount the restore wrote down.
//
// Only in the game image this was written against: the routine and the four small helpers
// are used at their known places, and the hook is refused when the bytes there differ.

using ApplyPropertyFn = uint64_t (*)(int kind, void* item, void* third, void* fourth, int index, int sixth, exalted::PropertyCall* call, int state, int flags, void* tenth);
using ItemVersionFn   = uint8_t (*)(void* item);
using PropertyRowFn   = const uint8_t* (*)(uint8_t version, int id);
using PropertyCountFn = int (*)(uint8_t version);
using UnitStatFn      = int32_t (*)(void* unit, uint32_t stat, uint16_t layer);

constexpr uintptr_t kApplyProperty        = 0x3d4620;
constexpr uint8_t   kApplyPropertyEntry[] = { 0x48, 0x8b, 0xc4, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0xb4, 0x24, 0xc0, 0x00, 0x00, 0x00 };
constexpr uintptr_t kItemVersion          = 0x34a0e0;
constexpr uint8_t   kItemVersionEntry[]   = { 0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xc9, 0x75, 0x1a };
constexpr uintptr_t kPropertyRow          = 0x3d3210;
constexpr uint8_t   kPropertyRowEntry[]   = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x63, 0xda };
constexpr uintptr_t kPropertyCount        = 0x3d32d0;
constexpr uint8_t   kPropertyCountEntry[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xe8 };
constexpr uintptr_t kUnitStat             = 0x2f5020; // the loader's own jump to its stat code
constexpr uint8_t   kUnitStatEntry[]      = { 0xff, 0x25 };

std::atomic<ApplyPropertyFn> g_originalApply { nullptr };
ItemVersionFn      g_itemVersion   = nullptr;
PropertyRowFn      g_propertyRow   = nullptr;
PropertyCountFn    g_propertyCount = nullptr;
UnitStatFn         g_unitStat      = nullptr;
exalted::TwinTable g_twins;
char               g_stripState[220] = "off";
bool               g_stripPlaced     = false;  // the hook is in the game's code (placed before the tables were built)
char               g_stripRefused[160] = "";   // why it is not, when it was wanted
std::atomic<bool>  g_stripArmed { false };     // it takes restores off (the tables of this start allow it)
std::atomic<bool>  g_stripPending { false };   // the loader is placing it; its way back to the game's routine comes right after
unsigned           g_stripped        = 0; // restores taken off since the game started
exalted::StatIds   g_statIds;              // the numbers of the marker and roll stats (stat-ids.txt)

struct StripEngine {
	int   kind;
	void *item, *third, *fourth;
	int   index, sixth, state, flags;
	void* tenth;
	int            Version(void* unit) { return g_itemVersion(unit); }
	int            PropertyCount(int version) { return g_propertyCount(static_cast<uint8_t>(version)); }
	const uint8_t* Property(int version, int id) { return g_propertyRow(static_cast<uint8_t>(version), id); }
	int            Stat(void* unit, int stat) { return g_unitStat(unit, static_cast<uint32_t>(stat), 0); }
	void           Apply(const exalted::PropertyCall& call) {
		exalted::PropertyCall copy = call;
		g_originalApply.load(std::memory_order_acquire)(kind, item, third, fourth, index, sixth, &copy, state, flags, tenth);
	}
};

uint64_t HookApplyProperty(int kind, void* item, void* third, void* fourth, int index, int sixth, exalted::PropertyCall* call, int state, int flags, void* tenth) {
	// The hook goes in before the tables are built. While the loader is handing over the way back
	// to the game's routine (right after it placed the hook) a call waits for it; never longer.
	ApplyPropertyFn original = g_originalApply.load(std::memory_order_acquire);
	while (original == nullptr && g_stripPending.load(std::memory_order_acquire)) {
		YieldProcessor();
		original = g_originalApply.load(std::memory_order_acquire);
	}
	if (original == nullptr) {
		return 0; // the loader gave no way back (see PlaceStrip): the call cannot reach the game's routine
	}
	// (min and max are looked at first: nearly every call has an amount and is done here)
	if (call != nullptr && call->min == 0 && call->max == 0 && item != nullptr && g_stripArmed.load(std::memory_order_acquire)) {
		StripEngine engine { kind, item, third, fourth, index, sixth, state, flags, tenth };
		if (exalted::IsStripSign(engine, item, *call, g_statIds)) {
			if (exalted::StripRestore(engine, item, *call, g_twins, g_statIds) > 0) {
				++g_stripped;
			}
			return 0; // the sign itself adds nothing
		}
	}
	return original(kind, item, third, fourth, index, sixth, call, state, flags, tenth);
}

// Placed before the tables are built, so they can go by whether it is there: without it, the
// recipes back to elite take only items that were never restored (see Config::stripHooked).
// Through the loader's mutation transaction where it has one, else its plain hook call.
void PlaceStrip(const D2RL::PluginContext* context) {
	g_stripPlaced = false;
	if (!g_config.enabled || !g_config.restoreStrip) {
		return;
	}
	const uintptr_t base = context->exeBase;
	if (base == 0 || ImageStamp(base) != kKnownImageStamp || !context->CheckExpectedBytes(kApplyProperty, kApplyPropertyEntry, sizeof(kApplyPropertyEntry))
	    || !context->CheckExpectedBytes(kItemVersion, kItemVersionEntry, sizeof(kItemVersionEntry)) || !context->CheckExpectedBytes(kPropertyRow, kPropertyRowEntry, sizeof(kPropertyRowEntry))
	    || !context->CheckExpectedBytes(kPropertyCount, kPropertyCountEntry, sizeof(kPropertyCountEntry)) || !context->CheckExpectedBytes(kUnitStat, kUnitStatEntry, sizeof(kUnitStatEntry))) {
		snprintf(g_stripRefused, sizeof(g_stripRefused), "this game build is not the one the routine is known in");
		LogF("back to elite: the game's property routine is not where it is known to be in this game build, so the hook that takes a restore off is left out");
		return;
	}
	g_itemVersion   = reinterpret_cast<ItemVersionFn>(base + kItemVersion);
	g_propertyRow   = reinterpret_cast<PropertyRowFn>(base + kPropertyRow);
	g_propertyCount = reinterpret_cast<PropertyCountFn>(base + kPropertyCount);
	g_unitStat      = reinterpret_cast<UnitStatFn>(base + kUnitStat);
	const D2RL::MutationService* mutation = nullptr;
	if (context->QueryService(&mutation) == D2RL::ServiceQueryResult::Success && D2RL::HasMutationServiceField(mutation, offsetof(D2RL::MutationService, cancelTransaction) + sizeof(void*))) {
		D2RL::Mutations::TransactionHandle transaction = D2RL::Mutations::InvalidTransactionHandle;
		D2RL::Mutations::OperationHandle   operation   = D2RL::Mutations::InvalidOperationHandle;
		D2RL::Mutations::InlineHookRequest request {};
		request.structSize   = D2RL::Mutations::InlineHookRequestSize;
		request.rva          = kApplyProperty;
		request.expected     = kApplyPropertyEntry;
		request.expectedSize = sizeof(kApplyPropertyEntry);
		request.target       = reinterpret_cast<void*>(&HookApplyProperty);
		D2RL::Mutations::CommitResult result {};
		result.structSize = D2RL::Mutations::CommitResultSize;
		void* original    = nullptr;
		g_stripPending.store(true, std::memory_order_release);
		bool  ok          = mutation->beginTransaction(context, &transaction) == D2RL::Mutations::Result::Success
		         && mutation->stageInlineHook(context, transaction, &request, &operation) == D2RL::Mutations::Result::Success;
		const bool committed = ok && mutation->commit(context, transaction, &result) == D2RL::Mutations::Result::Success && result.result == D2RL::Mutations::Result::Success;
		ok = committed && mutation->getInlineHookOriginal(context, transaction, operation, &original) == D2RL::Mutations::Result::Success && original != nullptr;
		if (!committed && transaction != D2RL::Mutations::InvalidTransactionHandle) {
			static_cast<void>(mutation->cancelTransaction(context, transaction));
		}
		if (ok) {
			g_originalApply.store(reinterpret_cast<ApplyPropertyFn>(original), std::memory_order_release);
		}
		g_stripPending.store(false, std::memory_order_release);
		if (committed && !ok) {
			// (cannot happen by the loader's word: a committed hook has its original.) The hook
			// stays in the game's code (a committed change lasts until the plugin unloads), so it
			// can no longer reach the routine: it is taken as absent for this start.
			snprintf(g_stripRefused, sizeof(g_stripRefused), "the loader placed the hook but gave no way back to the game's routine");
			Alert("Exalted Items: WARNING - the loader placed the hook on the game's property routine but gave no way back to it. Back to elite takes only items that were never "
			      "restored this start, and item properties may not be applied: quit the game, and report this with the log.");
			return;
		}
		if (!ok) {
			snprintf(g_stripRefused, sizeof(g_stripRefused), "the loader refused the hook (result %u, reason %u)", static_cast<unsigned>(result.result), static_cast<unsigned>(result.failureReason));
			LogF("back to elite: the loader refused the hook at 0x%llx (result %u, reason %u)", static_cast<unsigned long long>(kApplyProperty), static_cast<unsigned>(result.result),
			     static_cast<unsigned>(result.failureReason));
			return;
		}
	} else {
		ApplyPropertyFn original = nullptr;
		g_stripPending.store(true, std::memory_order_release);
		const bool installed = context->InstallInlineHook(kApplyProperty, kApplyPropertyEntry, sizeof(kApplyPropertyEntry), HookApplyProperty, &original);
		if (installed && original != nullptr) {
			g_originalApply.store(original, std::memory_order_release);
		}
		g_stripPending.store(false, std::memory_order_release);
		if (installed && original == nullptr) {
			snprintf(g_stripRefused, sizeof(g_stripRefused), "the loader placed the hook but gave no way back to the game's routine");
			Alert("Exalted Items: WARNING - the loader placed the hook on the game's property routine but gave no way back to it. Back to elite takes only items that were never "
			      "restored this start, and item properties may not be applied: quit the game, and report this with the log.");
			return;
		}
		if (!installed) {
			snprintf(g_stripRefused, sizeof(g_stripRefused), "the loader refused the hook");
			LogF("back to elite: the loader refused the hook at 0x%llx", static_cast<unsigned long long>(kApplyProperty));
			return;
		}
	}
	g_stripPlaced = true;
	LogF("back to elite: the hook that takes a restore off is in place (before the tables are built)");
}

// After the tables are built: what the hook goes by, and what the status says.
void ArmStrip(const exalted::Summary& summary) {
	g_statIds = summary.statIds;
	if (!g_config.restoreStrip) {
		snprintf(g_stripState, sizeof(g_stripState), "off");
	} else if (!g_stripPlaced) {
		snprintf(g_stripState, sizeof(g_stripState), "NOT active: %s; %s", g_stripRefused[0] != 0 ? g_stripRefused : "the hook is not in place",
		         summary.builtIn ? "this mod's own back-to-elite rows have no check, so a restored item goes back with its restore kept" : "back to elite takes only items that were never restored");
	} else if (summary.rollStats == 0 || !g_statIds.Known()) {
		snprintf(g_stripState, sizeof(g_stripState), "not possible with these tables (see the log)");
	} else {
		g_stripArmed.store(true, std::memory_order_release);
		snprintf(g_stripState, sizeof(g_stripState), "takes a restore off (%d attributes are written down on a restored item)", summary.rollStats);
	}
}

// --- The loot filter's Exalted box -------------------------------------------------------
//
// The game's loot filter knows three tiers of equipment: Normal, Exceptional, Elite. A rule
// names them in "equipmentQuality", and the rule panel has a box for each. The game works a
// base's tier out from its three family codes, so an exalted base, which is none of the
// three, comes out as Normal.
//
// With the box, an exalted base is a tier of its own, written "exalted" in a rule, value 4
// in the game (0 to 2 are the game's; 3 is its "no tier", which a rule refuses). Four calls
// in the game are pointed at the plugin, each through a jump written into unused padding:
//   - the tier of a dropped item (the show/hide decision) and of an entry in the rule
//     panel's item list: an exalted base gives 4, everything else the game's own answer;
//   - the list of tier names, built once on first use: "exalted" = 4 is added, which serves
//     the rule file's reader and writer and the boxes' click messages alike;
//   - the rule panel's table of tier -> box: the box the core added to the layout
//     ("ExaltedQualityChbx") is added for tier 4.
// Everything else (keeping the tier in a rule, matching, ticking the box when a rule is
// opened) is the game's own code, which goes by those tables.
//
// Only in the game image this was written against; every place is checked first.

constexpr int       kExaltedTier     = 4;
constexpr uintptr_t kTierOf          = 0x371d80; // tier of an item row: 0 normal, 1 exceptional, 2 elite
constexpr uint8_t   kTierOfEntry[]   = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc9 };
constexpr uintptr_t kTierNames       = 0x20d830; // fills the name list from an array of (name, tier)
constexpr uint8_t   kTierNamesEntry[] = { 0x49, 0x3b, 0xd0, 0x0f, 0x84, 0xfb, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0xdc };
constexpr uintptr_t kTierBoxes       = 0x173940; // fills a panel's table from an array of (tier, box)
constexpr uint8_t   kTierBoxesEntry[] = { 0x49, 0x3b, 0xd0, 0x0f, 0x84, 0x6f, 0x01, 0x00, 0x00, 0x56, 0x41, 0x54 };
constexpr uintptr_t kFindWidget      = 0x855c90; // a panel's widget by name
constexpr uint8_t   kFindWidgetEntry[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 0x20, 0x4c };
constexpr uintptr_t kToggleType      = 0x882be0; // the type record of a toggle button
constexpr uint8_t   kToggleTypeEntry[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x0d, 0xcb, 0x18, 0x46 };
constexpr uintptr_t kTierNamesGuard  = 0x2a97b40; // not 0 once the name list has been built
constexpr uintptr_t kBoxTableOffset  = 0x1b8;     // the tier -> box table inside the rule panel

struct FilterSite {
	uintptr_t   site;
	uint8_t     call[5];
	uintptr_t   cave;
	const char* what;
};
// (the two tier calls share a jump)
constexpr FilterSite kFilterSites[] = {
	{ 0x1c1235, { 0xe8, 0x46, 0x0b, 0x1b, 0x00 }, 0x3ed270, "the tier of a dropped item" },
	{ 0x151d706, { 0xe8, 0x75, 0x46, 0xe5, 0xfe }, 0x3ed270, "the tier of an entry in the rule panel's list" },
	{ 0x210156, { 0xe8, 0xd5, 0xd6, 0xff, 0xff }, 0x3ed280, "the list of tier names" },
	{ 0x14acd16, { 0xe8, 0x25, 0x6c, 0xcc, 0xfe }, 0x3ed290, "the rule panel's boxes" },
};

using TierOfFn     = int (*)(const uint8_t* row);
using FillFn       = uint64_t (*)(void* table, uint8_t* begin, uint8_t* end);
using FindWidgetFn = void* (*)(void* panel, const char* name);
using TypeFn       = void* (*)();

TierOfFn              g_tierOf      = nullptr;
FillFn                g_tierNames   = nullptr;
FillFn                g_tierBoxes   = nullptr;
FindWidgetFn          g_findWidget  = nullptr;
TypeFn                g_toggleType  = nullptr;
std::vector<uint32_t> g_exaltedCodes;            // every exalted base's code as the game holds it, sorted
char                  g_filterState[200] = "off";
unsigned              g_filterNamed = 0, g_filterBoxes = 0; // times the name was added / the box was found

int HookTierOf(const uint8_t* row) {
	if (row != nullptr) {
		uint32_t code = 0;
		memcpy(&code, row + kKnownCodeOffset, sizeof(code));
		if (std::binary_search(g_exaltedCodes.begin(), g_exaltedCodes.end(), code)) {
			return kExaltedTier;
		}
	}
	return g_tierOf(row);
}

// The game's array: three of (text, tier), 0x30 bytes each. A text is a pointer, a length, a
// capacity whose top bit says "not mine to free", and 16 bytes that hold a short text itself.
uint64_t HookTierNames(void* table, uint8_t* begin, uint8_t* end) {
	constexpr size_t kEntry = 0x30;
	if (begin == nullptr || end != begin + 3 * kEntry) {
		return g_tierNames(table, begin, end);
	}
	alignas(16) uint8_t entries[4 * kEntry] = {};
	memcpy(entries, begin, 3 * kEntry); // (a short text's pointer still points into the game's array, which outlives the call)
	uint8_t*       added    = entries + 3 * kEntry;
	const char     name[]   = "exalted";
	const uint64_t pointer  = reinterpret_cast<uint64_t>(added + 0x18);
	const uint64_t length   = sizeof(name) - 1;
	const uint64_t capacity = 0x800000000000000fULL;
	const int32_t  tier     = kExaltedTier;
	memcpy(added, &pointer, 8);
	memcpy(added + 8, &length, 8);
	memcpy(added + 0x10, &capacity, 8);
	memcpy(added + 0x18, name, sizeof(name));
	memcpy(added + 0x28, &tier, 4);
	++g_filterNamed;
	return g_tierNames(table, entries, entries + 4 * kEntry);
}

// The game's array: three of (tier, box), 16 bytes each. The table is a member of the panel.
uint64_t HookTierBoxes(void* table, uint8_t* begin, uint8_t* end) {
	constexpr size_t kEntry = 0x10;
	if (table == nullptr || begin == nullptr || end != begin + 3 * kEntry) {
		return g_tierBoxes(table, begin, end);
	}
	void* panel = static_cast<uint8_t*>(table) - kBoxTableOffset;
	void* box   = g_findWidget(panel, "ExaltedQualityChbx");
	// as the game does for its own three: only a toggle button will do
	bool toggle = false;
	if (box != nullptr) {
		const void* wanted = g_toggleType();
		using OwnTypeFn    = void* (*)(void* widget);
		void* type         = (*reinterpret_cast<OwnTypeFn**>(box))[0x58 / 8](box);
		for (; type != nullptr && !toggle; type = *reinterpret_cast<void**>(static_cast<uint8_t*>(type) + 0x10)) {
			toggle = type == wanted;
		}
	}
	if (!toggle) {
		return g_tierBoxes(table, begin, end); // a layout without the box: the game's three
	}
	alignas(16) uint8_t entries[4 * kEntry] = {};
	memcpy(entries, begin, 3 * kEntry);
	const int32_t tier = kExaltedTier;
	memcpy(entries + 3 * kEntry, &tier, 4);
	memcpy(entries + 3 * kEntry + 8, &box, 8);
	++g_filterBoxes;
	return g_tierBoxes(table, entries, entries + 4 * kEntry);
}

void InstallFilterBox(const D2RL::PluginContext* context, const exalted::Summary& summary) {
	if (!g_config.filterBox) {
		return;
	}
	if (!summary.filterBox) {
		snprintf(g_filterState, sizeof(g_filterState), "NOT active: the rule panel's layout could not be given the box (see the log); the filter sees exalted bases as Normal");
		return;
	}
	const uintptr_t base = context->exeBase;
	bool            ok   = base != 0 && ImageStamp(base) == kKnownImageStamp && context->CheckExpectedBytes(kTierOf, kTierOfEntry, sizeof(kTierOfEntry))
	          && context->CheckExpectedBytes(kTierNames, kTierNamesEntry, sizeof(kTierNamesEntry)) && context->CheckExpectedBytes(kTierBoxes, kTierBoxesEntry, sizeof(kTierBoxesEntry))
	          && context->CheckExpectedBytes(kFindWidget, kFindWidgetEntry, sizeof(kFindWidgetEntry)) && context->CheckExpectedBytes(kToggleType, kToggleTypeEntry, sizeof(kToggleTypeEntry));
	uint8_t padding[16];
	memset(padding, 0xcc, sizeof(padding));
	for (const FilterSite& site : kFilterSites) {
		ok = ok && context->CheckExpectedBytes(site.site, site.call, sizeof(site.call)) && context->CheckExpectedBytes(site.cave, padding, sizeof(padding));
	}
	if (!ok) {
		snprintf(g_filterState, sizeof(g_filterState), "NOT active: this game build is not the one the filter's code is known in; the box does nothing and the filter sees exalted bases as Normal");
		LogF("loot filter: the game's filter code is not where it is known to be in this game build (or another plugin changed it), so the Exalted box is left without effect");
		return;
	}
	// The name list is built the first time the game needs a tier's name. After that it is too late to add one.
	if (*reinterpret_cast<const int32_t*>(base + kTierNamesGuard) != 0) {
		snprintf(g_filterState, sizeof(g_filterState), "NOT active: the game had already built its list of tier names when the plugin loaded");
		LogF("loot filter: the game had already built its list of tier names, so \"exalted\" cannot be added; the Exalted box is left without effect");
		return;
	}
	g_tierOf     = reinterpret_cast<TierOfFn>(base + kTierOf);
	g_tierNames  = reinterpret_cast<FillFn>(base + kTierNames);
	g_tierBoxes  = reinterpret_cast<FillFn>(base + kTierBoxes);
	g_findWidget = reinterpret_cast<FindWidgetFn>(base + kFindWidget);
	g_toggleType = reinterpret_cast<TypeFn>(base + kToggleType);
	g_exaltedCodes.clear();
	for (const auto& entry : summary.codes) {
		if (!entry.second.empty() && !entry.first.empty() && entry.first[0] != '*') { // (the "*" entries are the crystal and the shard)
			g_exaltedCodes.push_back(FourCC(entry.second));
		}
	}
	std::sort(g_exaltedCodes.begin(), g_exaltedCodes.end());
	g_exaltedCodes.erase(std::unique(g_exaltedCodes.begin(), g_exaltedCodes.end()), g_exaltedCodes.end());

	void* const targets[] = { reinterpret_cast<void*>(&HookTierOf), reinterpret_cast<void*>(&HookTierOf), reinterpret_cast<void*>(&HookTierNames), reinterpret_cast<void*>(&HookTierBoxes) };
	uintptr_t   written   = 0; // the jump of the site before (the two tier calls share one)
	int         index     = 0;
	// The names and the boxes first: a rule that says "exalted" must be readable before an
	// exalted base is given that tier. The two tier calls come last.
	for (const int order : { 2, 3, 0, 1 }) {
		const FilterSite& site = kFilterSites[order];
		if (site.cave != written) {
			uint8_t        jump[14] = { 0xff, 0x25, 0, 0, 0, 0 };
			const uint64_t address  = reinterpret_cast<uint64_t>(targets[order]);
			memcpy(jump + 6, &address, sizeof(address));
			if (!context->PatchBytes(site.cave, padding, sizeof(jump), jump, sizeof(jump))) {
				break;
			}
			written = site.cave;
		}
		uint8_t       call[5]  = { 0xe8 };
		const int32_t relative = static_cast<int32_t>(static_cast<int64_t>(site.cave) - static_cast<int64_t>(site.site + 5));
		memcpy(call + 1, &relative, sizeof(relative));
		if (!context->PatchBytes(site.site, site.call, sizeof(site.call), call, sizeof(call))) {
			break;
		}
		++index;
	}
	if (index == 4) {
		snprintf(g_filterState, sizeof(g_filterState), "an Exalted box in the rule panel, %d bases count as that tier", static_cast<int>(g_exaltedCodes.size()));
	} else if (index >= 2) {
		// the names and boxes are in, a tier call is not: the box can be ticked, exalted bases still count as Normal
		snprintf(g_filterState, sizeof(g_filterState), "NOT complete: the loader refused a change to the game's tier code; the box is there but exalted bases still count as Normal");
		LogF("loot filter: the loader refused the change for %s; exalted bases still count as Normal there", kFilterSites[index == 2 ? 0 : 1].what);
	} else {
		snprintf(g_filterState, sizeof(g_filterState), "NOT active: the loader refused a change to the game's filter code; the filter sees exalted bases as Normal");
		LogF("loot filter: the loader refused the change for %s; the Exalted box is left without effect", kFilterSites[index == 0 ? 2 : 3].what);
	}
}

// --- I Hate TOMLs (optional in-game settings menu) -------------------------------------
//
// When that plugin is installed, every setting shows up in its menu. Without it nothing here
// runs and the settings are edited in exalted-items.toml as usual. Either way a change takes
// effect on the next game start, because the tables are built while the game starts.

struct MenuRow {
	const char* section; // nullptr = header
	const char* key;
	const char* type;    // bool, int, string
	const char* label;
	const char* note;
	int         min, max, step;
};

const MenuRow kMenu[] = {
	{ nullptr, nullptr, "header", "Exalted Items (changes apply on the next game start)", nullptr, 0, 0, 0 },
	{ "exalted-items", "enabled", "bool", "Enabled", "Off: the game reads the mod's files as they are.", 0, 1, 1 },
	{ "exalted-items", "drop_level_max", "int", "Highest item level", "The tier fills ten level tables down from here, three levels apart (96 = item levels 69 to 96).", exalted::kDropLevelFloor,
	  exalted::kDropLevelCeiling, 3 },
	{ "exalted-items", "terror_zone_only", "bool", "Drop only in terror zones", "On: exalted bases drop only from monsters in a terror zone. Off: wherever items of their level drop.", 0, 1, 1 },
	{ "exalted-items", "unique_drops", "string", "Unique drops", "off, normal, exceptional, elite, all, or a list: whose uniques a dropped exalted base can become.", 0, 0, 0 },
	{ "exalted-items", "set_drops", "string", "Set drops", "off, normal, exceptional, elite, all, or a list: whose set pieces a dropped exalted base can become.", 0, 0, 0 },
	{ "exalted-items", "stat_step_percent", "int", "Stat step %", "100 = the exceptional to elite step again, 0 = same stats as elite.", 0, 300, 5 },
	{ "exalted-items", "level_req_cap", "int", "Highest level requirement", nullptr, 1, 110, 1 },
	{ "exalted-items", "name_prefix", "string", "Name prefix", "Text in front of the elite base's name.", 0, 0, 0 },
	{ nullptr, nullptr, "header", "Loot filter", nullptr, 0, 0, 0 },
	{ "loot_filter", "exalted_box", "bool", "Exalted box in the loot filter", "A fourth Quality box beside Normal, Exceptional and Elite. Off: the filter sees exalted bases as Normal.", 0, 1, 1 },
	{ "loot_filter", "label", "string", "Text beside the box", nullptr, 0, 0, 0 },
	{ nullptr, nullptr, "header", "Cube recipes: elite item + these item codes (empty = off)", nullptr, 0, 0, 0 },
	{ "recipes", "unique_weapon", "string", "Unique weapon", nullptr, 0, 0, 0 },
	{ "recipes", "unique_armor", "string", "Unique armor", nullptr, 0, 0, 0 },
	{ "recipes", "set_weapon", "string", "Set weapon", nullptr, 0, 0, 0 },
	{ "recipes", "set_armor", "string", "Set armor", nullptr, 0, 0, 0 },
	{ "recipes", "rare_weapon", "string", "Rare weapon", nullptr, 0, 0, 0 },
	{ "recipes", "rare_armor", "string", "Rare armor", nullptr, 0, 0, 0 },
	{ "recipes", "magic_weapon", "string", "Magic weapon", nullptr, 0, 0, 0 },
	{ "recipes", "magic_armor", "string", "Magic armor", nullptr, 0, 0, 0 },
	{ "recipes", "crafted_weapon", "string", "Crafted weapon", nullptr, 0, 0, 0 },
	{ "recipes", "crafted_armor", "string", "Crafted armor", nullptr, 0, 0, 0 },
	{ "recipes", "normal_weapon", "string", "Normal weapon", nullptr, 0, 0, 0 },
	{ "recipes", "normal_armor", "string", "Normal armor", nullptr, 0, 0, 0 },
	{ nullptr, nullptr, "header", "Back to elite: any exalted item + these item codes (empty = off)", nullptr, 0, 0, 0 },
	{ "recipes", "downgrade", "string", "Exalted to elite", "desecration is the Shard of Desecration (r01 would be the El rune). The item stays what it is, on its elite base.", 0, 0, 0 },
	{ nullptr, nullptr, "header", "Restoration Crystal (in a recipe above, the word crystal stands for it)", nullptr, 0, 0, 0 },
	{ "crystal", "enabled", "bool", "Crystal", "Off: there is no such item, and recipes that name it are left out.", 0, 1, 1 },
	{ "crystal", "name", "string", "Name", nullptr, 0, 0, 0 },
	{ "crystal", "name_color", "string", "Name colour", "orange (as essences, keys and Worldstone Shards), white, red, green, blue, gold, grey, tan, yellow, darkgreen, purple.", 0, 0, 0 },
	{ "crystal", "drop_weight_percent", "int", "Boss drop weight %", "Hell act bosses drop it. 100 = as often as their essence, 0 = never.", 0, 1000, 10 },
	{ "crystal", "stash_slot", "bool", "Stacks in the stash", "A slot of its own in the stash's stacking tabs, beside the Worldstone Shards. Off: an ordinary item.", 0, 1, 1 },
	{ "crystal", "stash_slot_right", "int", "Stash slot: slots to the right", "Counted from the Western Worldstone Shard's slot. 0 and 0: a free place near it.", -20, 20, 1 },
	{ "crystal", "stash_slot_down", "int", "Stash slot: slots down", nullptr, -20, 20, 1 },
	{ nullptr, nullptr, "header", "Shard of Desecration (in a recipe, the word desecration stands for it)", nullptr, 0, 0, 0 },
	{ "desecration", "enabled", "bool", "Shard", "Off: there is no such item, and recipes that name it are left out.", 0, 1, 1 },
	{ "desecration", "name", "string", "Name", nullptr, 0, 0, 0 },
	{ "desecration", "name_color", "string", "Name colour", "orange (as essences, keys and Worldstone Shards), white, red, green, blue, gold, grey, tan, yellow, darkgreen, purple.", 0, 0, 0 },
	{ "desecration", "drop_weight_percent", "int", "Boss drop weight %", "The bosses that drop the crystal drop it. 100 = as often as their essence, 0 = never.", 0, 1000, 10 },
	{ "desecration", "stash_slot", "bool", "Stacks in the stash", nullptr, 0, 1, 1 },
	{ "desecration", "stash_slot_right", "int", "Stash slot: slots to the right", "Counted from the Western Worldstone Shard's slot. 0 and 0: a free place near it.", -20, 20, 1 },
	{ "desecration", "stash_slot_down", "int", "Stash slot: slots down", nullptr, -20, 20, 1 },
	{ nullptr, nullptr, "header", "Restoring: exalted item + crystal adds these, once per item", nullptr, 0, 0, 0 },
	{ "restore", "enabled", "bool", "Restoring", nullptr, 0, 1, 1 },
	{ "restore", "material", "string", "Material", "Item codes; crystal stands for the Restoration Crystal.", 0, 0, 0 },
	{ "restore", "show_line", "bool", "Show \"Restored\" on the item", nullptr, 0, 1, 1 },
	{ "restore", "strip_on_downgrade", "bool", "Back to elite takes the restore off", "The item loses what the restore added and can be restored again. Off: it keeps the attributes.", 0, 1, 1 },
};

// The fixed rows above, then one switch per attribute of every kind of item (exalted_core.h).
std::vector<MenuRow>     g_menu;
std::vector<std::string> g_menuSections; // "restore.<kind>": the text the rows point at

void BuildMenu() {
	if (!g_menu.empty()) {
		return;
	}
	g_menu.assign(std::begin(kMenu), std::end(kMenu));
	g_menuSections.reserve(std::size(exalted::kRestoreKinds));
	for (const exalted::RestoreKind& kind : exalted::kRestoreKinds) {
		g_menuSections.push_back(std::string("restore.") + kind.key);
		g_menu.push_back({ nullptr, nullptr, "header", kind.label, nullptr, 0, 0, 0 });
		for (const exalted::RestoreAttribute& attribute : kind.attributes) {
			g_menu.push_back({ g_menuSections.back().c_str(), attribute.key, "bool", attribute.label, nullptr, 0, 1, 1 });
		}
	}
}

struct MenuRegistration {
	uint32_t    structSize;
	uint32_t    flags;
	const char* description;
	bool (*get)(const char* key, char* out, uint32_t outSize);
	bool (*set)(const char* key, const char* value, char* error, uint32_t errorSize);
	void* reserved;
};

char             g_menuText[32768];
MenuRegistration g_menuRegistration {};
bool             g_menuJoined = false;

// Menu keys are "<section>.<key>".
const MenuRow* MenuFind(const char* key) {
	if (key == nullptr) {
		return nullptr;
	}
	BuildMenu();
	for (const MenuRow& row : g_menu) {
		if (row.section == nullptr) {
			continue;
		}
		const size_t length = strlen(row.section);
		if (strncmp(key, row.section, length) == 0 && key[length] == '.' && strcmp(key + length + 1, row.key) == 0) {
			return &row;
		}
	}
	return nullptr;
}

std::string MenuValue(const MenuRow& row) {
	const std::string key     = row.key;
	const std::string section = row.section;
	if (section == "crystal" || section == "desecration") {
		const bool crystal = section == "crystal";
		if (key == "enabled") return (crystal ? g_config.crystalEnabled : g_config.desecrationEnabled) ? "true" : "false";
		if (key == "name") return crystal ? g_config.crystalName : g_config.desecrationName;
		if (key == "name_color") return ColourWord(crystal ? g_config.crystalColor : g_config.desecrationColor);
		if (key == "stash_slot") return (crystal ? g_config.crystalStashSlot : g_config.desecrationStashSlot) ? "true" : "false";
		if (key == "stash_slot_right") return std::to_string(crystal ? g_config.crystalSlotRight : g_config.desecrationSlotRight);
		if (key == "stash_slot_down") return std::to_string(crystal ? g_config.crystalSlotDown : g_config.desecrationSlotDown);
		return std::to_string(crystal ? g_config.crystalDropPercent : g_config.desecrationDropPercent);
	}
	if (section == "loot_filter") {
		return key == "label" ? g_config.filterLabel : g_config.filterBox ? "true" : "false";
	}
	if (section == "restore") {
		return key == "enabled" ? (g_config.restoreEnabled ? "true" : "false") : key == "material" ? g_config.restoreMaterial : key == "strip_on_downgrade" ? (g_config.restoreStrip ? "true" : "false") : (g_config.restoreShowLine ? "true" : "false");
	}
	if (section.compare(0, 8, "restore.") == 0) {
		const auto& off = g_config.restoreOff;
		return std::find(off.begin(), off.end(), section.substr(8) + "." + key) == off.end() ? "true" : "false";
	}
	if (std::string(row.section) == "recipes" && key == "downgrade") {
		return g_config.downgrade;
	}
	if (std::string(row.section) == "recipes") {
		std::string value;
		for (const exalted::Recipe& recipe : g_config.recipes) {
			value = recipe.key == key ? recipe.materials : value;
		}
		return value;
	}
	if (key == "enabled") return g_config.enabled ? "true" : "false";
	if (key == "terror_zone_only") return g_config.terrorZoneOnly ? "true" : "false";
	if (key == "drop_level_max") return std::to_string(g_config.dropLevelMax);
	if (key == "unique_drops") return TierWords(g_config.uniqueTiers);
	if (key == "set_drops") return TierWords(g_config.setTiers);
	if (key == "stat_step_percent") return std::to_string(g_config.statStepPercent);
	if (key == "level_req_cap") return std::to_string(g_config.levelReqCap);
	if (key == "name_prefix") return g_config.namePrefix;
	return {};
}

bool MenuGet(const char* key, char* out, uint32_t outSize) {
	const MenuRow* row = MenuFind(key);
	if (row == nullptr || out == nullptr || outSize == 0) {
		return false;
	}
	snprintf(out, outSize, "%s", MenuValue(*row).c_str());
	return true;
}

// Replaces one "key = value" line of the config file, keeping every comment.
void SaveConfigValue(const char* section, const char* key, const std::string& value) {
	static char text[32768];
	memset(text, 0, sizeof(text));
	if (!g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		return;
	}
	std::string       out;
	const std::string wanted = key;
	bool              inSection = false, done = false, seen = false;
	const char*       line = text;
	while (*line != '\0') {
		const char*  end    = strchr(line, '\n');
		const size_t length = end != nullptr ? static_cast<size_t>(end - line) + 1 : strlen(line);
		const char*  c      = line;
		while (*c == ' ' || *c == '\t') {
			++c;
		}
		if (*c == '[') {
			if (inSection && !done) {
				out += wanted + " = " + value + "\r\n";
				done = true;
			}
			const size_t sectionLength = strlen(section);
			inSection                  = strncmp(c + 1, section, sectionLength) == 0 && c[1 + sectionLength] == ']';
			seen                       = seen || inSection;
		} else if (inSection && !done && strncmp(c, key, wanted.size()) == 0) {
			const char* after = c + wanted.size();
			while (*after == ' ' || *after == '\t') {
				++after;
			}
			if (*after == '=') {
				const bool crlf = length >= 2 && line[length - 2] == '\r';
				out += wanted + " = " + value + (end != nullptr ? (crlf ? "\r\n" : "\n") : "");
				done = true;
				line += length;
				continue;
			}
		}
		out.append(line, length);
		line += length;
	}
	if (!done) {
		out += seen ? wanted + " = " + value + "\r\n" : std::string("\r\n[") + section + "]\r\n" + wanted + " = " + value + "\r\n";
	}
	static_cast<void>(g_ctx->WriteConfig(out.c_str()));
}

exalted::Config LoadConfig();

bool MenuSet(const char* key, const char* value, char* error, uint32_t errorSize) {
	const MenuRow* row = MenuFind(key);
	if (row == nullptr || value == nullptr) {
		if (error != nullptr && errorSize > 0) {
			snprintf(error, errorSize, "unknown setting");
		}
		return false;
	}
	std::string text = value;
	while (!text.empty() && (text.front() == ' ' || text.front() == '"')) {
		text.erase(text.begin());
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '"' || text.back() == '\r' || text.back() == '\n')) {
		text.pop_back();
	}
	const std::string type = row->type;
	std::string       stored;
	if (type == "bool") {
		stored = text == "true" || text == "1" ? "true" : "false";
	} else if (type == "int") {
		int number = atoi(text.c_str());
		number     = number < row->min ? row->min : number > row->max ? row->max : number;
		stored     = std::to_string(number);
	} else {
		const std::string name = row->key;
		if (name == "name_color") {
			if (ColourCode(text) < 0) {
				if (error != nullptr && errorSize > 0) {
					snprintf(error, errorSize, "use one of: %s", kColourList);
				}
				return false;
			}
			text = ColourWord(ColourCode(text));
		}
		if (name == "unique_drops" || name == "set_drops") {
			const int tiers = ParseTiers(text);
			if (tiers < 0) {
				if (error != nullptr && errorSize > 0) {
					snprintf(error, errorSize, "use off, normal, exceptional, elite, all, or a list such as: exceptional, elite");
				}
				return false;
			}
			text = TierWords(tiers);
		}
		if (text.find('"') != std::string::npos || text.find('\\') != std::string::npos || text.size() > 60) {
			if (error != nullptr && errorSize > 0) {
				snprintf(error, errorSize, "up to 60 characters, without quotes or backslashes");
			}
			return false;
		}
		if (name == "name_prefix" && std::string(value).find(text + " ") != std::string::npos) {
			text += " "; // keep the space between the prefix and the name
		}
		stored = "\"" + text + "\"";
	}
	SaveConfigValue(row->section, row->key, stored);
	g_config = LoadConfig(); // what the menu shows; the tables change on the next start
	return true;
}

void JoinSettingsMenu() {
	if (g_menuJoined) {
		return;
	}
	const D2RL::PluginCommunicationService* communication = nullptr;
	if (g_ctx->QueryService(&communication) != D2RL::ServiceQueryResult::Success || communication == nullptr
		|| communication->serviceSize < offsetof(D2RL::PluginCommunicationService, releaseService) + sizeof(void*) || communication->acquireService == nullptr) {
		return;
	}
	D2RL::PluginCommunication::AcquireServiceRequest request {};
	request.structSize       = D2RL::PluginCommunication::AcquireServiceRequestSize;
	request.providerPluginId = "i-hate-tomls";
	request.name             = "settings-menu";
	request.minimumVersion   = 1;
	request.minimumTableSize = 0x30;
	D2RL::PluginCommunication::AcquiredService acquired {};
	acquired.structSize = D2RL::PluginCommunication::AcquiredServiceSize;
	if (communication->acquireService(g_ctx, &request, &acquired) != D2RL::PluginCommunication::Result::Success || acquired.table == nullptr) {
		return; // I Hate TOMLs is not installed, or has not loaded yet
	}
	std::string text = "title = \"Exalted Items\"\n";
	BuildMenu();
	for (const MenuRow& row : g_menu) {
		text += "[[setting]]\n";
		if (row.section != nullptr) {
			text += std::string("key = \"") + row.section + "." + row.key + "\"\n";
		}
		std::string label = row.label;
		for (size_t at = label.find('"'); at != std::string::npos; at = label.find('"', at + 2)) {
			label.insert(at, "\\");
		}
		text += std::string("type = \"") + row.type + "\"\nlabel = \"" + label + "\"\n";
		if (row.note != nullptr) {
			text += std::string("note = \"") + row.note + "\"\n";
		}
		if (std::string(row.type) == "int") {
			text += "min = " + std::to_string(row.min) + "\nmax = " + std::to_string(row.max) + "\nstep = " + std::to_string(row.step) + "\nshift_step = " + std::to_string(row.step * 5)
			      + "\nctrl_step = " + std::to_string(row.step * 10) + "\n";
		}
	}
	snprintf(g_menuText, sizeof(g_menuText), "%s", text.c_str());
	g_menuRegistration.structSize  = 0x28;
	g_menuRegistration.flags       = 0;
	g_menuRegistration.description = g_menuText;
	g_menuRegistration.get         = MenuGet;
	g_menuRegistration.set         = MenuSet;
	g_menuRegistration.reserved    = nullptr;
	using AddFn     = void* (*)(const D2RL::PluginContext* context, const MenuRegistration* registration);
	const AddFn add = *reinterpret_cast<const AddFn*>(static_cast<const char*>(acquired.table) + 8);
	if (add == nullptr) {
		return;
	}
	g_menuJoined = add(g_ctx, &g_menuRegistration) != nullptr;
	LogF("settings menu: %s", g_menuJoined ? "joined I Hate TOMLs" : "I Hate TOMLs refused the registration");
}

void __cdecl OnGameJoined(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
	// In case I Hate TOMLs loaded after this plugin. By the first area change every plugin has
	// loaded, so one try is enough; later calls return at once.
	static bool tried = false;
	if (!tried) {
		tried = true;
		JoinSettingsMenu();
	}
}

// 'exalted legend': the item code of every exalted base and material, written to legend.txt
// beside codes.txt (replaced each time), from what this start built.
void WriteLegend() {
	if (g_legendPath.empty() || g_legend.legend.empty()) {
		g_ctx->WriteConsoleMessage("Exalted Items: no legend - no exalted items were made this start (see 'exalted').");
		return;
	}
	SYSTEMTIME now {};
	GetLocalTime(&now);
	char heading[1200];
	snprintf(heading, sizeof(heading),
	         "Exalted Items %s: item code legend\n"
	         "Written %04u-%02u-%02u %02u:%02u by the 'exalted legend' console command, from the tables built at this game start.\n"
	         "Codes are this computer's, kept in codes.txt in this folder. A code never changes once it is there. On another\n"
	         "computer with other mods a few codes can differ: run 'exalted legend' there to see its own.\n"
	         "Use a code wherever the game wants an item code (cube recipes, treasure classes). The elite items keep their own codes.\n"
	         "Req = required level. \"Can drop as\" = the item qualities the exalted base can drop with besides normal, magic and rare.",
	         kPluginVersion, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
	const std::string  text = exalted::FormatLegend(g_legend, g_legendConfig, heading);
	const std::wstring temp = g_legendPath + L".xlt-tmp";
	const HANDLE       file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD              written = 0;
	const bool         ok      = file != INVALID_HANDLE_VALUE && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size();
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
	}
	char line[900];
	if (!ok || MoveFileExW(temp.c_str(), g_legendPath.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
		const DWORD error = GetLastError();
		DeleteFileW(temp.c_str());
		snprintf(line, sizeof(line), "Exalted Items: could not write %s (error %lu)", Narrow(g_legendPath).c_str(), error);
	} else {
		snprintf(line, sizeof(line), "Exalted Items: legend of %d exalted items and %d materials written to %s", static_cast<int>(g_legend.legend.size()),
		         (g_legend.crystalCode.empty() ? 0 : 1) + (g_legend.desecrationCode.empty() ? 0 : 1), Narrow(g_legendPath).c_str());
	}
	LogF("%s", line);
	g_ctx->WriteConsoleMessage(line);
}

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	const char* args = command != nullptr && command->args != nullptr ? command->args : "";
	while (*args == ' ') {
		++args;
	}
	char line[700];
	if (_strnicmp(args, "legend", 6) == 0 && (args[6] == '\0' || args[6] == ' ')) {
		WriteLegend();
		return D2RL::ConsoleCommandResult::Handled;
	}
	snprintf(line, sizeof(line), "Exalted Items %s (%s)", kPluginVersion, kBuildId);
	g_ctx->WriteConsoleMessage(line);
	g_ctx->WriteConsoleMessage(g_status);
	for (const std::string& alert : g_alerts) {
		if (alert != g_status) {
			g_ctx->WriteConsoleError(alert.c_str());
		}
	}
	if (!g_legendPath.empty()) {
		g_ctx->WriteConsoleMessage("  'exalted legend' writes every exalted item and its item code to legend.txt beside codes.txt");
	}
	static const char* const kindName[2] = { "unique", "set" };
	for (int kind = 0; kind < 2; ++kind) {
		if (g_originalPick[kind] != nullptr) {
			snprintf(line, sizeof(line), "  %s: %d exalted base(s) rolled %s, %d became one, %d had nothing in the family for their item level (the rest fell back as the game does)", kindName[kind],
			         g_pickRolls[kind].load(), kindName[kind], g_pickMade[kind].load(), g_pickEmpty[kind].load());
			g_ctx->WriteConsoleMessage(line);
			const uint32_t missed = g_pickMiss[kind].code.load(std::memory_order_relaxed);
			if (missed == 0) {
				continue;
			}
			char text[5];
			CodeText(missed, text);
			snprintf(line, sizeof(line), "    last roll the game picked nothing for: posed as base '%s' at item level %d%s", text, static_cast<int>(g_pickMiss[kind].level.load(std::memory_order_relaxed)),
			         kind == exalted::PickUnique ? " (expected when that unique has already dropped in the game)" : " (not expected)");
		} else {
			snprintf(line, sizeof(line), "  %s drops: %s", kindName[kind], g_pickState[kind]);
		}
		g_ctx->WriteConsoleMessage(line);
	}
	// How often the game has been handed each adjusted copy: 0 means the game never asked for
	// that mod file through a path the redirect recognises.
	for (int i = 0; i < g_redirectCount; ++i) {
		snprintf(line, sizeof(line), "  %s: read %ld time(s)", g_redirects[i].rel.c_str(), static_cast<long>(g_redirects[i].hits));
		g_ctx->WriteConsoleMessage(line);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// --- Exalted base codes -------------------------------------------------------------------
//
// A saved item names its base by item code, so the code of an exalted base must never change.
// The codes handed out are kept in codes.txt in the plugin's folder ("elite=exalted", one per
// line) and given back to the generator on every start. A base that is gone for a while (its
// mod was switched off) keeps its line, and its code is not given to anything else.

using CodeList = exalted::CodeList;

void ReadCodes(const std::wstring& path, CodeList& codes) {
	std::string text;
	if (!GameIo::ReadWhole(path, text)) {
		return;
	}
	std::string line;
	for (const char c : text + "\n") {
		if (c != '\n') {
			if (c != '\r' && c != ' ' && c != '\t') {
				line += c;
			}
			continue;
		}
		const size_t equals = line.find('=');
		if (!line.empty() && line[0] != '#' && equals != std::string::npos && equals > 0 && equals + 1 < line.size()) {
			codes.emplace_back(line.substr(0, equals), line.substr(equals + 1));
		}
		line.clear();
	}
}

void SaveCodes(const std::wstring& path, const CodeList& before, const CodeList& now) {
	CodeList all = now;
	for (const auto& old : before) {
		bool current = false;
		for (const auto& entry : now) {
			current = current || entry.first == old.first;
		}
		if (!current) {
			all.push_back(old); // not in the mod right now: remembered for when it is back
		}
	}
	std::sort(all.begin(), all.end());
	std::string text = "# Exalted Items: the item code of each exalted base (elite code = exalted code).\r\n"
	                   "# Saved items name their base by this code, so a base keeps its code for good.\r\n"
	                   "# Do not edit or delete this file: items on exalted bases could turn into other items.\r\n";
	for (const auto& entry : all) {
		text += entry.first + "=" + entry.second + "\r\n";
	}
	std::string existing;
	if (GameIo::ReadWhole(path, existing) && existing == text) {
		return;
	}
	const std::wstring temp    = path + L".xlt-tmp";
	const HANDLE       file    = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD              written = 0;
	const bool         ok      = file != INVALID_HANDLE_VALUE && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size();
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
	}
	if (!ok || MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
		LogF("could not save the exalted base codes to %s (error %lu). They are worked out again on the next start, which gives the same codes unless the mod's items change.",
		     Narrow(path).c_str(), GetLastError());
		DeleteFileW(temp.c_str());
	}
}

// Writes a file that must not exist yet (stat-ids.txt is never replaced by the plugin).
bool WriteNewFile(const std::wstring& path, const std::string& text) {
	const HANDLE file    = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD        written = 0;
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size();
	CloseHandle(file);
	if (!ok) {
		DeleteFileW(path.c_str());
	}
	return ok;
}

// --- Never bricking: previous copies, the saves, the characters named ------------------------
//
// Saved items name exalted bases by code (codes.txt) and restores by stat number (stat-ids.txt).
// If either file is lost, the tables a start builds could read those items as something else,
// and if Exalted does not start its tier at all, characters holding exalted items do not load.
// So a missing file is recovered, in this order: its previous copy (backups\ beside it; reinstalling
// the mods rebuilds only the mod's .mpq folder, never this one), then the saves (read only):
// the tables of this start are only used when every save reads with them to its end. Only when
// neither works does Exalted refuse, naming the characters that hold exalted items.

void Alert(const std::string& text) {
	LogF("%s", text.c_str());
	if (std::find(g_alerts.begin(), g_alerts.end(), text) == g_alerts.end()) {
		g_alerts.push_back(text);
	}
	if (g_ctx != nullptr) {
		g_ctx->WriteConsoleError(text.c_str());
	}
}

// The game's save folder for the active mod (<Saved Games>\Diablo II Resurrected\mods\<mod>).
// EXALTED_SAVES_DIR names another one (the tests).
std::wstring SavesFolder(const D2RL::PluginContext* context) {
	wchar_t     own[1024];
	const DWORD n = GetEnvironmentVariableW(L"EXALTED_SAVES_DIR", own, 1024);
	if (n > 0 && n < 1024) {
		return own;
	}
	PWSTR        known = nullptr;
	std::wstring folder;
	if (SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &known) == S_OK && known != nullptr) {
		folder = std::wstring(known) + L"\\Diablo II Resurrected";
		if (context->activeMod != nullptr && context->activeMod[0] != '\0') {
			folder += L"\\mods\\" + Widen(context->activeMod);
		}
	}
	CoTaskMemFree(known);
	return folder;
}

std::vector<exalted::SaveFile> ReadSaves(const std::wstring& folder) {
	std::vector<exalted::SaveFile> files;
	for (const wchar_t* pattern : { L"\\*.d2s", L"\\*.d2i" }) {
		WIN32_FIND_DATAW found {};
		const HANDLE     find = FindFirstFileW((folder + pattern).c_str(), &found);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			exalted::SaveFile file;
			file.name = Narrow(found.cFileName);
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && GameIo::ReadWhole(folder + L"\\" + found.cFileName, file.bytes)) {
				files.push_back(std::move(file));
			}
		} while (FindNextFileW(find, &found) != 0);
		FindClose(find);
	}
	std::sort(files.begin(), files.end(), [](const exalted::SaveFile& a, const exalted::SaveFile& b) { return a.name < b.name; });
	return files;
}

// The tables the game reads this start (plain = false: the adjusted copies where there are
// some), or the mod's own (plain = true), as the save reader needs them.
exalted::SaveTables TablesFor(GameIo& io, const exalted::Summary& summary, bool plain) {
	exalted::SaveTables                       tables;
	const std::pair<const char*, std::string*> files[] = { { "global/excel/armor.txt", &tables.armor }, { "global/excel/weapons.txt", &tables.weapons },
		                                                   { "global/excel/misc.txt", &tables.misc }, { "global/excel/itemstatcost.txt", &tables.stats } };
	for (const auto& file : files) {
		const bool copy = !plain && std::find(summary.files.begin(), summary.files.end(), file.first) != summary.files.end();
		if (!(copy && io.ReadCopy(file.first, *file.second)) && !io.ReadMod(file.first, *file.second)) {
			io.ReadBase(file.first, *file.second);
		}
	}
	return tables;
}

// "Hellpal, Femboys and the shared stash (SharedStashSoftCoreV2)"
std::string Characters(const std::vector<std::string>& files) {
	std::vector<std::string> names;
	for (const std::string& file : files) {
		const std::string name = exalted::SaveLabel(file.substr(0, file.find(':')));
		if (std::find(names.begin(), names.end(), name) == names.end()) {
			names.push_back(name);
		}
	}
	std::string text;
	for (size_t i = 0; i < names.size(); ++i) {
		text += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
	}
	return text.empty() ? "no character" : text;
}

bool ReplaceFile(const std::wstring& path, const std::string& text) {
	const std::wstring temp    = path + L".xlt-tmp";
	const HANDLE       file    = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD              written = 0;
	const bool         ok      = file != INVALID_HANDLE_VALUE && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size();
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
	}
	if (!ok || MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
		DeleteFileW(temp.c_str());
		return false;
	}
	return true;
}

// The previous copy of codes.txt or stat-ids.txt: backups\<name>, the same as the file after a
// start that worked.
void KeepPreviousCopy(const std::wstring& folder, const wchar_t* name) {
	std::string now, kept;
	if (!GameIo::ReadWhole(folder + L"\\" + name, now)) {
		return;
	}
	const std::wstring copy = folder + L"\\backups\\" + name;
	if (GameIo::ReadWhole(copy, kept) && kept == now) {
		return;
	}
	MakeFolders(folder + L"\\backups");
	if (!ReplaceFile(copy, now)) {
		LogF("could not keep a previous copy of %s in %s (error %lu)", Narrow(name).c_str(), Narrow(folder + L"\\backups").c_str(), GetLastError());
	}
}

std::wstring Stamp() {
	SYSTEMTIME now {};
	GetLocalTime(&now);
	wchar_t text[32];
	swprintf(text, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	return text;
}

struct Ledger {
	bool        codesKnown   = false; // codes.txt was read (here, at the old place, or a previous copy)
	bool        statIdsKnown = false;
	std::string statIdsProblem;       // stat-ids.txt is there and cannot be read (and nothing else could be)
};

// codes.txt and stat-ids.txt into `config`: from the plugin's folder; else from the plugin's old
// place (it was moved between a mod's loader folder and the game's); else from the previous copy.
// One taken from elsewhere is put in place (a copy; the source stays).
Ledger ReadLedger(const std::wstring& folder, const std::wstring& other, exalted::Config& config) {
	Ledger ledger;
	struct Source {
		std::wstring dir;
		const char*  what;
		bool         previous;
	};
	const Source sources[] = { { folder, "", false }, { other, "the plugin's old place", false }, { folder + L"\\backups", "its previous copy", true },
		                       { other + L"\\backups", "its previous copy at the plugin's old place", true } };
	const auto putInPlace = [&](const std::wstring& from, const wchar_t* name, const Source& source) {
		const std::wstring to = folder + L"\\" + name;
		MakeFolders(folder);
		if (IsFile(to)) {
			MoveFileExW(to.c_str(), (to + L".unreadable-" + Stamp()).c_str(), 0); // (kept aside, not deleted)
		}
		const bool copied = CopyFileW(from.c_str(), to.c_str(), TRUE) != 0;
		char       line[900];
		snprintf(line, sizeof(line), "%s %s from %s%s", Narrow(name).c_str(), source.previous ? "was missing or could not be read: put back" : "taken over", source.what,
		         copied ? "" : " (it could not be copied there; read from where it is)");
		if (source.previous) {
			Alert(std::string("Exalted Items: ") + line + ".");
		} else {
			LogF("%s (%s)", line, Narrow(from).c_str());
		}
	};
	for (const Source& source : sources) {
		const std::wstring path = source.dir + L"\\codes.txt";
		if (source.dir.empty() || source.dir == L"\\backups" || !IsFile(path)) {
			continue;
		}
		ReadCodes(path, config.savedCodes);
		ledger.codesKnown = true;
		if (source.dir != folder) {
			putInPlace(path, L"codes.txt", source);
		}
		break;
	}
	for (const Source& source : sources) {
		const std::wstring path = source.dir + L"\\stat-ids.txt";
		std::string        text;
		if (source.dir.empty() || source.dir == L"\\backups" || !GameIo::ReadWhole(path, text)) {
			continue;
		}
		std::string      why;
		exalted::StatIds ids;
		if (!exalted::ParseStatIds(text, ids, why)) {
			LogF("%s cannot be read (%s)", Narrow(path).c_str(), why.c_str());
			if (ledger.statIdsProblem.empty()) {
				ledger.statIdsProblem = "stat-ids.txt cannot be read (" + why + ")";
			}
			continue;
		}
		config.savedStatIds = ids;
		ledger.statIdsKnown = true;
		if (source.dir != folder) {
			putInPlace(path, L"stat-ids.txt", source);
		}
		break;
	}
	return ledger;
}

// Runs the generator when codes.txt or stat-ids.txt could not be read at all: with the codes in
// the tables the last start stored (else codes worked out anew), and the stat layout of 1.3.0's
// rule (else the other one). The first that every save reads with is taken. `refusal` is set
// when none is: the start must not go on.
exalted::Summary RecoverAndRun(GameIo& io, exalted::Config& config, const Ledger& ledger, const std::wstring& savesFolder, const std::wstring& pluginFolder, std::string& refusal) {
	const double                         started = Clock();
	const std::vector<exalted::SaveFile> saves   = ReadSaves(savesFolder);
	std::vector<exalted::CodeList>       codeSets;
	if (ledger.codesKnown) {
		codeSets.push_back(config.savedCodes);
	} else {
		std::string armor, weapons, misc;
		const bool  stored = io.ReadCopy("global/excel/armor.txt", armor) | io.ReadCopy("global/excel/weapons.txt", weapons);
		if (stored) {
			io.ReadCopy("global/excel/misc.txt", misc) || io.ReadMod("global/excel/misc.txt", misc) || io.ReadBase("global/excel/misc.txt", misc);
			if (armor.empty()) {
				io.ReadMod("global/excel/armor.txt", armor) || io.ReadBase("global/excel/armor.txt", armor);
			}
			if (weapons.empty()) {
				io.ReadMod("global/excel/weapons.txt", weapons) || io.ReadBase("global/excel/weapons.txt", weapons);
			}
			exalted::CodeList found = exalted::CodesInTables(armor, weapons, misc);
			if (!found.empty()) {
				LogF("codes.txt and its previous copy are missing: %zu codes found in the tables stored at the last start", found.size());
				codeSets.push_back(std::move(found));
			}
		}
		codeSets.emplace_back(); // codes worked out anew
	}
	std::vector<exalted::Config::NewLayout> layouts;
	if (ledger.statIdsKnown) {
		layouts.push_back(exalted::Config::NewLayout::Auto); // (not used: the map is known)
	}
	LogF("%s%s%s: checking every save (%zu files in %s) against the tables before they are used", ledger.codesKnown ? "" : "codes.txt", !ledger.codesKnown && !ledger.statIdsKnown ? " and " : "",
	     ledger.statIdsKnown ? "" : "stat-ids.txt", saves.size(), Narrow(savesFolder).c_str());
	exalted::Summary         summary;
	std::vector<std::string> unreadable;
	std::string              taken;
	for (size_t c = 0; c < codeSets.size() && taken.empty(); ++c) {
		std::vector<exalted::Config::NewLayout> tries = layouts;
		if (tries.empty()) { // 1.3.0's rule first, then the other layout
			const bool legacyFirst = !codeSets[c].empty();
			tries                  = { legacyFirst ? exalted::Config::NewLayout::Legacy : exalted::Config::NewLayout::Packed, legacyFirst ? exalted::Config::NewLayout::Packed : exalted::Config::NewLayout::Legacy };
		}
		for (const exalted::Config::NewLayout layout : tries) {
			config.savedCodes    = codeSets[c];
			config.newStatLayout = layout;
			io.alerts.clear();
			summary = exalted::Run(io, config);
			if (!summary.ok) {
				return summary;
			}
			if (summary.builtIn || saves.empty()) {
				taken = summary.builtIn ? "the mod has Exalted's rows in its own tables" : "there are no saves yet";
				break;
			}
			std::set<std::string> exaltedCodes;
			for (const auto& code : summary.codes) {
				exaltedCodes.insert(code.second);
			}
			const exalted::SaveCheck check = exalted::CheckSaves(saves, TablesFor(io, summary, false), TablesFor(io, summary, true), exaltedCodes);
			for (const std::string& skipped : check.skipped) {
				LogF("  save not checked (the save reader cannot read it, with or without Exalted's tables): %s", skipped.c_str());
			}
			const char* codesFrom = ledger.codesKnown ? "codes.txt" : !codeSets[c].empty() ? "the codes of the last start's tables" : "codes worked out anew";
			const char* layoutName = ledger.statIdsKnown ? "stat-ids.txt" : layout == exalted::Config::NewLayout::Legacy ? "1.2.0's stat layout" : "the packed stat layout";
			if (check.Ok()) {
				char line[300];
				snprintf(line, sizeof(line), "%s and %s: all %d saves read with these tables (%zu hold exalted items)", codesFrom, layoutName, check.files - static_cast<int>(check.skipped.size()),
				         check.holders.size());
				taken = line;
				break;
			}
			LogF("  %s and %s: %zu saves would read wrong, e.g. %s", codesFrom, layoutName, check.unreadable.size(), check.unreadable[0].c_str());
			unreadable = check.unreadable;
		}
	}
	if (taken.empty()) {
		std::vector<std::string> holders = exalted::FilesWithUnknownItems(saves, TablesFor(io, summary, true));
		holders.insert(holders.end(), unreadable.begin(), unreadable.end());
		refusal = std::string(ledger.codesKnown ? "" : "codes.txt") + (!ledger.codesKnown && !ledger.statIdsKnown ? " and " : "") + (ledger.statIdsKnown ? "" : "stat-ids.txt") +
		          " could not be recovered (no previous copy, and the saves do not read with the tables Exalted would build now). Exalted items are off, so " + Characters(holders) +
		          " cannot be loaded until those files are put back into " + Narrow(pluginFolder) + " from a backup; the other characters play as usual.";
		return summary;
	}
	const std::vector<std::string> holders = exalted::FilesWithUnknownItems(saves, TablesFor(io, summary, true));
	char                           line[700];
	const bool both = !ledger.codesKnown && !ledger.statIdsKnown;
	snprintf(line, sizeof(line), "Exalted Items: %s%s%s %s, recovered: %s (%.0f ms).", ledger.codesKnown ? "" : "codes.txt", both ? " and " : "", ledger.statIdsKnown ? "" : "stat-ids.txt",
	         !ledger.statIdsProblem.empty() ? "could not be read and had no previous copy" : both ? "were missing and had no previous copies" : "was missing and had no previous copy", taken.c_str(),
	         Clock() - started);
	if (holders.empty()) {
		LogF("%s", line + 15); // (a new install: nothing in the saves depends on them)
	} else {
		Alert(line);
	}
	return summary;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "exalted-items",
	.name        = "Exalted Items",
	.version     = kPluginVersion,
	.author      = "Latedozer",
	.description = "A fourth item tier above elite, built from the active mod's own item tables.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
	.reserved    = {},
};

}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) {
		return false;
	}
	g_ctx = context;
	LogF("Exalted Items %s (%s) loading (D2R %s, SDK %s)", kPluginInfo.version, kBuildId, context->buildVersion != nullptr ? context->buildVersion : "?", D2RL_SDK_VERSION);
	static_cast<void>(context->RegisterConsoleCommand("exalted", OnConsole, "Exalted Items: what was generated, and how often the game used it"));

	g_config = LoadConfig();
	JoinSettingsMenu();
	{
		const D2RL::LifecycleService* lifecycle = nullptr;
		if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, offsetof(D2RL::LifecycleService, unregisterGameplayEventListener))) {
			// The game's tables can be loaded again; the hook then reads its item numbers again.
			D2RL::Lifecycle::DataTablesLoadedListener tables {};
			tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
			tables.callback   = OnTablesLoaded;
			D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
			static_cast<void>(lifecycle->registerDataTablesLoadedListener(context, &tables, &handle));
			if (!g_menuJoined) {
				D2RL::Lifecycle::GameplayEventListener listener {};
				listener.structSize = D2RL::Lifecycle::GameplayEventListenerSize;
				listener.kind       = D2RL::Lifecycle::GameplayEventKind::LevelChanged;
				listener.callback   = OnGameJoined;
				handle              = D2RL::Lifecycle::InvalidHandle;
				static_cast<void>(lifecycle->registerGameplayEventListener(context, &listener, &handle));
			}
		}
	}
	GameIo      io;
	std::string problem;
	// (a folder that is there can be missed once by the file system: tried again before the tier
	// is left off, which would keep characters holding exalted items from loading)
	bool found = FindDataRoot(context, io.dataRoot, problem);
	for (int retry = 1; !found && retry <= 3 && problem.rfind("could not find the data folder", 0) == 0; ++retry) {
		LogF("%s Trying again (%d of 3).", problem.c_str(), retry);
		Sleep(100);
		found = FindDataRoot(context, io.dataRoot, problem);
	}
	if (!found) {
		snprintf(g_status, sizeof(g_status), "Exalted Items: inactive - %s The exalted tier is off this start: characters holding exalted items or materials may not load until it is back.",
		         problem.c_str());
		Alert(g_status);
		return true;
	}
	const Storage      storage      = FindStorage(context, io.dataRoot);
	const std::wstring pluginFolder = storage.folder;
	const std::wstring codesPath    = pluginFolder + L"\\codes.txt";
	io.genRoot                      = pluginFolder + L"\\data";
	LogF("mod data folder (read only): %s", Narrow(io.dataRoot).c_str());
	LogF("the plugin's own files (%s): %s", storage.global ? "installed for every mod" : "installed for this mod", Narrow(pluginFolder).c_str());
	LogF("the saves (read only, and only when codes.txt or stat-ids.txt must be recovered): %s", Narrow(SavesFolder(context)).c_str());
	if (IsDirectory(storage.other + L"\\data")) {
		RemoveStale(storage.other + L"\\data", storage.other + L"\\data", {});
	}

	exalted::Config config  = g_config;
	const double    started = Clock();
	// The hook that takes a restore off goes in first: the tables are built by whether it is there.
	PlaceStrip(context);
	config.stripHooked = g_stripPlaced;
	// codes.txt and stat-ids.txt: read, or put back from the plugin's old place or a previous copy.
	// Without them (and without a previous copy) the saves decide (RecoverAndRun).
	const std::wstring statIdsPath = pluginFolder + L"\\stat-ids.txt";
	const Ledger       ledger      = ReadLedger(pluginFolder, storage.other, config);
	std::string        refusal;
	const exalted::Summary summary = config.enabled && (!ledger.codesKnown || !ledger.statIdsKnown) ? RecoverAndRun(io, config, ledger, SavesFolder(context), pluginFolder, refusal)
	                                                                                               : exalted::Run(io, config);
	const double tablesMs = Clock() - started;
	for (const std::string& alert : io.alerts) {
		Alert(alert);
	}
	// Exalted cannot start its tier: the characters that hold exalted items are named, because they
	// will not load.
	const auto stop = [&](const char* kind, const std::string& why) {
		std::string named;
		if (refusal.empty()) {
			const std::vector<std::string> holders = exalted::FilesWithUnknownItems(ReadSaves(SavesFolder(context)), TablesFor(io, summary, true));
			named = holders.empty() ? " No save holds exalted items." : " Characters holding exalted items cannot be loaded until this is fixed: " + Characters(holders) + ".";
		}
		snprintf(g_status, sizeof(g_status), "Exalted Items: %s - %s%s", kind, why.c_str(), named.c_str());
		Alert(g_status);
		ArmStrip(summary);
	};
	if (!refusal.empty()) {
		stop("inactive", refusal);
		return true;
	}
	if (!summary.ok) {
		stop("failed", summary.error + " (the game runs without exalted items)");
		return true;
	}
	RemoveStale(io.genRoot, io.genRoot, summary.files);
	ArmStrip(summary);
	if (summary.builtIn) {
		// The mod's own files are read as they are (no redirect), and codes.txt is left for the
		// mod it was written for. Only the code hooks, fed from the mod's rows.
		InstallPickers(context, summary.families);
		InstallFilterBox(context, summary);
		g_legend.legend          = summary.legend;
		g_legend.crystalCode     = summary.crystalCode;
		g_legend.desecrationCode = summary.desecrationCode;
		g_legendConfig           = config;
		g_legendPath             = pluginFolder + L"\\legend.txt";
		snprintf(g_status, sizeof(g_status),
		         "Exalted Items: built into this mod's own tables (an exported mod), used as they are; %d exalted bases at item levels %d to %d; unique drops: %s; set drops: %s; loot filter: %s; "
		         "%d restore recipes, back to elite: %s; crystal %s, shard %s. No file adjusted or redirected; table settings do nothing for this mod.",
		         summary.bases, summary.levelLow, summary.levelHigh, g_pickState[exalted::PickUnique], g_pickState[exalted::PickSet], g_config.filterBox ? g_filterState : "off", summary.restores,
		         g_stripState, summary.crystalCode.empty() ? "none" : summary.crystalCode.c_str(), summary.desecrationCode.empty() ? "none" : summary.desecrationCode.c_str());
		LogF("%s", g_status);
		LogF("start-up took %.1f ms: tables %.1f, finding the pickers %.2f, hooking them %.1f", Clock() - started, tablesMs, g_pickerFindMs, g_pickerHookMs);
		return true;
	}
	if (!config.enabled || summary.files.empty()) {
		snprintf(g_status, sizeof(g_status), "Exalted Items: %s. The game reads the mod's files as they are.", config.enabled ? "nothing to add" : "disabled in exalted-items.toml");
		LogF("%s", g_status);
		return true;
	}
	MakeFolders(pluginFolder);
	SaveCodes(codesPath, config.savedCodes, summary.codes);
	if (summary.statIdsMade && !config.savedStatIds.Known()) {
		// Written once; from here on only the migration tool changes it. (One that could not be
		// read was kept aside when the saves had decided.)
		const exalted::StatIds& ids = summary.statIds;
		if (IsFile(statIdsPath)) {
			MoveFileExW(statIdsPath.c_str(), (statIdsPath + L".unreadable-" + Stamp()).c_str(), 0);
		}
		if (!IsFile(statIdsPath) && WriteNewFile(statIdsPath, exalted::FormatStatIds(ids))) {
			LogF("stat-ids.txt written: marker stat %d, roll stats %d to %d (%s)", ids.marker, ids.roll[0], ids.roll[exalted::kRollStatCount - 1],
			     ids.legacy ? "the layout of 1.2.0, kept for the saved items" : "after the mod's own stats");
		} else {
			LogF("stat-ids.txt could not be written; the numbers are worked out again at the next start");
		}
	}
	KeepPreviousCopy(pluginFolder, L"codes.txt");
	KeepPreviousCopy(pluginFolder, L"stat-ids.txt");
	const double redirectStarted = Clock();
	const int    slots           = InstallRedirects(io, summary.files);
	const double redirectMs      = Clock() - redirectStarted;
	if (slots == 0) {
		RemoveRedirects();
		g_redirectCount = 0;
		g_stripArmed.store(false, std::memory_order_release);
		stop("inactive", "the adjusted files were built, but the file redirect could not be set up, so the game is NOT using them.");
		return true;
	}
	InstallPickers(context, summary.families);
	InstallFilterBox(context, summary);
	g_legend.legend          = summary.legend;
	g_legend.crystalCode     = summary.crystalCode;
	g_legend.desecrationCode = summary.desecrationCode;
	g_legendConfig           = config;
	g_legendPath             = pluginFolder + L"\\legend.txt";
	char loot[160];
	if (summary.rungs > 0) {
		snprintf(loot, sizeof(loot), "%d loot rows added above the game's own, and %d high-level rows pointed at them", summary.rungs, summary.repointed);
	} else {
		snprintf(loot, sizeof(loot), "the game's own loot rows reach every level, none added");
	}
	char crystal[480];
	if (!summary.crystalCode.empty()) {
		snprintf(crystal, sizeof(crystal), "Restoration Crystal (item code %s, %s, %s) in %d boss loot rows, %d restore recipes; back to elite: %s", summary.crystalCode.c_str(),
		         summary.crystalTinted ? "purple picture" : "the shard's own picture", summary.crystalSlots > 0 ? "stacks in the stash" : "does not stack", summary.crystalRows,
		         summary.restores, g_stripState);
	} else {
		snprintf(crystal, sizeof(crystal), "no Restoration Crystal");
	}
	char shard[200];
	if (!summary.desecrationCode.empty()) {
		snprintf(shard, sizeof(shard), "Shard of Desecration (item code %s, %s, %s) in %d boss loot rows", summary.desecrationCode.c_str(), summary.desecrationTinted ? "red picture" : "the shard's own picture",
		         summary.desecrationSlots > 0 ? "stacks in the stash" : "does not stack", summary.desecrationRows);
	} else {
		snprintf(shard, sizeof(shard), "no Shard of Desecration");
	}
	snprintf(g_status, sizeof(g_status),
	         "Exalted Items: %d exalted bases at item levels %d to %d (%d names, %d models); they drop %s; unique drops: %s; set drops: %s; loot filter: %s; %d cube upgrade recipes, %d back to elite; %s; %s; %s. %d adjusted file(s), %d rebuilt this start; mod files untouched.",
	         summary.bases, summary.levelLow, summary.levelHigh, summary.names, summary.models, summary.terrorZoneOnly ? "only in terror zones" : "everywhere", g_pickState[exalted::PickUnique], g_pickState[exalted::PickSet], g_filterState, summary.recipes, summary.downgrades, crystal, shard, loot,
	         static_cast<int>(summary.files.size()), io.rewritten);
	LogF("%s (%d import slots redirected)", g_status, slots);
	// Where the time went, so a slow start can be told from the log.
	LogF("start-up took %.1f ms: tables %.1f (of that, reading files %.1f, comparing and storing copies %.1f), file redirect %.1f, finding the pickers %.2f, hooking them %.1f", Clock() - started,
	     tablesMs, io.readMs, io.writeMs, redirectMs, g_pickerFindMs, g_pickerHookMs);
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	RemoveRedirects();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = instance;
	}
	return TRUE;
}
