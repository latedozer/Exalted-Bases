// Exalted Items: recognising the paths of the adjusted files.
//
// The file redirect looks at every path the game opens or asks about, and nearly all of them
// are not one of the handful of adjusted files. So the test is built to say "no" quickly: the
// last four characters of the path are packed into one number and compared with the endings
// of the adjusted files (".txt", "json"); only a path that ends like one of them is compared
// in full, from the end, where file names differ.
//
// Letter case (A-Z) and the kind of slash do not matter in the path. The endings are given
// already in lower case with backslashes.
//
// Portable C++ (no Windows calls), so it can be tested and timed outside the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace exalted {

template <typename Char>
constexpr Char FoldPathChar(Char c) {
	return c >= Char('A') && c <= Char('Z') ? static_cast<Char>(c - Char('A') + Char('a')) : c == Char('/') ? Char('\\') : c;
}

template <typename Char>
class SuffixSet {
public:
	static constexpr int kMax = 32;

	void Clear() {
		count_     = 0;
		keyCount_  = 0;
		minLength_ = static_cast<size_t>(-1);
		keyed_     = true;
	}

	// Adds a path ending (lower case, backslashes). Returns its number, or -1 when there is no room.
	int Add(const std::basic_string<Char>& suffix) {
		if (count_ >= kMax || suffix.empty()) {
			return -1;
		}
		storage_[count_] = suffix;
		Rule& rule       = rules_[count_];
		rule.text        = storage_[count_].data();
		rule.size        = storage_[count_].size();
		rule.key         = 0;
		if (rule.size >= 4) {
			rule.key = Key(rule.text + rule.size - 4, false);
			bool known = false;
			for (int i = 0; i < keyCount_; ++i) {
				known = known || keys_[i] == rule.key;
			}
			if (!known) {
				keys_[keyCount_++] = rule.key;
			}
		} else {
			keyed_ = false; // an ending this short cannot be told by its last four characters
		}
		minLength_ = rule.size < minLength_ ? rule.size : minLength_;
		return count_++;
	}

	int Count() const { return count_; }

	// The number of the ending this path has, or -1. The first one added wins.
	int Match(const Char* text, size_t length) const {
		if (length < minLength_) {
			return -1; // also when nothing has been added
		}
		uint64_t key = 0;
		if (keyed_) {
			key        = Key(text + length - 4, true);
			bool known = false;
			for (int i = 0; i < keyCount_; ++i) {
				known = known || keys_[i] == key;
			}
			if (!known) {
				return -1;
			}
		}
		for (int i = 0; i < count_; ++i) {
			const Rule& rule = rules_[i];
			if (length < rule.size || (keyed_ && rule.key != key)) {
				continue;
			}
			const Char* tail = text + (length - rule.size);
			size_t      left = rule.size;
			while (left > 0 && FoldPathChar(tail[left - 1]) == rule.text[left - 1]) {
				--left;
			}
			if (left == 0) {
				return i;
			}
		}
		return -1;
	}

private:
	struct Rule {
		const Char* text = nullptr;
		size_t      size = 0;
		uint64_t    key  = 0;
	};

	// Four characters as one number. Characters wider than 16 bits are cut short, which can only
	// make two different endings look alike here; the full comparison still decides.
	static uint64_t Key(const Char* four, bool fold) {
		uint64_t key = 0;
		for (int i = 0; i < 4; ++i) {
			const Char c = fold ? FoldPathChar(four[i]) : four[i];
			key          = (key << 16) | static_cast<uint16_t>(c);
		}
		return key;
	}

	Rule                    rules_[kMax];
	std::basic_string<Char> storage_[kMax];
	uint64_t                keys_[kMax] = {};
	int                     count_      = 0;
	int                     keyCount_   = 0;
	size_t                  minLength_  = static_cast<size_t>(-1);
	bool                    keyed_      = true;
};

}
