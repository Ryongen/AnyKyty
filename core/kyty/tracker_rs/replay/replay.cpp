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
#include <map>
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

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <trace> [--out <file>] [--bench] [--digest-every N] [--limit N] [--skip-gaps] [--stats] [--hot]\n", argv[0]);
		return 2;
	}
	const char* out_path     = nullptr;
	bool        bench        = false;
	bool        skip_gaps    = false;
	bool        stats_only   = false;
	bool        apply_taken  = false;
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
