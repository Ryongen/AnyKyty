#include "replay.h"

#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/trackerTrace.h"

#include <x86intrin.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Libs::Graphics;
namespace TT = Libs::Graphics::TrackerTrace;

namespace {

constexpr uint8_t EvUpload   = 100;
constexpr uint8_t EvDeferred = 101;
constexpr uint8_t EvFatal    = 102;
constexpr uint8_t EvDigest   = 103;

#if defined(ANYPS5_KYTY_RUST_TRACKER)
constexpr const char* Implementation = "rust";
#else
constexpr const char* Implementation = "cpp";
#endif

struct Trace {
	const uint64_t* words = nullptr;
	size_t          count = 0;
	uint64_t        start = 0;
};

struct View {
	const uint64_t* base = nullptr;
	uint8_t         op   = 0;
	uint8_t         args = 0;
	uint8_t         results = 0;
	size_t          total   = 0;

	[[nodiscard]] uint32_t Tid() const { return static_cast<uint32_t>(base[1]); }
	[[nodiscard]] uint32_t Flags() const { return static_cast<uint32_t>(base[1] >> 32); }
	[[nodiscard]] uint64_t Seq() const { return base[2]; }
	[[nodiscard]] uint64_t Parent() const { return base[3]; }
	[[nodiscard]] uint64_t Start() const { return base[4]; }
	[[nodiscard]] uint64_t Duration() const { return base[5]; }
	[[nodiscard]] uint64_t Arg(size_t index) const { return index < args ? base[TT::RecordFixedWords + index] : 0; }
	[[nodiscard]] const uint64_t* Results() const { return base + TT::RecordFixedWords + args; }
	[[nodiscard]] const uint64_t* Events() const { return base + TT::RecordFixedWords + args + results; }
	[[nodiscard]] size_t EventWords() const { return total - TT::RecordFixedWords - args - results; }

	template <typename Func>
	void ForEachEvent(Func&& func) const {
		const auto* events = Events();
		const auto  size   = EventWords();
		for (size_t at = 0; at < size;) {
			const auto kind  = static_cast<uint8_t>(events[at]);
			const auto count = static_cast<size_t>((events[at] >> 8) & 0xFFu);
			if (at + 1 + count > size) break;
			func(kind, events + at + 1, count);
			at += 1 + count;
		}
	}
};

bool Valid(const uint64_t* record, size_t available) {
	const uint64_t header = record[0];
	if ((header & 0xFFu) != TT::RecordMagic) return false;
	const size_t total = (header >> 24) & 0xFFFFFFu;
	if (total < TT::RecordFixedWords || total > available) return false;
	uint64_t hash = 0x6A09E667F3BCC908ull;
	for (size_t index = 1; index < total; index++) hash = (hash ^ record[index]) * 0x100000001B3ull;
	return ((hash ^ (hash >> 32) ^ (hash >> 16)) & 0xFFFFu) == (header >> 48);
}

View Decode(const uint64_t* record) {
	const uint64_t header = record[0];
	return {.base    = record,
	        .op      = static_cast<uint8_t>(header >> 8),
	        .args    = static_cast<uint8_t>((header >> 16) & 0xFu),
	        .results = static_cast<uint8_t>((header >> 20) & 0xFu),
	        .total   = static_cast<size_t>((header >> 24) & 0xFFFFFFu)};
}

struct Index {
	std::vector<std::pair<uint64_t, uint64_t>>                top;
	std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>     nested;
	uint64_t                                                  stop      = UINT64_MAX;
	uint64_t                                                  records   = 0;
	uint64_t                                                  skipped   = 0;
	uint64_t                                                  threads   = 0;
	uint64_t                                                  loose     = 0;
	uint64_t                                                  max_seq   = 0;
	uint64_t                                                  overflows = 0;
};

Trace MapTrace(const char* path) {
	const int fd = ::open(path, O_RDONLY);
	if (fd < 0) {
		std::fprintf(stderr, "cannot open %s\n", path);
		std::exit(2);
	}
	struct stat st {};
	::fstat(fd, &st);
	void* memory = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	if (memory == MAP_FAILED) {
		std::fprintf(stderr, "cannot map %s\n", path);
		std::exit(2);
	}
	::madvise(memory, static_cast<size_t>(st.st_size), MADV_SEQUENTIAL);
	Trace trace {.words = static_cast<const uint64_t*>(memory), .count = static_cast<size_t>(st.st_size) / 8};
	if (trace.count < 8 || trace.words[0] != TT::FileMagic) {
		std::fprintf(stderr, "%s is not a tracker trace\n", path);
		std::exit(2);
	}
	trace.start = trace.words[5];
	return trace;
}

Index Scan(const Trace& trace, bool skip_gaps) {
	Index index;
	std::vector<uint64_t> seqs;
	for (size_t at = 8; at < trace.count;) {
		if (!Valid(trace.words + at, trace.count - at)) {
			if (trace.words[at] != 0) index.skipped++;
			at++;
			continue;
		}
		const auto view = Decode(trace.words + at);
		index.records++;
		if (view.op == static_cast<uint8_t>(TT::Op::Thread)) {
			index.threads++;
		} else if (view.op == static_cast<uint8_t>(TT::Op::Standalone)) {
			index.loose++;
		} else {
			if ((view.Flags() & 2u) != 0) index.overflows++;
			seqs.push_back(view.Seq());
			if (view.Parent() == 0) {
				index.top.emplace_back(view.Seq(), at);
			} else {
				index.nested.emplace_back(view.Parent(), view.Seq(), at);
			}
		}
		at += view.total;
	}
	std::sort(index.top.begin(), index.top.end());
	std::sort(index.nested.begin(), index.nested.end());
	std::sort(seqs.begin(), seqs.end());
	index.max_seq = seqs.empty() ? 0 : seqs.back();
	if (!skip_gaps) {
		uint64_t expected = 1;
		for (const auto seq: seqs) {
			if (seq != expected) {
				index.stop = expected;
				break;
			}
			expected++;
		}
	}
	return index;
}

struct Frame {
	View                  view;
	std::vector<uint64_t> out;
	std::vector<uint64_t> times;
	size_t                time_at        = 0;
	bool                  protect_answer = true;
	uint32_t              ordinals[4]    = {};
};

class Driver {
public:
	Driver(const Trace& trace, const Index& index, FILE* out, bool bench, uint64_t digest_every, uint64_t limit)
	    : m_trace(trace), m_index(index), m_out(out), m_bench(bench), m_digest_every(digest_every), m_limit(limit) {
		s_driver = this;
	}

	static Driver* s_driver;

	uint64_t Now() {
		if (m_frames.empty()) return m_trace.start;
		auto& frame = *m_frames.back();
		if (frame.time_at < frame.times.size()) return frame.times[frame.time_at++];
		return frame.view.Start();
	}

	bool ProtectAnswer() { return m_frames.empty() ? true : m_frames.back()->protect_answer; }

	void OnEvent(uint8_t kind, const uint64_t* words, size_t count) {
		if (m_bench) return;
		switch (static_cast<TT::Ev>(kind)) {
			case TT::Ev::Time:
			case TT::Ev::ProtectResult:
			case TT::Ev::Window:
			case TT::Ev::DeferredTaken:
			case TT::Ev::ArgRange:
			case TT::Ev::Upload: return;
			case TT::Ev::BdaMark: count = 1; break;
			default: break;
		}
		Emit(kind, words, count);
	}

	void Emit(uint8_t kind, const uint64_t* words, size_t count) {
		if (m_frames.empty()) {
			m_loose++;
			return;
		}
		auto& out = m_frames.back()->out;
		out.push_back(static_cast<uint64_t>(kind) | (static_cast<uint64_t>(count) << 8));
		out.insert(out.end(), words, words + count);
	}

	[[noreturn]] void OnFatal(std::string_view text) {
		std::vector<uint64_t> words((text.size() + 7) / 8, 0);
		std::memcpy(words.data(), text.data(), text.size());
		const uint64_t seq = m_frames.empty() ? 0 : m_frames.back()->view.Seq();
		std::fprintf(stderr, "[replay %s] fatal at seq %" PRIu64 ": %.*s\n", Implementation, seq, static_cast<int>(text.size()), text.data());
		if (!m_frames.empty()) {
			Emit(EvFatal, words.data(), std::min<size_t>(words.size(), 255));
			for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it) Write((*it)->view, (*it)->out);
		}
		if (m_out != nullptr) std::fflush(m_out);
		std::_Exit(3);
	}

	int Run() {
		const auto started = std::chrono::steady_clock::now();
		uint64_t   done    = 0;
		for (const auto& [seq, offset]: m_index.top) {
			if (seq >= m_index.stop || done >= m_limit) break;
			const auto view = Decode(m_trace.words + offset);
			if (m_bench) {
				const auto     t0 = __rdtsc();
				Execute(view);
				const uint64_t t1 = __rdtsc();
				auto&          stat = m_stats[view.op | ((view.Flags() & 1u) << 8)];
				stat.count++;
				stat.cycles += t1 - t0;
				stat.game_ns += view.Duration();
			} else {
				Execute(view);
			}
			done++;
			if (m_digest_every != 0 && done % m_digest_every == 0) Digest(seq);
		}
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		if (!m_bench) Digest(UINT64_MAX);
		Finish();
		(void)m_tracker.release();
		(void)m_pages.release();
		std::fprintf(stderr,
		             "[replay %s] %" PRIu64 " top-level calls, %" PRIu64 " nested, %" PRIu64 " children outside a callback window, %" PRIu64
		             " events outside a call, in %.2f s\n",
		             Implementation, done, m_nested_done, m_orphans, m_loose, seconds);
		if (m_bench) Report(seconds);
		return 0;
	}

private:
	struct Stat {
		uint64_t count = 0, cycles = 0, game_ns = 0;
	};

	void Write(const View& view, const std::vector<uint64_t>& out) {
		if (m_out == nullptr) return;
		const uint64_t header[] = {view.Seq(), static_cast<uint64_t>(view.op) | (static_cast<uint64_t>(out.size()) << 8)};
		std::fwrite(header, sizeof(header), 1, m_out);
		if (!out.empty()) std::fwrite(out.data(), 8, out.size(), m_out);
	}

	void Digest(uint64_t seq) {
		if (m_tracker == nullptr || m_pages == nullptr) return;
		uint64_t hash  = 0xCBF29CE484222325ull;
		uint64_t words = 0;
		const auto emit = [&](const uint64_t* data, size_t count) {
			for (size_t index = 0; index < count; index++) hash = (hash ^ data[index]) * 0x100000001B3ull;
			words += count;
		};
#if defined(ANYPS5_KYTY_RUST_TRACKER)
		kyty_tr_debug_state(
		    m_tracker->RustHandle(), static_cast<const KytyTrPages*>(m_pages->RustHandle()),
		    [](void* context, const uint64_t* data, size_t count) { (*static_cast<decltype(emit)*>(context))(data, count); },
		    const_cast<void*>(static_cast<const void*>(&emit)));
#else
		m_tracker->DebugState(emit);
		m_pages->DebugState([](void* context, const uint64_t* data, size_t count) { (*static_cast<decltype(emit)*>(context))(data, count); },
		                    const_cast<void*>(static_cast<const void*>(&emit)));
#endif
		if (m_out == nullptr) return;
		const uint64_t entry[] = {seq, static_cast<uint64_t>(255) | (static_cast<uint64_t>(3) << 8), EvDigest | (2ull << 8), hash, words};
		std::fwrite(entry, sizeof(entry), 1, m_out);
	}

	static uint64_t RegionIndexOf(RegionManager* region) {
#if defined(ANYPS5_KYTY_RUST_TRACKER)
		return kyty_tr_region_index(region);
#else
		return region->CpuAddress() / TRACKER_REGION_SIZE;
#endif
	}

	void RunWindow(Frame& frame, TT::Callback kind) {
		const auto ordinal = frame.ordinals[static_cast<size_t>(kind)]++;
		uint64_t   begin = 0, end = 0;
		bool       found = false;
		frame.view.ForEachEvent([&](uint8_t event, const uint64_t* words, size_t count) {
			if (found || event != static_cast<uint8_t>(TT::Ev::Window) || count < 3) return;
			if ((words[0] & 0xFFu) == static_cast<uint64_t>(kind) && (words[0] >> 8) == ordinal) {
				begin = words[1];
				end   = words[2];
				found = true;
			}
		});
		if (!found) return;
		const auto parent = frame.view.Seq();
		auto       it     = std::lower_bound(m_index.nested.begin(), m_index.nested.end(), std::make_tuple(parent, begin + 1, uint64_t {0}));
		for (; it != m_index.nested.end() && std::get<0>(*it) == parent && std::get<1>(*it) <= end; ++it) {
			if (std::get<1>(*it) >= m_index.stop) break;
			m_ran_children.push_back(std::get<1>(*it));
			m_nested_done++;
			Execute(Decode(m_trace.words + std::get<2>(*it)));
		}
	}

	void RunOrphans(const View& view) {
		const auto parent = view.Seq();
		auto       it     = std::lower_bound(m_index.nested.begin(), m_index.nested.end(), std::make_tuple(parent, uint64_t {0}, uint64_t {0}));
		for (; it != m_index.nested.end() && std::get<0>(*it) == parent; ++it) {
			if (std::find(m_ran_children.begin(), m_ran_children.end(), std::get<1>(*it)) != m_ran_children.end()) continue;
			if (std::get<1>(*it) >= m_index.stop) break;
			m_orphans++;
			m_nested_done++;
			Execute(Decode(m_trace.words + std::get<2>(*it)));
		}
	}

	void Range(Frame& frame, uint64_t address, uint64_t size) {
		const uint64_t words[] = {address, size};
		if (!m_bench) Emit(static_cast<uint8_t>(TT::Ev::Range), words, 2);
		RunWindow(frame, TT::Callback::Range);
	}

	void Upload(Frame& frame) {
		if (!m_bench) Emit(EvUpload, nullptr, 0);
		auto& deferred = MemoryTracker::DeferredProtects();
		for (const auto& item: deferred) m_regions[RegionIndexOf(item.manager)] = item.manager;
		RunWindow(frame, TT::Callback::Upload);
		uint64_t before = 0, after = 0;
		bool     taken  = false;
		frame.view.ForEachEvent([&](uint8_t event, const uint64_t* words, size_t count) {
			if (event == static_cast<uint8_t>(TT::Ev::DeferredTaken) && count >= 2) {
				before = words[0];
				after  = words[1];
				taken  = true;
			}
		});
		if (taken && after < before) {
			if (!m_bench) {
				for (const auto& item: deferred) {
					const uint64_t words[] = {RegionIndexOf(item.manager), item.address, item.size};
					Emit(EvDeferred, words, 3);
				}
			}
			if (m_apply_taken) m_taken.insert(m_taken.end(), deferred.begin(), deferred.end());
			deferred.clear();
		}
	}

	void Execute(const View& view) {
		auto frame  = std::make_unique<Frame>();
		frame->view = view;
		view.ForEachEvent([&](uint8_t kind, const uint64_t* words, size_t count) {
			if (kind == static_cast<uint8_t>(TT::Ev::Time) && count >= 1) frame->times.push_back(words[0]);
			if (kind == static_cast<uint8_t>(TT::Ev::ProtectResult) && count >= 1) frame->protect_answer = words[0] != 0;
		});
		const auto saved = m_ran_children.size();
		m_frames.push_back(frame.get());
		auto& f = *frame;
		if (m_frames.size() == 1 && !m_taken.empty()) {
			const auto taken = std::move(m_taken);
			m_taken.clear();
			for (const auto& item: taken) RegionManager::ApplyDeferred(item.manager, item.address, item.size);
		}
		Dispatch(f);
		RunOrphans(view);
		m_frames.pop_back();
		m_ran_children.resize(saved);
		if (!m_bench) Write(view, f.out);
	}

	void Dispatch(Frame& f) {
		const auto& v = f.view;
		using Op      = TT::Op;
		std::vector<uint64_t> results;
		switch (static_cast<Op>(v.op)) {
			case Op::PageManagerCreate: m_pages = std::make_unique<PageManager>(); break;
			case Op::PageManagerDestroy: m_pages.reset(); break;
			case Op::HasReadWatchers: results = {m_pages->HasReadWatchers(v.Arg(0), v.Arg(1))}; break;
			case Op::IsWatched: results = {m_pages->IsWatched(v.Arg(0))}; break;
			case Op::WatchedSpan: {
				uint64_t   first = 0, end = 0;
				const bool any   = m_pages->WatchedSpan(v.Arg(0), v.Arg(1), &first, &end);
				results          = {any, any ? first : 0, any ? end : 0};
				break;
			}
			case Op::AllowExecute: results = {m_pages->AllowExecute(v.Arg(0))}; break;
			case Op::UpdatePageWatchers: {
				const bool track = v.Arg(2) != 0, read = v.Arg(3) != 0;
				if (track && read) m_pages->UpdatePageWatchers<true, true>(v.Arg(0), v.Arg(1));
				if (track && !read) m_pages->UpdatePageWatchers<true, false>(v.Arg(0), v.Arg(1));
				if (!track && read) m_pages->UpdatePageWatchers<false, true>(v.Arg(0), v.Arg(1));
				if (!track && !read) m_pages->UpdatePageWatchers<false, false>(v.Arg(0), v.Arg(1));
				break;
			}
			case Op::HasHotPages: results = {m_pages->HasHotPages()}; break;
			case Op::NoteWriteFault: m_pages->NoteWriteFault(v.Arg(0)); break;
			case Op::CollectHotPageWrites:
				m_pages->CollectHotPageWrites(
				    [](void* context, uint64_t begin, uint64_t end) {
					    auto& self = *static_cast<Driver*>(context);
					    self.Range(*self.m_frames.back(), begin, end - begin);
				    },
				    this, v.Arg(0), v.Arg(1));
				break;
			case Op::ForgetHotPages: m_pages->ForgetHotPages(v.Arg(0), v.Arg(1)); break;
			case Op::TrackerCreate: m_tracker = std::make_unique<MemoryTracker>(*m_pages); break;
			case Op::TrackerDestroy: m_tracker.reset(); break;
			case Op::IsRegionCpuModified: results = {m_tracker->IsRegionCpuModified(v.Arg(0), v.Arg(1))}; break;
			case Op::IsRegionGpuModified: results = {m_tracker->IsRegionGpuModified(v.Arg(0), v.Arg(1))}; break;
			case Op::MayBeGpuModified: results = {m_tracker->MayBeGpuModified(v.Arg(0), v.Arg(1))}; break;
			case Op::MarkRegionAsCpuModified: m_tracker->MarkRegionAsCpuModified(v.Arg(0), v.Arg(1)); break;
			case Op::UnmarkRegionAsGpuModified: m_tracker->UnmarkRegionAsGpuModified(v.Arg(0), v.Arg(1)); break;
			case Op::InvalidateRegion:
				m_tracker->InvalidateRegion(v.Arg(0), v.Arg(1), [&] {
					if (!m_bench) Emit(static_cast<uint8_t>(TT::Ev::Flush), nullptr, 0);
					RunWindow(f, TT::Callback::Flush);
				});
				break;
			case Op::ForEachDownloadRange: {
				const auto func = [&](uint64_t address, uint64_t bytes) noexcept { Range(f, address, bytes); };
				if (v.Arg(2) != 0) {
					m_tracker->ForEachDownloadRange<true>(v.Arg(0), v.Arg(1), func);
				} else {
					m_tracker->ForEachDownloadRange<false>(v.Arg(0), v.Arg(1), func);
				}
				break;
			}
			case Op::RegionKeptSync: results = {m_tracker->RegionKeptSync(v.Arg(0))}; break;
			case Op::NoteGuestWriteFault: m_tracker->NoteGuestWriteFault(v.Arg(0)); break;
			case Op::OpenWriteBlock: {
				const auto probe = m_tracker->OpenWriteBlock(v.Arg(0), v.Arg(1), v.Arg(2) != 0);
				results          = {probe.opened, probe.gpu, probe.tracked};
				break;
			}
			case Op::TakeKeptPages: results = {m_tracker->TakeKeptPages()}; break;
			case Op::RegionCpuDirtyEpoch: results = {m_tracker->RegionCpuDirtyEpoch(v.Arg(0))}; break;
			case Op::CpuModificationEpoch: results = {m_tracker->CpuModificationEpoch(v.Arg(0), v.Arg(1))}; break;
			case Op::DeferredProtectionPending: results = {m_tracker->DeferredProtectionPending(v.Arg(0))}; break;
			case Op::NoteHostWrite: m_tracker->NoteHostWrite(v.Arg(0), v.Arg(1)); break;
			case Op::RegionGpuCleanEpoch: results = {m_tracker->RegionGpuCleanEpoch(v.Arg(0))}; break;
			case Op::ForEachCpuDirtySpan:
				m_tracker->ForEachCpuDirtySpan(v.Arg(0), v.Arg(1), v.Arg(2), [&](uint64_t address, uint64_t bytes) { Range(f, address, bytes); });
				break;
			case Op::CopyRegionCpuDirty: {
				RegionBits pages;
				const bool any = m_tracker->CopyRegionCpuDirty(v.Arg(0), pages);
				results        = {any};
				if (any && !m_bench) {
					uint64_t words[TRACKER_REGION_PAGES / 64];
					for (size_t word = 0; word < TRACKER_REGION_PAGES / 64; word++) words[word] = pages.Word(word);
					Emit(static_cast<uint8_t>(TT::Ev::Bits), words, TRACKER_REGION_PAGES / 64);
				}
				break;
			}
			case Op::ConsumeCpuWrites:
				m_tracker->ConsumeCpuWrites(static_cast<CpuWriteConsumer>(v.Arg(0)), v.Arg(1), v.Arg(2),
				                            [&](uint64_t address, uint64_t bytes) { Range(f, address, bytes); });
				break;
			case Op::MarkRegionAsGpuModifiedUnlessCpuDirty:
				m_tracker->MarkRegionAsGpuModifiedUnlessCpuDirty(v.Arg(0), v.Arg(1), [&](uint64_t address, uint64_t bytes) { Range(f, address, bytes); });
				break;
			case Op::ForEachUploadRange: {
				std::unique_ptr<MemoryTracker::UploadScope> scope;
				if (v.Arg(4) != 0) scope = std::make_unique<MemoryTracker::UploadScope>();
				m_tracker->ForEachUploadRange(
				    v.Arg(0), v.Arg(1), v.Arg(2) != 0, [&](uint64_t address, uint64_t bytes) noexcept { Range(f, address, bytes); },
				    [&]() noexcept { Upload(f); }, static_cast<uint32_t>(v.Arg(3)));
				break;
			}
			case Op::ForEachUploadRanges: {
				std::vector<std::pair<uint64_t, uint64_t>> ranges;
				v.ForEachEvent([&](uint8_t kind, const uint64_t* words, size_t count) {
					if (kind == static_cast<uint8_t>(TT::Ev::ArgRange) && count >= 2) ranges.emplace_back(words[0], words[1]);
				});
				std::unique_ptr<MemoryTracker::UploadScope> scope;
				if (v.Arg(2) != 0) scope = std::make_unique<MemoryTracker::UploadScope>();
				m_tracker->ForEachUploadRanges(
				    ranges,
				    [&](size_t index, uint64_t address, uint64_t bytes) noexcept {
					    const uint64_t words[] = {index, address, bytes};
					    if (!m_bench) Emit(static_cast<uint8_t>(TT::Ev::IndexedRange), words, 3);
					    RunWindow(f, TT::Callback::Range);
				    },
				    [&]() noexcept { Upload(f); }, static_cast<uint32_t>(v.Arg(1)));
				break;
			}
			case Op::ApplyDeferredProtection: {
				const auto it = m_regions.find(v.Arg(0));
				if (it == m_regions.end()) {
					m_unknown_regions++;
					break;
				}
				RegionManager::ApplyDeferred(it->second, v.Arg(1), v.Arg(2));
				break;
			}
			case Op::CpuDirtyEpoch: results = {RegionManager::CpuDirtyEpoch()}; break;
			case Op::GpuCleanEpoch: results = {RegionManager::GpuCleanEpoch()}; break;
			case Op::TakeOpenedUploadBytes: results = {RegionManager::TakeOpenedUploadBytes()}; break;
			default: m_unknown_ops++; break;
		}
		if (!m_bench) {
			std::vector<uint64_t> merged;
			merged.reserve(1 + results.size() + f.out.size());
			merged.push_back(results.size());
			merged.insert(merged.end(), results.begin(), results.end());
			merged.insert(merged.end(), f.out.begin(), f.out.end());
			f.out.swap(merged);
		}
	}

	void Finish() {
		if (m_out != nullptr) std::fflush(m_out);
		if (m_unknown_regions != 0 || m_unknown_ops != 0) {
			std::fprintf(stderr, "[replay %s] %" PRIu64 " deferred applications to a region never deferred, %" PRIu64 " unknown calls\n",
			             Implementation, m_unknown_regions, m_unknown_ops);
		}
	}

	void Report(double seconds) {
		const auto t0 = std::chrono::steady_clock::now();
		const auto c0 = __rdtsc();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const auto c1 = __rdtsc();
		const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
		const auto ghz = static_cast<double>(c1 - c0) / ns;
		std::printf("{\"implementation\": \"%s\", \"seconds\": %.3f, \"tsc_ghz\": %.4f, \"ops\": [", Implementation, seconds, ghz);
		bool first = true;
		for (const auto& [key, stat]: m_stats) {
			std::printf("%s{\"op\": %u, \"fault\": %u, \"count\": %" PRIu64 ", \"ns\": %.1f, \"game_ns\": %.1f}", first ? "" : ", ", key & 0xFFu,
			            key >> 8, stat.count, static_cast<double>(stat.cycles) / ghz / static_cast<double>(stat.count),
			            static_cast<double>(stat.game_ns) / static_cast<double>(stat.count));
			first = false;
		}
		std::printf("]}\n");
	}

	const Trace&                              m_trace;
	const Index&                              m_index;
	FILE*                                     m_out;
	bool                                      m_bench;
	uint64_t                                  m_digest_every;
	uint64_t                                  m_limit;
	std::vector<Frame*>                       m_frames;
	std::vector<uint64_t>                     m_ran_children;
	std::unique_ptr<PageManager>              m_pages;
	std::unique_ptr<MemoryTracker>            m_tracker;
	std::unordered_map<uint64_t, RegionManager*> m_regions;
	std::vector<MemoryTracker::DeferredProtect>  m_taken;

public:
	bool m_apply_taken = false;

private:
	std::map<uint32_t, Stat>                  m_stats;
	uint64_t                                  m_nested_done     = 0;
	uint64_t                                  m_orphans         = 0;
	uint64_t                                  m_loose           = 0;
	uint64_t                                  m_unknown_regions = 0;
	uint64_t                                  m_unknown_ops     = 0;
};

Driver* Driver::s_driver = nullptr;

std::atomic<uint64_t> g_hot_counter {0};
bool                  g_hot = false;

uint64_t Mix(uint64_t value) {
	value ^= value >> 33;
	value *= 0xFF51AFD7ED558CCDull;
	value ^= value >> 33;
	value *= 0xC4CEB9FE1A85EC53ull;
	return value ^ (value >> 33);
}

}

namespace Replay {

uint64_t Now() {
	return Driver::s_driver != nullptr ? Driver::s_driver->Now() : 0;
}

bool ProtectAnswer() {
	return Driver::s_driver != nullptr ? Driver::s_driver->ProtectAnswer() : true;
}

void OnEvent(uint8_t kind, const uint64_t* words, size_t count) {
	if (Driver::s_driver != nullptr) Driver::s_driver->OnEvent(kind, words, count);
}

void OnFatal(std::string_view text) {
	if (Driver::s_driver != nullptr) Driver::s_driver->OnFatal(text);
	std::_Exit(3);
}

bool HotAvailable() {
	return g_hot;
}

bool HotWatch(uint64_t begin, uint64_t) {
	return Mix(begin ^ g_hot_counter.fetch_add(1)) % 7 != 0;
}

void HotUnwatch(uint64_t, uint64_t) {}

bool HotCollect(uint64_t begin, uint64_t end, void (*written)(void*, uint64_t, uint64_t), void* context, bool strict) {
	const auto salt = g_hot_counter.fetch_add(1);
	if (Mix(begin ^ (salt << 1)) % 11 == 0) return false;
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		if (Mix(page ^ salt) % (strict ? 3 : 4) == 0) written(context, page, page + TRACKER_PAGE_SIZE);
	}
	return true;
}

}

void History(const Trace& trace, const Index& index, uint64_t page, uint64_t until) {
	std::vector<std::pair<uint64_t, uint64_t>> all(index.top.begin(), index.top.end());
	for (const auto& entry: index.nested) all.emplace_back(std::get<1>(entry), std::get<2>(entry));
	std::sort(all.begin(), all.end());
	const auto covers = [&](uint64_t address, uint64_t size) { return size != 0 && address <= page + 4095 && page < address + size; };
	for (const auto& [seq, offset]: all) {
		if (seq > until) break;
		const auto view = Decode(trace.words + offset);
		bool       hit  = false;
		switch (view.op) {
			case 3: case 5: case 7: case 10: case 11: case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 46: case 48: case 50:
			case 53: case 54: hit = covers(view.Arg(0), view.Arg(1)); break;
			case 4: case 6: case 9: case 42: case 43: case 47: hit = covers(view.Arg(0), 1); break;
			case 52: case 56: hit = covers(view.Arg(1), view.Arg(2)); break;
			default: break;
		}
		std::string effects;
		view.ForEachEvent([&](uint8_t kind, const uint64_t* words, size_t count) {
			if ((kind == 1 || kind == 3 || kind == 4 || kind == 9) && count >= 2 && covers(words[0], words[1])) {
				char text[96];
				std::snprintf(text, sizeof(text), " ev%u(%" PRIx64 ",%" PRIx64 "%s)", kind, words[0], words[1], kind == 1 ? (words[2] == 1 ? ",R" : words[2] == 3 ? ",RW" : ",NA") : "");
				effects += text;
				hit = true;
			}
		});
		if (!hit) continue;
		std::printf("seq %" PRIu64 " parent %" PRIu64 " op %u tid %u fault %u t %.6f dur %" PRIu64 " args %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " res",
		            seq, view.Parent(), view.op, view.Tid(), view.Flags() & 1u, static_cast<double>(view.Start()) / 1e9, view.Duration(), view.Arg(0), view.Arg(1),
		            view.Arg(2), view.Arg(3), view.Arg(4));
		for (size_t index = 0; index < view.results; index++) std::printf(" %" PRIx64, view.Results()[index]);
		std::printf("%s\n", effects.c_str());
	}
}

void Profile(const Trace& trace, const Index& index) {
	std::unordered_map<uint32_t, std::string> names;
	std::unordered_map<uint32_t, uint64_t>    roles;
	for (size_t at = 8; at < trace.count;) {
		if (!Valid(trace.words + at, trace.count - at)) {
			at++;
			continue;
		}
		const auto view = Decode(trace.words + at);
		if (view.op == static_cast<uint8_t>(TT::Op::Thread)) {
			char name[17] = {};
			std::memcpy(name, view.base + TT::RecordFixedWords + 2, 16);
			std::string text(name);
			for (auto& c: text)
				if (c >= '0' && c <= '9') c = '#';
			names[static_cast<uint32_t>(view.Arg(0))] = text;
			roles[static_cast<uint32_t>(view.Arg(0))] = view.Arg(1);
		}
		at += view.total;
	}
	struct Cell {
		uint64_t count = 0, faults = 0, nested = 0, ns = 0, protects = 0, bda = 0;
	};
	std::map<std::pair<uint8_t, std::string>, Cell> cells;
	uint64_t first = UINT64_MAX, last = 0;
	const auto add = [&](uint64_t offset, bool nested) {
		const auto view = Decode(trace.words + offset);
		const auto tid  = view.Tid();
		auto       name = names.contains(tid) ? names[tid] : std::string("?");
		const auto role = roles.contains(tid) ? roles[tid] : 0;
		if (role & 1) name += "[gpu]";
		if (role & 2) name += "[cp]";
		if (role & 4) name += "[main]";
		auto& cell = cells[{view.op, name}];
		cell.count++;
		cell.faults += (view.Flags() & 1u) != 0;
		cell.nested += nested;
		cell.ns += view.Duration();
		view.ForEachEvent([&](uint8_t kind, const uint64_t*, size_t) {
			cell.protects += kind == static_cast<uint8_t>(TT::Ev::Protect);
			cell.bda += kind == static_cast<uint8_t>(TT::Ev::BdaMark);
		});
		first = std::min(first, view.Start());
		last  = std::max(last, view.Start() + view.Duration());
	};
	for (const auto& [seq, offset]: index.top) add(offset, false);
	for (const auto& entry: index.nested) add(std::get<2>(entry), true);
	std::printf("{\"seconds\": %.1f, \"calls\": [", static_cast<double>(last - first) / 1e9);
	bool comma = false;
	for (const auto& [key, cell]: cells) {
		std::printf("%s\n{\"op\": %u, \"thread\": \"%s\", \"count\": %" PRIu64 ", \"faults\": %" PRIu64 ", \"nested\": %" PRIu64
		            ", \"ns\": %" PRIu64 ", \"protects\": %" PRIu64 ", \"bda\": %" PRIu64 "}",
		            comma ? "," : "", key.first, key.second.c_str(), cell.count, cell.faults, cell.nested, cell.ns, cell.protects, cell.bda);
		comma = true;
	}
	std::printf("]}\n");
}

int Contend(unsigned threads, uint64_t rounds) {
	::setenv("KYTY_ASYNC_REPROTECT", "1", 1);
	TT::g_enabled = false;
	auto pages    = std::make_unique<PageManager>();
	auto tracker  = std::make_unique<MemoryTracker>(*pages);
	constexpr uint64_t base    = 0x2'0000'0000ull;
	constexpr uint64_t regions = 4;
	for (uint64_t region = 0; region < regions; region++) {
		const auto address = base + region * TRACKER_REGION_SIZE;
		tracker->ForEachUploadRange(address, TRACKER_REGION_SIZE, false, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
	}
	std::atomic<unsigned> ready {0};
	std::atomic<bool>     go {false};
	std::vector<uint64_t> cycles(threads, 0);
	std::vector<std::thread> workers;
	for (unsigned index = 0; index < threads; index++) {
		workers.emplace_back([&, index] {
			ready.fetch_add(1);
			while (!go.load()) std::this_thread::yield();
			const auto     t0      = __rdtsc();
			const uint64_t per     = TRACKER_REGION_PAGES * regions / threads;
			for (uint64_t round = 0; round < rounds; round++) {
				const uint64_t slot = index + threads * (round % per);
				const uint64_t page = base + slot * TRACKER_PAGE_SIZE;
				(void)tracker->OpenWriteBlock(page, 64 * 1024, false);
				(void)tracker->IsRegionCpuModified(page, TRACKER_PAGE_SIZE);
				tracker->InvalidateRegion(page, TRACKER_PAGE_SIZE, [] {});
				(void)tracker->MayBeGpuModified(page, TRACKER_PAGE_SIZE);
				tracker->ForEachUploadRange(page, TRACKER_PAGE_SIZE, false, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
			}
			cycles[index] = __rdtsc() - t0;
		});
	}
	while (ready.load() != threads) std::this_thread::yield();
	const auto started = std::chrono::steady_clock::now();
	const auto c0      = __rdtsc();
	go.store(true);
	for (auto& worker: workers) worker.join();
	const auto wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count();
	const auto ghz  = static_cast<double>(__rdtsc() - c0) / wall;
	uint64_t   sum  = 0;
	for (const auto value: cycles) sum += value;
	const double per_cycle = static_cast<double>(sum) / ghz / static_cast<double>(threads * rounds);
	uint64_t     waits[4] = {}, ns[4] = {};
	for (uint32_t kind = 0; kind < 4; kind++) {
		const auto taken = TrackingSpinLock::TakeContention(static_cast<SpinLockClass>(kind));
		waits[kind]      = taken.waits;
		ns[kind]         = taken.ns;
	}
	std::printf("{\"implementation\": \"%s\", \"threads\": %u, \"rounds\": %" PRIu64 ", \"wall_ms\": %.1f, \"ns_per_fault_cycle\": %.1f, "
	            "\"cycles_per_second\": %.0f, \"tracker_waits\": %" PRIu64 ", \"tracker_wait_ms\": %.1f, \"pages_waits\": %" PRIu64 ", \"pages_wait_ms\": %.1f}\n",
	            Implementation, threads, rounds, wall / 1e6, per_cycle, static_cast<double>(threads * rounds) / (wall / 1e9),
	            waits[1], static_cast<double>(ns[1]) / 1e6, waits[2], static_cast<double>(ns[2]) / 1e6);
	(void)tracker.release();
	(void)pages.release();
	return 0;
}

struct Invariants {
	uint64_t regions = 0, pages = 0, both = 0, write_mismatch = 0, access_mismatch = 0, flag_mismatch = 0, deferred = 0;
};

Invariants CheckState(MemoryTracker& tracker, PageManager& pages) {
	std::vector<uint64_t> words;
	const auto            emit = [&](const uint64_t* data, size_t count) { words.insert(words.end(), data, data + count); };
#if defined(ANYPS5_KYTY_RUST_TRACKER)
	kyty_tr_debug_state(
	    tracker.RustHandle(), nullptr,
	    [](void* context, const uint64_t* data, size_t count) { (*static_cast<decltype(emit)*>(context))(data, count); },
	    const_cast<void*>(static_cast<const void*>(&emit)));
	const size_t tracker_words = words.size();
	kyty_tr_debug_state(
	    nullptr, static_cast<const KytyTrPages*>(pages.RustHandle()),
	    [](void* context, const uint64_t* data, size_t count) { (*static_cast<decltype(emit)*>(context))(data, count); },
	    const_cast<void*>(static_cast<const void*>(&emit)));
#else
	tracker.DebugState(emit);
	const size_t tracker_words = words.size();
	pages.DebugState([](void* context, const uint64_t* data, size_t count) { (*static_cast<decltype(emit)*>(context))(data, count); },
	                 const_cast<void*>(static_cast<const void*>(&emit)));
#endif
	constexpr size_t W = TRACKER_REGION_PAGES / 64;
	struct Region {
		uint64_t cpu[W], gpu[W];
		uint64_t has_cpu, has_gpu, deferred;
	};
	std::map<uint64_t, Region> regions;
	size_t at = 0;
	while (at < tracker_words && words[at] != UINT64_MAX) {
		Region region {};
		const uint64_t index = words[at++];
		std::memcpy(region.cpu, &words[at], W * 8);
		std::memcpy(region.gpu, &words[at + W], W * 8);
		at += 7 * W;
		region.has_cpu  = words[at + 1];
		region.has_gpu  = words[at + 2];
		region.deferred = words[at + 6];
		const bool busy = words[at + 8] != 0;
		at += 9;
		if (busy) at += TRACKER_REGION_PAGES + TRACKER_REGION_PAGES / 8 + W;
		regions[index] = region;
	}
	Invariants result;
	result.regions = regions.size();
	for (const auto& [index, region]: regions) {
		bool any_cpu = false, any_gpu = false;
		for (size_t word = 0; word < W; word++) {
			any_cpu |= region.cpu[word] != 0;
			any_gpu |= region.gpu[word] != 0;
			result.both += std::popcount(region.cpu[word] & region.gpu[word]);
		}
		result.flag_mismatch += (region.has_cpu != 0) != any_cpu;
		result.flag_mismatch += (region.has_gpu != 0) != any_gpu;
		result.deferred += region.deferred;
	}
	at = tracker_words;
	while (at < words.size() && words[at] != UINT64_MAX) {
		const uint64_t index = words[at++];
		const auto     it    = regions.find(index);
		for (size_t page = 0; page < TRACKER_REGION_PAGES; page++) {
			const uint64_t state  = words[at + page];
			const auto     write  = state & 0xFFu;
			const auto     access = (state >> 8) & 0xFFu;
			if (it == regions.end()) continue;
			result.pages++;
			const bool cpu = (it->second.cpu[page / 64] >> (page % 64)) & 1u;
			const bool gpu = (it->second.gpu[page / 64] >> (page % 64)) & 1u;
			result.write_mismatch += write != (cpu ? 0u : 1u);
			result.access_mismatch += access != (gpu ? 1u : 0u);
		}
		at += TRACKER_REGION_PAGES;
	}
	return result;
}

int Stress(unsigned threads, uint64_t rounds, uint64_t seed) {
	::setenv("KYTY_ASYNC_REPROTECT", "1", 1);
	TT::g_enabled = false;
	auto pages    = std::make_unique<PageManager>();
	auto tracker  = std::make_unique<MemoryTracker>(*pages);
	constexpr uint64_t base    = 0x2'0000'0000ull;
	constexpr uint64_t regions = 2;
	constexpr uint64_t span    = regions * TRACKER_REGION_SIZE;
	std::mutex                                   queue_mutex;
	std::vector<MemoryTracker::DeferredProtect> queue;
	std::atomic<bool>                            done {false};
	std::atomic<uint64_t>                        applied {0};
	std::thread host_copies([&] {
		for (;;) {
			std::vector<MemoryTracker::DeferredProtect> batch;
			{
				std::lock_guard lock(queue_mutex);
				batch.swap(queue);
			}
			for (const auto& item: batch) RegionManager::ApplyDeferred(item.manager, item.address, item.size);
			applied.fetch_add(batch.size());
			if (batch.empty()) {
				if (done.load()) break;
				std::this_thread::yield();
			}
		}
	});
	std::vector<std::thread> workers;
	std::atomic<uint32_t>    sync {1};
	for (unsigned index = 0; index < threads; index++) {
		workers.emplace_back([&, index] {
			uint64_t state = seed * 0x9E3779B97F4A7C15ull + index * 0xBF58476D1CE4E5B9ull + 1;
			const auto next = [&] {
				state ^= state << 13;
				state ^= state >> 7;
				state ^= state << 17;
				return state;
			};
			for (uint64_t round = 0; round < rounds; round++) {
				const uint64_t page    = base + (next() % (span / TRACKER_PAGE_SIZE)) * TRACKER_PAGE_SIZE;
				const uint64_t pages_n = 1 + next() % 24;
				const uint64_t size    = std::min(pages_n * TRACKER_PAGE_SIZE, base + span - page);
				switch (next() % 10) {
					case 0:
					case 1: (void)tracker->OpenWriteBlock(page, 64 * 1024, (next() & 1) != 0); break;
					case 2: tracker->InvalidateRegion(page, size, [] {}); break;
					case 3:
					case 4: {
						const MemoryTracker::UploadScope scope;
						const uint32_t device_sync = (next() & 1) != 0 ? sync.fetch_add(1) : 0;
						tracker->ForEachUploadRange(
						    page, size, false, [](uint64_t, uint64_t) noexcept {},
						    [&]() noexcept {
							    auto& deferred = MemoryTracker::DeferredProtects();
							    if (!deferred.empty() && (next() & 1) != 0) {
								    std::lock_guard lock(queue_mutex);
								    queue.insert(queue.end(), deferred.begin(), deferred.end());
								    deferred.clear();
							    }
						    },
						    device_sync);
						break;
					}
					case 5: tracker->MarkRegionAsGpuModifiedUnlessCpuDirty(page, size, [](uint64_t, uint64_t) {}); break;
					case 6: tracker->ForEachDownloadRange<true>(page, size, [](uint64_t, uint64_t) noexcept {}); break;
					case 7: tracker->ConsumeCpuWrites(static_cast<CpuWriteConsumer>(next() & 1), page, size, [](uint64_t, uint64_t) {}); break;
					case 8: (void)tracker->IsRegionCpuModified(page, size); (void)tracker->IsRegionGpuModified(page, size); break;
					default: {
						tracker->ForEachUploadRange(page, size, true, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
						break;
					}
				}
			}
		});
	}
	for (auto& worker: workers) worker.join();
	done.store(true);
	host_copies.join();
	const auto check = CheckState(*tracker, *pages);
	std::printf("{\"implementation\": \"%s\", \"threads\": %u, \"rounds\": %" PRIu64 ", \"seed\": %" PRIu64 ", \"regions\": %" PRIu64
	            ", \"pages\": %" PRIu64 ", \"cpu_and_gpu\": %" PRIu64 ", \"write_watch_mismatch\": %" PRIu64 ", \"access_watch_mismatch\": %" PRIu64
	            ", \"flag_mismatch\": %" PRIu64 ", \"deferred_left\": %" PRIu64 ", \"deferred_applied_async\": %" PRIu64 "}\n",
	            Implementation, threads, rounds, seed, check.regions, check.pages, check.both, check.write_mismatch, check.access_mismatch,
	            check.flag_mismatch, check.deferred, applied.load());
	(void)tracker.release();
	(void)pages.release();
	return (check.both | check.write_mismatch | check.access_mismatch | check.flag_mismatch | check.deferred) != 0 ? 1 : 0;
}

int main(int argc, char** argv) {
	if (argc >= 3 && std::string(argv[1]) == "--stress") {
		const auto threads = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
		const auto rounds  = argc >= 4 ? std::strtoull(argv[3], nullptr, 10) : 100000;
		const auto seed    = argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 1;
		return Stress(threads, rounds, seed);
	}
	if (argc >= 3 && std::string(argv[1]) == "--contend") {
		const auto threads = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
		const auto rounds  = argc >= 4 ? std::strtoull(argv[3], nullptr, 10) : 200000;
		return Contend(threads, rounds);
	}
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <trace> [--out <file>] [--bench] [--digest-every N] [--limit N] [--skip-gaps] [--stats] [--hot]\n", argv[0]);
		return 2;
	}
	const char* out_path     = nullptr;
	bool        bench        = false;
	bool        skip_gaps    = false;
	bool        stats_only   = false;
	bool        apply_taken  = false;
	bool        profile      = false;
	uint64_t    history_page = 0;
	uint64_t    history_until = UINT64_MAX;
	uint64_t    digest_every = 0;
	uint64_t    limit        = UINT64_MAX;
	for (int arg = 2; arg < argc; arg++) {
		const std::string option = argv[arg];
		if (option == "--out" && arg + 1 < argc) out_path = argv[++arg];
		else if (option == "--bench") bench = true;
		else if (option == "--skip-gaps") skip_gaps = true;
		else if (option == "--stats") stats_only = true;
		else if (option == "--hot") g_hot = true;
		else if (option == "--apply-taken") apply_taken = true;
		else if (option == "--profile") profile = true;
		else if (option == "--history" && arg + 1 < argc) history_page = std::strtoull(argv[++arg], nullptr, 16);
		else if (option == "--until" && arg + 1 < argc) history_until = std::strtoull(argv[++arg], nullptr, 10);
		else if (option == "--digest-every" && arg + 1 < argc) digest_every = std::strtoull(argv[++arg], nullptr, 10);
		else if (option == "--limit" && arg + 1 < argc) limit = std::strtoull(argv[++arg], nullptr, 10);
	}
	if (!g_hot) ::unsetenv("KYTY_HOT_PAGE_WRITE_WATCH");
	else ::setenv("KYTY_HOT_PAGE_WRITE_WATCH", "1", 1);
	const auto trace = MapTrace(argv[1]);
	const auto index = Scan(trace, skip_gaps);
	std::fprintf(stderr,
	             "[replay %s] %" PRIu64 " records (%zu top-level calls, %zu nested, %" PRIu64 " threads, %" PRIu64 " loose events, %" PRIu64
	             " unreadable words, %" PRIu64 " calls with dropped events), last seq %" PRIu64 ", replay stops before seq %" PRIu64 "\n",
	             Implementation, index.records, index.top.size(), index.nested.size(), index.threads, index.loose, index.skipped,
	             index.overflows, index.max_seq, index.stop);
	if (stats_only) return 0;
	if (profile) {
		Profile(trace, index);
		return 0;
	}
	if (history_page != 0) {
		History(trace, index, history_page & ~uint64_t {4095}, history_until);
		return 0;
	}
	for (const auto& [seq, offset]: index.top) {
		const auto view = Decode(trace.words + offset);
		if (view.op == static_cast<uint8_t>(TT::Op::TrackerCreate)) {
			::setenv("KYTY_ASYNC_REPROTECT", view.Arg(2) != 0 ? "1" : "0", 1);
			break;
		}
	}
	TT::g_enabled = true;
	FILE* out     = out_path != nullptr ? std::fopen(out_path, "wb") : nullptr;
	if (out != nullptr) std::setvbuf(out, nullptr, _IOFBF, 16 << 20);
	Driver driver(trace, index, out, bench, digest_every, limit);
	driver.m_apply_taken = apply_taken;
	const int code = driver.Run();
	if (out != nullptr) std::fclose(out);
	return code;
}
