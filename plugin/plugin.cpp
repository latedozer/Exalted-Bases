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
#include "redirect_match.h"

namespace {

const D2RL::PluginContext* g_ctx = nullptr;
char                       g_status[1400] = "Exalted Items: not run yet.";
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
	problem = "could not find the data folder of the mod '" + modName + "' (expected mods\\" + modName + "\\" + modName + ".mpq\\data).";
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
			} else if (key == "name_color") {
				const char code = ColourCode(textValue);
				if (code < 0) {
					LogF("config: name_color = \"%s\" is not understood (use %s, or none); orange is used", textValue.c_str(), kColourList);
				} else {
					config.crystalColor = code;
				}
			}
		} else if (section == "restore") {
			if (key == "enabled") {
				config.restoreEnabled = on;
			} else if (key == "material") {
				config.restoreMaterial = textValue;
			} else if (key == "show_line") {
				config.restoreShowLine = on;
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
	{ "recipes", "downgrade", "string", "Exalted to elite", "r01 is the El rune. The item stays what it is, on its elite base.", 0, 0, 0 },
	{ nullptr, nullptr, "header", "Restoration Crystal (in a recipe above, the word crystal stands for it)", nullptr, 0, 0, 0 },
	{ "crystal", "enabled", "bool", "Crystal", "Off: there is no such item, and recipes that name it are left out.", 0, 1, 1 },
	{ "crystal", "name", "string", "Name", nullptr, 0, 0, 0 },
	{ "crystal", "name_color", "string", "Name colour", "orange (as essences, keys and Worldstone Shards), white, red, green, blue, gold, grey, tan, yellow, darkgreen, purple.", 0, 0, 0 },
	{ "crystal", "drop_weight_percent", "int", "Boss drop weight %", "Hell act bosses drop it. 100 = as often as their essence, 0 = never.", 0, 1000, 10 },
	{ "crystal", "stash_slot", "bool", "Stacks in the stash", "A slot of its own in the stash's stacking tabs, beside the Worldstone Shards. Off: an ordinary item.", 0, 1, 1 },
	{ nullptr, nullptr, "header", "Restoring: exalted item + crystal adds these, once per item", nullptr, 0, 0, 0 },
	{ "restore", "enabled", "bool", "Restoring", nullptr, 0, 1, 1 },
	{ "restore", "material", "string", "Material", "Item codes; crystal stands for the Restoration Crystal.", 0, 0, 0 },
	{ "restore", "show_line", "bool", "Show \"Restored\" on the item", nullptr, 0, 1, 1 },
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
	if (section == "crystal") {
		return key == "enabled" ? (g_config.crystalEnabled ? "true" : "false") : key == "name" ? g_config.crystalName : key == "name_color" ? ColourWord(g_config.crystalColor) : key == "stash_slot" ? (g_config.crystalStashSlot ? "true" : "false") : std::to_string(g_config.crystalDropPercent);
	}
	if (section == "restore") {
		return key == "enabled" ? (g_config.restoreEnabled ? "true" : "false") : key == "material" ? g_config.restoreMaterial : (g_config.restoreShowLine ? "true" : "false");
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

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	const char* args = command != nullptr && command->args != nullptr ? command->args : "";
	while (*args == ' ') {
		++args;
	}
	g_ctx->WriteConsoleMessage(g_status);
	char                     line[700];
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

using CodeList = std::vector<std::pair<std::string, std::string>>;

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

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "exalted-items",
	.name        = "Exalted Items",
	.version     = "0.10.6",
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
	LogF("Exalted Items %s loading (D2R %s, SDK %s)", kPluginInfo.version, context->buildVersion != nullptr ? context->buildVersion : "?", D2RL_SDK_VERSION);
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
	if (!FindDataRoot(context, io.dataRoot, problem)) {
		LogF("%s", problem.c_str());
		snprintf(g_status, sizeof(g_status), "Exalted Items: inactive - %s", problem.c_str());
		return true;
	}
	const Storage      storage      = FindStorage(context, io.dataRoot);
	const std::wstring pluginFolder = storage.folder;
	const std::wstring codesPath    = pluginFolder + L"\\codes.txt";
	io.genRoot                      = pluginFolder + L"\\data";
	LogF("mod data folder (read only): %s", Narrow(io.dataRoot).c_str());
	LogF("the plugin's own files (%s): %s", storage.global ? "installed for every mod" : "installed for this mod", Narrow(pluginFolder).c_str());
	// The plugin was moved between a mod's loader folder and the game's: the codes come along
	// (saved items go by them), and the copies left at the old place are removed.
	if (!IsFile(codesPath) && IsFile(storage.other + L"\\codes.txt")) {
		MakeFolders(pluginFolder);
		if (CopyFileW((storage.other + L"\\codes.txt").c_str(), codesPath.c_str(), TRUE) != 0) {
			LogF("codes.txt taken over from %s", Narrow(storage.other).c_str());
		} else {
			LogF("codes.txt could not be copied from %s (error %lu); the game runs without exalted items rather than hand out new codes", Narrow(storage.other).c_str(), GetLastError());
			snprintf(g_status, sizeof(g_status), "Exalted Items: inactive - codes.txt could not be copied from %s.", Narrow(storage.other).c_str());
			return true;
		}
	}
	if (IsDirectory(storage.other + L"\\data")) {
		RemoveStale(storage.other + L"\\data", storage.other + L"\\data", {});
	}

	exalted::Config config  = g_config;
	const double    started = Clock();
	ReadCodes(codesPath, config.savedCodes);
	const exalted::Summary summary  = exalted::Run(io, config);
	const double           tablesMs = Clock() - started;
	if (!summary.ok) {
		LogF("failed: %s (the game runs without exalted items)", summary.error.c_str());
		snprintf(g_status, sizeof(g_status), "Exalted Items: failed - %s", summary.error.c_str());
		return true;
	}
	RemoveStale(io.genRoot, io.genRoot, summary.files);
	if (!config.enabled || summary.files.empty()) {
		snprintf(g_status, sizeof(g_status), "Exalted Items: %s. The game reads the mod's files as they are.", config.enabled ? "nothing to add" : "disabled in exalted-items.toml");
		LogF("%s", g_status);
		return true;
	}
	MakeFolders(pluginFolder);
	SaveCodes(codesPath, config.savedCodes, summary.codes);
	const double redirectStarted = Clock();
	const int    slots           = InstallRedirects(io, summary.files);
	const double redirectMs      = Clock() - redirectStarted;
	if (slots == 0) {
		RemoveRedirects();
		g_redirectCount = 0;
		snprintf(g_status, sizeof(g_status), "Exalted Items: the adjusted files were built, but the file redirect could not be set up, so the game is NOT using them.");
		LogF("%s", g_status);
		return true;
	}
	InstallPickers(context, summary.families);
	char loot[160];
	if (summary.rungs > 0) {
		snprintf(loot, sizeof(loot), "%d loot rows added above the game's own, and %d high-level rows pointed at them", summary.rungs, summary.repointed);
	} else {
		snprintf(loot, sizeof(loot), "the game's own loot rows reach every level, none added");
	}
	char crystal[240];
	if (!summary.crystalCode.empty()) {
		snprintf(crystal, sizeof(crystal), "Restoration Crystal (item code %s, %s, %s) in %d boss loot rows, %d restore recipes", summary.crystalCode.c_str(),
		         summary.crystalTinted ? "purple picture" : "the shard's own picture", summary.crystalSlots > 0 ? "stacks in the stash" : "does not stack", summary.crystalRows,
		         summary.restores);
	} else {
		snprintf(crystal, sizeof(crystal), "no Restoration Crystal");
	}
	snprintf(g_status, sizeof(g_status),
	         "Exalted Items: %d exalted bases at item levels %d to %d (%d names, %d models); they drop %s; unique drops: %s; set drops: %s; %d cube upgrade recipes, %d back to elite; %s; %s. %d adjusted file(s), %d rebuilt this start; mod files untouched.",
	         summary.bases, summary.levelLow, summary.levelHigh, summary.names, summary.models, summary.terrorZoneOnly ? "only in terror zones" : "everywhere", g_pickState[exalted::PickUnique], g_pickState[exalted::PickSet], summary.recipes, summary.downgrades, crystal, loot,
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
