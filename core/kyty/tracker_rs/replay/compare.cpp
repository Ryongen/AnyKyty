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
	uint64_t              seq = 0;
	uint8_t               op  = 0;
	std::vector<uint64_t> words;
};

bool Next(FILE* file, Entry& entry) {
	uint64_t header[2];
	if (std::fread(header, 8, 2, file) != 2) return false;
	entry.seq = header[0];
	entry.op  = static_cast<uint8_t>(header[1]);
	entry.words.resize(static_cast<size_t>(header[1] >> 8));
	return entry.words.empty() || std::fread(entry.words.data(), 8, entry.words.size(), file) == entry.words.size();
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

bool Mutating(uint8_t op) {
	switch (op) {
		case 7:
		case 9:
		case 10:
		case 11:
		case 34:
		case 37:
		case 38:
		case 39:
		case 40:
		case 42:
		case 43:
		case 44:
		case 48:
		case 52:
		case 53:
		case 54:
		case 55:
		case 56:
		case 59: return true;
		default: return false;
	}
}

bool Overlapped(const File& trace, const std::vector<uint64_t>& by_seq, uint64_t seq) {
	const uint64_t* self  = trace.words + by_seq[seq];
	const uint32_t  tid   = static_cast<uint32_t>(self[1]);
	const uint64_t  begin = self[4];
	const uint64_t  end   = self[4] + self[5];
	const uint64_t  from  = seq > 200000 ? seq - 200000 : 1;
	const uint64_t  to    = std::min<uint64_t>(by_seq.size(), seq + 200000);
	for (uint64_t other = from; other < to; other++) {
		if (other == seq || by_seq[other] == 0) continue;
		const uint64_t* record = trace.words + by_seq[other];
		if (static_cast<uint32_t>(record[1]) == tid) continue;
		if (!Mutating(static_cast<uint8_t>(record[0] >> 8))) continue;
		if (record[4] <= end && record[4] + record[5] >= begin) return true;
	}
	return false;
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
	File                  trace;
	std::vector<uint64_t> by_seq;
	if (trace_path != nullptr) {
		trace = Map(trace_path);
		for (size_t at = 8; at < trace.count;) {
			if (!ValidRecord(trace.words + at, trace.count - at)) {
				at++;
				continue;
			}
			const uint64_t header = trace.words[at];
			const uint8_t  op     = static_cast<uint8_t>(header >> 8);
			if (op != 0 && op != 250) {
				const auto seq = trace.words[at + 2];
				if (seq >= by_seq.size()) by_seq.resize(seq + (seq >> 2) + 1024, 0);
				by_seq[seq] = at;
			}
			at += (header >> 24) & 0xFFFFFFu;
		}
	}
	FILE* fa = std::fopen(argv[1], "rb");
	FILE* fb = std::fopen(argv[2], "rb");
	if (fa == nullptr || fb == nullptr) {
		std::fprintf(stderr, "cannot open the outcomes\n");
		return 2;
	}
	std::setvbuf(fa, nullptr, _IOFBF, 8 << 20);
	std::setvbuf(fb, nullptr, _IOFBF, 8 << 20);
	uint64_t compared = 0, differ = 0, digests = 0, digest_differ = 0, extra_a = 0, extra_b = 0;
	uint64_t checked = 0, mismatched = 0, missing = 0, overlapped = 0, unchecked = 0, alone_shown = 0;
	std::map<uint8_t, uint64_t> differ_by_op;
	std::map<uint8_t, uint64_t> mismatch_by_op;
	Entry x, y;
	for (;;) {
		const bool has_a = Next(fa, x);
		const bool has_b = Next(fb, y);
		if (!has_a || !has_b) {
			while (has_a && Next(fa, x)) extra_a++;
			while (has_b && Next(fb, y)) extra_b++;
			extra_a += has_a;
			extra_b += has_b;
			break;
		}
		compared++;
		if (x.op == 255) digests++;
		const auto nx = Normalize(x.words.data(), x.words.size(), false);
		const auto ny = Normalize(y.words.data(), y.words.size(), false);
		if (x.seq != y.seq || x.op != y.op || nx != ny) {
			differ++;
			differ_by_op[x.op]++;
			if (x.op == 255) digest_differ++;
			if (differ <= show) {
				std::printf("A/B differ at entry %" PRIu64 ": seq %" PRIu64 "/%" PRIu64 " op %u/%u\n  A: %s\n  B: %s\n", compared, x.seq, y.seq, x.op,
				            y.op, Describe(nx).c_str(), Describe(ny).c_str());
			}
		}
		if (trace_path != nullptr && x.op != 255) {
			if (x.seq >= by_seq.size() || by_seq[x.seq] == 0) {
				missing++;
			} else {
				checked++;
				const auto game   = GameOutcome(trace.words + by_seq[x.seq]);
				const auto replay = Normalize(x.words.data(), x.words.size(), true);
				if (game != replay) {
					mismatched++;
					mismatch_by_op[x.op]++;
					const bool checked_overlap = mismatched <= 5000;
					const bool concurrent      = checked_overlap && Overlapped(trace, by_seq, x.seq);
					overlapped += concurrent;
					unchecked += !checked_overlap;
					if ((checked_overlap && !concurrent && alone_shown++ < show) || mismatched <= show) {
						const uint64_t* record = trace.words + by_seq[x.seq];
						std::printf("%s seq %" PRIu64 " thread %" PRIu32 " start %" PRIu64 " ns duration %" PRIu64 " ns args %" PRIx64 " %" PRIx64 "\n",
						            concurrent ? "concurrent" : "ALONE", x.seq, static_cast<uint32_t>(record[1]), record[4], record[5],
						            ((record[0] >> 16) & 0xFu) > 0 ? record[6] : 0, ((record[0] >> 16) & 0xFu) > 1 ? record[7] : 0);
						std::printf("game/A differ at seq %" PRIu64 " op %u\n  game: %s\n  A:    %s\n", x.seq, x.op, Describe(game).c_str(),
						            Describe(replay).c_str());
					}
				}
			}
		}
	}
	std::printf("A/B: %" PRIu64 " entries compared, %" PRIu64 " differ (%" PRIu64 " state digests, %" PRIu64 " differ), %" PRIu64 " only in A, %" PRIu64
	            " only in B\n",
	            compared, differ, digests, digest_differ, extra_a, extra_b);
	for (const auto& [op, count]: differ_by_op) std::printf("  op %u: %" PRIu64 " differences\n", op, count);
	if (trace_path != nullptr) {
		std::printf("game/A: %" PRIu64 " calls checked, %" PRIu64 " differ from what the game recorded (%" PRIu64
		            " of the first 5000 overlapped in time a state-changing call of another thread, %" PRIu64 " beyond them not examined), %" PRIu64
		            " not in the trace\n",
		            checked, mismatched, overlapped, unchecked, missing);
		for (const auto& [op, count]: mismatch_by_op) std::printf("  op %u: %" PRIu64 " differences\n", op, count);
	}
	return (differ == 0 && extra_a == 0 && extra_b == 0) ? 0 : 1;
}
