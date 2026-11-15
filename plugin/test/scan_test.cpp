// Native test: runs the picker search against a dumped game image (file offset == RVA).
//   scan_test <image dump> [expected set rva] [expected unique rva] [expected item row rva] [expected item level rva]
#include "../game_code.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: scan_test <image dump> [set rva] [unique rva] [item row rva] [item level rva]\n");
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	if (f == nullptr) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::vector<uint8_t> image;
	uint8_t              buffer[1 << 16];
	size_t               n;
	while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) image.insert(image.end(), buffer, buffer + n);
	fclose(f);

	const exalted::GameCode code = exalted::FindGameCode(image.data(), { { image.data(), image.data() + image.size() } });
	printf("%s\n", code.notes.c_str());
	printf("set=0x%llx (entry %u bytes) unique=0x%llx (entry %u bytes) itemRow=0x%llx itemLevel=0x%llx version=0x%x code=0x%x\n", static_cast<unsigned long long>(code.pickRva[exalted::PickSet]),
	       code.pickEntrySize[exalted::PickSet], static_cast<unsigned long long>(code.pickRva[exalted::PickUnique]), code.pickEntrySize[exalted::PickUnique],
	       static_cast<unsigned long long>(code.itemRowRva), static_cast<unsigned long long>(code.itemLevelRva), code.versionOffset, code.codeOffset);
	bool ok = code.pickRva[exalted::PickSet] != 0 && code.pickRva[exalted::PickUnique] != 0;
	const uint64_t got[] = { code.pickRva[exalted::PickSet], code.pickRva[exalted::PickUnique], code.itemRowRva, code.itemLevelRva };
	for (int i = 0; i < 4 && i + 2 < argc; ++i) {
		const uint64_t expected = strtoull(argv[i + 2], nullptr, 16);
		if (expected != got[i]) {
			printf("MISMATCH: value %d is 0x%llx, expected 0x%llx\n", i, static_cast<unsigned long long>(got[i]), static_cast<unsigned long long>(expected));
			ok = false;
		}
	}
	if (!code.searched) {
		printf("MISMATCH: a run without hints did not search the image\n");
		ok = false;
	}

	// Known addresses handed in: the same answer, without going through the image.
	auto same = [&code](const exalted::GameCode& other) {
		return other.pickRva[0] == code.pickRva[0] && other.pickRva[1] == code.pickRva[1] && other.pickEntrySize[0] == code.pickEntrySize[0] && other.pickEntrySize[1] == code.pickEntrySize[1]
		    && other.itemRowRva == code.itemRowRva && other.itemLevelRva == code.itemLevelRva && other.versionOffset == code.versionOffset && other.codeOffset == code.codeOffset;
	};
	const std::vector<exalted::CodeRange> ranges = { { image.data(), image.data() + image.size() } };
	struct Case {
		const char* what;
		uint64_t    hint[2]; // [PickUnique], [PickSet]
		bool        searched;
	};
	const Case cases[] = {
		{ "both addresses known", { code.pickRva[exalted::PickUnique], code.pickRva[exalted::PickSet] }, false },
		{ "only the unique picker known", { code.pickRva[exalted::PickUnique], 0 }, true },
		{ "only the set picker known", { 0, code.pickRva[exalted::PickSet] }, true },
		{ "addresses one byte off", { code.pickRva[exalted::PickUnique] + 1, code.pickRva[exalted::PickSet] + 1 }, true },
		{ "addresses swapped", { code.pickRva[exalted::PickSet], code.pickRva[exalted::PickUnique] }, true },
		{ "addresses outside the image", { image.size() + 0x1000, image.size() - 8 }, true },
	};
	for (const Case& c : cases) {
		const exalted::GameCode hinted = exalted::FindGameCode(image.data(), ranges, c.hint);
		const bool              good   = same(hinted) && hinted.searched == c.searched && hinted.notes == code.notes;
		printf("  hint, %-30s %s (%s)\n", c.what, good ? "ok" : "FAILED", hinted.searched ? "image searched" : "no search");
		ok = ok && good;
	}
	printf("%s\n", ok ? "scan test: ok" : "scan test: FAILED");
	return ok ? 0 : 1;
}
