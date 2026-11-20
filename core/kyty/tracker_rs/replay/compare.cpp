#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct File {
	const uint64_t* words = nullptr;
	size_t          count = 0;
};

File Map(const char* path) {
	const int fd = ::open(path, O_RDONLY);
	if (fd < 0) {
		std::fprintf(stderr, "cannot open %s\n", path);
		std::exit(2);
	}
	struct stat st {};
	::fstat(fd, &st);
	if (st.st_size == 0) return {};
	void* memory = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	return {static_cast<const uint64_t*>(memory), static_cast<size_t>(st.st_size) / 8};
}

struct Entry {
	uint64_t        seq   = 0;
	uint8_t         op    = 0;
	const uint64_t* words = nullptr;
	size_t          count = 0;
};

std::vector<Entry> Entries(const File& file) {
	std::vector<Entry> entries;
	for (size_t at = 0; at + 2 <= file.count;) {
		Entry entry {.seq = file.words[at], .op = static_cast<uint8_t>(file.words[at + 1]), .words = file.words + at + 2,
		             .count = static_cast<size_t>(file.words[at + 1] >> 8)};
		if (at + 2 + entry.count > file.count) break;
		entries.push_back(entry);
		at += 2 + entry.count;
	}
	return entries;
}

bool Kept(uint8_t kind, bool game) {
	switch (kind) {
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 11:
		case 12: return true;
		case 102: return true;
		case 100:
		case 101: return !game;
		default: return false;
	}
}

std::vector<uint64_t> Normalize(const uint64_t* words, size_t count, bool for_game) {
	std::vector<uint64_t> out;
	if (count == 0) return out;
	const size_t results = static_cast<size_t>(words[0]);
	out.assign(words, words + std::min(count, 1 + results));
	for (size_t at = 1 + results; at < count;) {
		const auto kind = static_cast<uint8_t>(words[at]);
		auto       size = static_cast<size_t>((words[at] >> 8) & 0xFFu);
		if (at + 1 + size > count) break;
		if (Kept(kind, for_game)) {
			size_t keep = size;
			if (kind == 2) keep = std::min<size_t>(keep, 1);
			if (kind == 6) keep = 0;
			out.push_back(static_cast<uint64_t>(kind) | (static_cast<uint64_t>(keep) << 8));
			out.insert(out.end(), words + at + 1, words + at + 1 + keep);
		}
		at += 1 + size;
	}
	return out;
}

std::vector<uint64_t> GameOutcome(const uint64_t* record) {
	const uint64_t header  = record[0];
	const size_t   args    = (header >> 16) & 0xFu;
	const size_t   results = (header >> 20) & 0xFu;
	const size_t   total   = (header >> 24) & 0xFFFFFFu;
	std::vector<uint64_t> raw;
	raw.push_back(results);
	raw.insert(raw.end(), record + 6 + args, record + 6 + args + results);
	raw.insert(raw.end(), record + 6 + args + results, record + total);
	return Normalize(raw.data(), raw.size(), true);
}

bool ValidRecord(const uint64_t* record, size_t available) {
	const uint64_t header = record[0];
	if ((header & 0xFFu) != 0xA7) return false;
	const size_t total = (header >> 24) & 0xFFFFFFu;
	if (total < 6 || total > available) return false;
	uint64_t hash = 0x6A09E667F3BCC908ull;
	for (size_t index = 1; index < total; index++) hash = (hash ^ record[index]) * 0x100000001B3ull;
	return ((hash ^ (hash >> 32) ^ (hash >> 16)) & 0xFFFFu) == (header >> 48);
}

std::string Describe(const std::vector<uint64_t>& words) {
	std::string text;
	char        buffer[64];
	for (size_t index = 0; index < words.size() && index < 24; index++) {
		std::snprintf(buffer, sizeof(buffer), "%s%" PRIx64, index == 0 ? "" : " ", words[index]);
		text += buffer;
	}
	if (words.size() > 24) text += " …";
	return text;
}

}

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s <outcome A> <outcome B> [--trace <trace>] [--show N]\n", argv[0]);
		return 2;
	}
	const char* trace_path = nullptr;
	size_t      show       = 12;
	for (int arg = 3; arg < argc; arg++) {
		if (std::strcmp(argv[arg], "--trace") == 0 && arg + 1 < argc) trace_path = argv[++arg];
		if (std::strcmp(argv[arg], "--show") == 0 && arg + 1 < argc) show = std::strtoull(argv[++arg], nullptr, 10);
	}
	const auto a = Entries(Map(argv[1]));
	const auto b = Entries(Map(argv[2]));
	uint64_t   compared = 0, differ = 0, digests = 0, digest_differ = 0;
	std::map<uint8_t, uint64_t> differ_by_op;
	const size_t n = std::min(a.size(), b.size());
	for (size_t index = 0; index < n; index++) {
		const auto& x = a[index];
		const auto& y = b[index];
		const auto  nx = Normalize(x.words, x.count, false);
		const auto  ny = Normalize(y.words, y.count, false);
		if (x.op == 255) digests++;
		compared++;
		if (x.seq != y.seq || x.op != y.op || nx != ny) {
			differ++;
			differ_by_op[x.op]++;
			if (x.op == 255) digest_differ++;
			if (differ <= show) {
				std::printf("A/B differ at entry %zu: seq %" PRIu64 "/%" PRIu64 " op %u/%u\n  A: %s\n  B: %s\n", index, x.seq, y.seq, x.op, y.op,
				            Describe(nx).c_str(), Describe(ny).c_str());
			}
		}
	}
	std::printf("A/B: %zu and %zu entries, %" PRIu64 " compared, %" PRIu64 " differ (%" PRIu64 " state digests, %" PRIu64 " differ)\n", a.size(),
	            b.size(), compared, differ, digests, digest_differ);
	for (const auto& [op, count]: differ_by_op) std::printf("  op %u: %" PRIu64 " differences\n", op, count);
	int code = (differ == 0 && a.size() == b.size()) ? 0 : 1;
	if (trace_path != nullptr) {
		const auto trace = Map(trace_path);
		std::unordered_map<uint64_t, size_t> by_seq;
		for (size_t at = 8; at < trace.count;) {
			if (!ValidRecord(trace.words + at, trace.count - at)) {
				at++;
				continue;
			}
			const uint64_t header = trace.words[at];
			const uint8_t  op     = static_cast<uint8_t>(header >> 8);
			if (op != 0 && op != 250) by_seq[trace.words[at + 2]] = at;
			at += (header >> 24) & 0xFFFFFFu;
		}
		uint64_t checked = 0, mismatched = 0, missing = 0;
		std::map<uint8_t, uint64_t> mismatch_by_op;
		for (const auto& entry: a) {
			if (entry.op == 255) continue;
			const auto it = by_seq.find(entry.seq);
			if (it == by_seq.end()) {
				missing++;
				continue;
			}
			checked++;
			const auto game   = GameOutcome(trace.words + it->second);
			const auto replay = Normalize(entry.words, entry.count, true);
			if (game != replay) {
				mismatched++;
				mismatch_by_op[entry.op]++;
				if (mismatched <= show) {
					std::printf("game/A differ at seq %" PRIu64 " op %u\n  game: %s\n  A:    %s\n", entry.seq, entry.op, Describe(game).c_str(),
					            Describe(replay).c_str());
				}
			}
		}
		std::printf("game/A: %" PRIu64 " calls checked, %" PRIu64 " differ from what the game recorded, %" PRIu64 " not in the trace\n", checked,
		            mismatched, missing);
		for (const auto& [op, count]: mismatch_by_op) std::printf("  op %u: %" PRIu64 " differences\n", op, count);
	}
	return code;
}
