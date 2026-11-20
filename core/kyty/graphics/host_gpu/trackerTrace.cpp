#include "graphics/host_gpu/trackerTrace.h"

#include "common/timer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif
#endif

namespace Libs::Graphics::TrackerTrace {

#if defined(__linux__)

namespace {

constexpr uint64_t Chunk            = 64ull << 20;
constexpr uint64_t HeaderWords      = 8;
constexpr size_t   ThreadEventWords = size_t {1} << 20;
constexpr uint32_t FlagFault        = 1u;
constexpr uint32_t FlagOverflow     = 2u;

struct Writer {
	int                    fd    = -1;
	uint8_t*               base  = nullptr;
	uint64_t               limit = 0;
	std::atomic<uint64_t>  reserved {0};
	std::atomic<uint64_t>  mapped {0};
	std::atomic<uint32_t>  mapping {0};
	std::atomic<bool>      full {false};
	std::atomic<uint64_t>  seq {0};
	std::atomic<RoleQuery> roles {nullptr};
};

Writer g_writer;

struct ThreadState {
	uint32_t tid;
	bool     announced;
	bool     overflow;
	uint32_t fault_depth;
	Call*    current;
	uint64_t* events;
	size_t   used;
};

thread_local ThreadState t_state;

void Relax() noexcept {
#if defined(__x86_64__)
	_mm_pause();
#endif
}

std::string ExpandPath(const char* value) {
	std::string out;
	for (const char* at = value; *at != '\0';) {
		if (at[0] == '$' && at[1] == '{') {
			const char* close = std::strchr(at + 2, '}');
			if (close != nullptr) {
				const std::string name(at + 2, close);
				if (const char* expanded = std::getenv(name.c_str()); expanded != nullptr) {
					out += expanded;
				}
				at = close + 1;
				continue;
			}
		}
		out += *at++;
	}
	return out;
}

bool MapUpTo(uint64_t end) noexcept {
	for (;;) {
		auto have = g_writer.mapped.load(std::memory_order_acquire);
		if (have >= end) {
			return true;
		}
		uint32_t expected = 0;
		if (g_writer.mapping.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
			have = g_writer.mapped.load(std::memory_order_acquire);
			while (have < end) {
				const auto grown = have + Chunk;
				if (::ftruncate(g_writer.fd, static_cast<off_t>(grown)) != 0 ||
				    ::mmap(g_writer.base + have, Chunk, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_writer.fd,
				           static_cast<off_t>(have)) == MAP_FAILED) {
					g_writer.full.store(true, std::memory_order_release);
					g_writer.mapping.store(0, std::memory_order_release);
					return false;
				}
				have = grown;
				g_writer.mapped.store(have, std::memory_order_release);
			}
			g_writer.mapping.store(0, std::memory_order_release);
			return true;
		}
		if (g_writer.full.load(std::memory_order_acquire)) {
			return false;
		}
		Relax();
	}
}

uint64_t* Reserve(size_t words) noexcept {
	if (g_writer.full.load(std::memory_order_relaxed)) {
		return nullptr;
	}
	const auto bytes  = static_cast<uint64_t>(words) * 8;
	const auto offset = g_writer.reserved.fetch_add(bytes, std::memory_order_relaxed);
	if (offset + bytes > g_writer.limit) {
		g_writer.full.store(true, std::memory_order_release);
		return nullptr;
	}
	if (!MapUpTo(offset + bytes)) {
		return nullptr;
	}
	return reinterpret_cast<uint64_t*>(g_writer.base + offset);
}

void Publish(uint64_t* record, Op op, size_t args, size_t results, size_t total) noexcept {
	uint64_t hash = 0x6A09E667F3BCC908ull;
	for (size_t index = 1; index < total; index++) {
		hash = (hash ^ record[index]) * 0x100000001B3ull;
	}
	const uint64_t check  = (hash ^ (hash >> 32) ^ (hash >> 16)) & 0xFFFFu;
	const uint64_t header = RecordMagic | (static_cast<uint64_t>(op) << 8) | (static_cast<uint64_t>(args) << 16) |
	                        (static_cast<uint64_t>(results) << 20) | (static_cast<uint64_t>(total) << 24) |
	                        (check << 48);
	__atomic_store_n(record, header, __ATOMIC_RELEASE);
}

void Announce() noexcept {
	auto& state     = t_state;
	state.announced = true;
	state.tid       = static_cast<uint32_t>(::syscall(SYS_gettid));
	char name[17]   = {};
	(void)::prctl(PR_GET_NAME, name, 0, 0, 0);
	uint64_t words[2] = {};
	std::memcpy(words, name, 16);
	const auto     query = g_writer.roles.load(std::memory_order_acquire);
	const uint64_t roles = query != nullptr ? query() : 0;
	constexpr size_t total = RecordFixedWords + 4;
	auto* record = Reserve(total);
	if (record == nullptr) {
		return;
	}
	record[1] = state.tid;
	record[2] = 0;
	record[3] = 0;
	record[4] = Common::SteadyNs();
	record[5] = 0;
	record[6] = state.tid;
	record[7] = roles;
	record[8] = words[0];
	record[9] = words[1];
	Publish(record, Op::Thread, 4, 0, total);
}

uint64_t* Events() noexcept {
	auto& state = t_state;
	if (state.events == nullptr) {
		void* memory = ::mmap(nullptr, ThreadEventWords * 8, PROT_READ | PROT_WRITE,
		                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (memory == MAP_FAILED) {
			return nullptr;
		}
		state.events = static_cast<uint64_t*>(memory);
	}
	return state.events;
}

void WriteStandalone(Ev kind, const uint64_t* words, size_t count) noexcept {
	const size_t total  = RecordFixedWords + 1 + count;
	auto*        record = Reserve(total);
	if (record == nullptr) {
		return;
	}
	record[1] = t_state.tid | (static_cast<uint64_t>(t_state.fault_depth != 0 ? FlagFault : 0) << 32);
	record[2] = 0;
	record[3] = t_state.current != nullptr ? 1 : 0;
	record[4] = Common::SteadyNs();
	record[5] = 0;
	record[6] = static_cast<uint64_t>(kind) | (static_cast<uint64_t>(count) << 8);
	for (size_t index = 0; index < count; index++) {
		record[7 + index] = words[index];
	}
	Publish(record, Op::Standalone, 0, 0, total);
}

bool Init() {
	const char* value = std::getenv("KYTY_TRACKER_TRACE");
	if (value == nullptr || value[0] == '\0') {
		return false;
	}
	const auto path  = ExpandPath(value);
	uint64_t   limit = 8192;
	if (const char* mb = std::getenv("KYTY_TRACKER_TRACE_LIMIT_MB"); mb != nullptr && mb[0] != '\0') {
		limit = std::strtoull(mb, nullptr, 10);
	}
	limit = (limit << 20) / Chunk * Chunk;
	if (limit < Chunk) {
		limit = Chunk;
	}
	const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		std::fprintf(stderr, "[kyty-tracker-trace] cannot open %s: tracing stays off\n", path.c_str());
		return false;
	}
	void* base = ::mmap(nullptr, limit, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED) {
		::close(fd);
		std::fprintf(stderr, "[kyty-tracker-trace] cannot reserve %llu MiB: tracing stays off\n",
		             static_cast<unsigned long long>(limit >> 20));
		return false;
	}
	g_writer.fd    = fd;
	g_writer.base  = static_cast<uint8_t*>(base);
	g_writer.limit = limit;
	auto* header   = Reserve(HeaderWords);
	if (header == nullptr) {
		return false;
	}
	header[0] = FileMagic;
	header[1] = FileVersion;
	header[2] = 4096;
	header[3] = 4ull << 20;
	header[4] = static_cast<uint64_t>(::getpid());
	header[5] = Common::SteadyNs();
	header[6] = 0;
	header[7] = limit;
	std::atexit([] {
		__atomic_store_n(reinterpret_cast<uint64_t*>(g_writer.base) + 6, g_writer.reserved.load(std::memory_order_acquire),
		                 __ATOMIC_RELEASE);
	});
	std::fprintf(stderr, "[kyty-tracker-trace] recording every memory tracker call into %s (at most %llu MiB)\n",
	             path.c_str(), static_cast<unsigned long long>(limit >> 20));
	return true;
}

const bool g_initialized = [] {
	g_enabled = Init();
	return g_enabled;
}();

}

void SetRoleQuery(RoleQuery query) noexcept {
	g_writer.roles.store(query, std::memory_order_release);
}

uint64_t CurrentSeq() noexcept {
	return g_writer.seq.load(std::memory_order_acquire);
}

void Event(Ev kind, const uint64_t* words, size_t count) noexcept {
	auto& state = t_state;
	if (!state.announced) {
		Announce();
	}
	if (state.current == nullptr) {
		WriteStandalone(kind, words, count);
		return;
	}
	auto* events = Events();
	if (events == nullptr || state.used + 1 + count > ThreadEventWords) {
		state.overflow = true;
		return;
	}
	events[state.used] = static_cast<uint64_t>(kind) | (static_cast<uint64_t>(count) << 8);
	for (size_t index = 0; index < count; index++) {
		events[state.used + 1 + index] = words[index];
	}
	state.used += 1 + count;
}

void Call::Begin(Op op, const uint64_t* args, size_t count) noexcept {
	if (g_writer.full.load(std::memory_order_relaxed)) {
		return;
	}
	auto& state = t_state;
	if (!state.announced) {
		Announce();
	}
	m_op           = static_cast<uint8_t>(op);
	m_arg_count    = static_cast<uint8_t>(count);
	m_result_count = 0;
	m_flags        = state.fault_depth != 0 ? FlagFault : 0;
	for (auto& ordinal: m_ordinals) {
		ordinal = 0;
	}
	for (size_t index = 0; index < count; index++) {
		m_args[index] = args[index];
	}
	m_seq         = g_writer.seq.fetch_add(1, std::memory_order_acq_rel) + 1;
	m_parent      = state.current != nullptr ? state.current->m_seq : 0;
	m_previous    = state.current;
	m_event_start = state.used;
	m_started     = Common::SteadyNs();
	state.current = this;
	m_active      = true;
}

void Call::End() noexcept {
	auto&      state    = t_state;
	const auto duration = Common::SteadyNs() - m_started;
	const auto events   = state.used - m_event_start;
	const auto total    = RecordFixedWords + m_arg_count + m_result_count + events;
	if (state.overflow) {
		m_flags |= FlagOverflow;
	}
	if (auto* record = Reserve(total); record != nullptr) {
		record[1] = state.tid | (static_cast<uint64_t>(m_flags) << 32);
		record[2] = m_seq;
		record[3] = m_parent;
		record[4] = m_started;
		record[5] = duration;
		auto* out = record + RecordFixedWords;
		for (size_t index = 0; index < m_arg_count; index++) {
			*out++ = m_args[index];
		}
		for (size_t index = 0; index < m_result_count; index++) {
			*out++ = m_results[index];
		}
		if (events != 0) {
			std::memcpy(out, state.events + m_event_start, events * 8);
		}
		Publish(record, static_cast<Op>(m_op), m_arg_count, m_result_count, total);
	}
	state.used    = m_event_start;
	state.current = m_previous;
	if (state.current == nullptr) {
		state.overflow = false;
	}
	m_active = false;
}

void Call::CloseWindow(Callback kind, uint64_t begin) noexcept {
	const auto ordinal = m_ordinals[static_cast<size_t>(kind)]++;
	const auto end     = CurrentSeq();
	if (end != begin) {
		const uint64_t words[] = {static_cast<uint64_t>(kind) | (static_cast<uint64_t>(ordinal) << 8), begin, end};
		Event(Ev::Window, words, 3);
	}
}

void FaultScope::Enter() noexcept {
	t_state.fault_depth++;
}

void FaultScope::Leave() noexcept {
	t_state.fault_depth--;
}

#else

namespace {

const bool g_initialized = [] {
	if (const char* value = std::getenv("KYTY_TRACKER_TRACE"); value != nullptr && value[0] != '\0') {
		std::fprintf(stderr, "[kyty-tracker-trace] KYTY_TRACKER_TRACE is recorded on Linux only\n");
	}
	return false;
}();

}

void SetRoleQuery(RoleQuery) noexcept {}

uint64_t CurrentSeq() noexcept {
	return 0;
}

void Event(Ev, const uint64_t*, size_t) noexcept {}

void Call::Begin(Op, const uint64_t*, size_t) noexcept {}

void Call::End() noexcept {}

void Call::CloseWindow(Callback, uint64_t) noexcept {}

void FaultScope::Enter() noexcept {}

void FaultScope::Leave() noexcept {}

#endif

}
