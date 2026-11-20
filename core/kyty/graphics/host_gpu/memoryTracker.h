#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_

#include "common/assert.h"
#include "graphics/host_gpu/commandBlocks.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/trackerTrace.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(ANYPS5_KYTY_RUST_TRACKER)
#include "graphics/host_gpu/memoryTrackerRust.h"
#else

namespace Libs::Graphics {

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool MayBeGpuModified(uint64_t vaddr, uint64_t size) const {
		TrackerTrace::Call trace(TrackerTrace::Op::MayBeGpuModified, vaddr, size);
		const bool         result = MayBeGpuModifiedUntraced(vaddr, size);
		trace.Result(result);
		return result;
	}
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		TrackerTrace::Call trace(TrackerTrace::Op::InvalidateRegion, vaddr, size);
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const bool should_flush = [&] {
				std::scoped_lock lock(manager->lock);
				if (manager->IsModified<DirtySource::Gpu>(offset, bytes)) {
					return true;
				}
				manager->ChangeState<DirtySource::Cpu, true>(offset, bytes);
				return false;
			}();
			if (should_flush) {
				trace.Add(TrackerTrace::Ev::Flush, manager->CpuAddress() + offset, bytes);
				const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Flush);
				on_flush();
			}
		});
	}
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                           const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                               const char* operation);
#else
	void ValidateGpuDirtyPages(const RangeSet&, uint64_t, uint64_t, const char*) const noexcept {}
	void ValidateGpuDirtyOwnership(const RangeSet&, uint64_t, uint64_t, const char*) {}
#endif

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		TrackerTrace::Call trace(TrackerTrace::Op::ForEachDownloadRange, vaddr, size, clear);
		const auto         traced = [&](uint64_t address, uint64_t bytes) noexcept {
			trace.Add(TrackerTrace::Ev::Range, address, bytes);
			const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Range);
			func(address, bytes);
		};
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->ForEachModifiedRange<DirtySource::Gpu, false>(offset, bytes, traced);
			if constexpr (clear) {
				manager->ChangeState<DirtySource::Gpu, false>(offset, bytes);
			}
		});
	}

	[[nodiscard]] uint32_t RegionKeptSync(uint64_t index) const {
		TrackerTrace::Call trace(TrackerTrace::Op::RegionKeptSync, index);
		if (index >= REGION_COUNT) {
			EXIT("invalid memory tracker region\n");
		}
		const auto* manager = m_regions[index].load(std::memory_order_acquire);
		const auto  result  = manager != nullptr ? manager->KeptSync() : 0;
		trace.Result(result);
		return result;
	}

	void NoteGuestWriteFault(uint64_t vaddr) {
		TrackerTrace::Call trace(TrackerTrace::Op::NoteGuestWriteFault, vaddr);
		const auto         index = vaddr / TRACKER_REGION_SIZE;
		if (index >= REGION_COUNT) {
			return;
		}
		if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
			std::scoped_lock lock(manager->lock);
			manager->NoteGuestWriteFault(vaddr % TRACKER_REGION_SIZE);
		}
	}

	[[nodiscard]] RegionManager::WriteBlockProbe OpenWriteBlock(uint64_t vaddr, uint64_t block_size, bool open_block) {
		TrackerTrace::Call trace(TrackerTrace::Op::OpenWriteBlock, vaddr, block_size, open_block);
		const auto         probe = OpenWriteBlockUntraced(vaddr, block_size, open_block);
		trace.Result(probe.opened, probe.gpu, probe.tracked);
		return probe;
	}

	[[nodiscard]] RegionManager::WriteBlockProbe OpenWriteBlockUntraced(uint64_t vaddr, uint64_t block_size, bool open_block) {
		const auto index = vaddr / TRACKER_REGION_SIZE;
		if (index >= REGION_COUNT) {
			return {};
		}
		auto* manager = m_regions[index].load(std::memory_order_acquire);
		if (manager == nullptr) {
			return {};
		}
		std::scoped_lock lock(manager->lock);
		return manager->OpenWriteBlock(vaddr % TRACKER_REGION_SIZE, block_size, open_block);
	}

	void MarkCommandBlocks(uint64_t vaddr, uint64_t size) { m_command_blocks.Mark(vaddr, size); }
	void MarkSegmentBlocks(uint64_t vaddr, uint64_t size) { m_segment_blocks.Mark(vaddr, size); }
	void ForgetCommandBlocks(uint64_t vaddr, uint64_t size) {
		m_command_blocks.Forget(vaddr, size);
		m_segment_blocks.Forget(vaddr, size);
	}
	[[nodiscard]] bool IsCommandBlock(uint64_t vaddr) const noexcept { return m_command_blocks.Contains(vaddr); }
	[[nodiscard]] bool IsSegmentBlock(uint64_t vaddr) const noexcept { return m_segment_blocks.Contains(vaddr); }

	[[nodiscard]] bool TakeKeptPages() {
		TrackerTrace::Call trace(TrackerTrace::Op::TakeKeptPages);
		const bool         result = m_kept_pages.exchange(false, std::memory_order_relaxed);
		trace.Result(result);
		return result;
	}

	[[nodiscard]] uint64_t RegionCpuDirtyEpoch(uint64_t index) const {
		TrackerTrace::Call trace(TrackerTrace::Op::RegionCpuDirtyEpoch, index);
		if (index >= REGION_COUNT) {
			EXIT("invalid memory tracker region\n");
		}
		const auto* manager = m_regions[index].load(std::memory_order_acquire);
		const auto  result  = manager != nullptr ? manager->RegionCpuDirtyEpoch() : 0;
		trace.Result(result);
		return result;
	}

	[[nodiscard]] uint64_t CpuModificationEpoch(uint64_t vaddr, uint64_t size) const {
		TrackerTrace::Call trace(TrackerTrace::Op::CpuModificationEpoch, vaddr, size);
		const auto         result = CpuModificationEpochUntraced(vaddr, size);
		trace.Result(result);
		return result;
	}
	[[nodiscard]] uint64_t CpuModificationEpochUntraced(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || !GuestRange {vaddr, size}.Valid() ||
		    vaddr / TRACKER_REGION_SIZE != (vaddr + size - 1) / TRACKER_REGION_SIZE)
			return 0;
		const auto* manager = m_regions[vaddr / TRACKER_REGION_SIZE].load(std::memory_order_acquire);
		return manager == nullptr ? 0 : manager->CpuModificationEpoch();
	}
	[[nodiscard]] bool DeferredProtectionPending(uint64_t vaddr) const {
		TrackerTrace::Call trace(TrackerTrace::Op::DeferredProtectionPending, vaddr);
		bool               result = false;
		if (GuestRange {vaddr, 1}.Valid()) {
			const auto* manager = m_regions[vaddr / TRACKER_REGION_SIZE].load(std::memory_order_acquire);
			result              = manager != nullptr && manager->DeferredProtectionPending();
		}
		trace.Result(result);
		return result;
	}
	void NoteHostWrite(uint64_t vaddr, uint64_t size) {
		TrackerTrace::Call trace(TrackerTrace::Op::NoteHostWrite, vaddr, size);
		if (size == 0 || !GuestRange {vaddr, size}.Valid()) return;
		for (auto index = vaddr / TRACKER_REGION_SIZE; index * TRACKER_REGION_SIZE < vaddr + size; ++index)
			if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) manager->NoteHostWrite();
	}

	[[nodiscard]] uint64_t RegionGpuCleanEpoch(uint64_t index) const {
		TrackerTrace::Call trace(TrackerTrace::Op::RegionGpuCleanEpoch, index);
		if (index >= REGION_COUNT) {
			EXIT("invalid memory tracker region\n");
		}
		const auto* manager = m_regions[index].load(std::memory_order_acquire);
		const auto  result  = manager != nullptr ? manager->RegionGpuCleanEpoch() : 0;
		trace.Result(result);
		return result;
	}

	template <typename Func>
	void ForEachCpuDirtySpan(uint64_t vaddr, uint64_t size, uint64_t max_gap, Func&& func) {
		TrackerTrace::Call trace(TrackerTrace::Op::ForEachCpuDirtySpan, vaddr, size, max_gap);
		ValidateRange(vaddr, size);
		const auto                                  end = vaddr + size;
		std::vector<std::pair<uint64_t, uint64_t>> spans;
		const auto add = [&](uint64_t begin, uint64_t finish) {
			begin  = std::max(begin, vaddr);
			finish = std::min(finish, end);
			if (begin >= finish) {
				return;
			}
			if (!spans.empty() && begin <= spans.back().second + max_gap) {
				spans.back().second = finish;
			} else {
				spans.emplace_back(begin, finish);
			}
		};
		for (uint64_t index = vaddr / TRACKER_REGION_SIZE; index * TRACKER_REGION_SIZE < end;
		     index++) {
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			const auto start   = std::max(index * TRACKER_REGION_SIZE, vaddr);
			const auto finish  = std::min((index + 1) * TRACKER_REGION_SIZE, end);
			if (manager == nullptr) {
				add(start, finish);
				continue;
			}
			if (!manager->HasCpuDirty()) {
				continue;
			}
			std::scoped_lock lock(manager->lock);
			manager->ForEachModifiedRange<DirtySource::Cpu, false>(
			    start - index * TRACKER_REGION_SIZE, finish - start,
			    [&](uint64_t address, uint64_t bytes) { add(address, address + bytes); });
		}
		for (const auto& [begin, finish]: spans) {
			trace.Add(TrackerTrace::Ev::Range, begin, finish - begin);
			const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Range);
			func(begin, finish - begin);
		}
	}

	bool CopyRegionCpuDirty(uint64_t index, RegionBits& pages) {
		TrackerTrace::Call trace(TrackerTrace::Op::CopyRegionCpuDirty, index);
		const bool         result = CopyRegionCpuDirtyUntraced(index, pages);
		trace.Result(result);
		if (trace.Active() && result) {
			uint64_t words[TRACKER_REGION_PAGES / 64] = {};
			for (size_t page = 0; page < TRACKER_REGION_PAGES; page++) {
				words[page / 64] |= static_cast<uint64_t>(pages.Get(page)) << (page % 64);
			}
			TrackerTrace::Event(TrackerTrace::Ev::Bits, words, TRACKER_REGION_PAGES / 64);
		}
		return result;
	}

	bool CopyRegionCpuDirtyUntraced(uint64_t index, RegionBits& pages) {
		if (index >= REGION_COUNT) {
			EXIT("invalid memory tracker region\n");
		}
		auto* manager = m_regions[index].load(std::memory_order_acquire);
		if (manager == nullptr) {
			pages.Fill();
			return true;
		}
		if (!manager->HasCpuDirty()) {
			return false;
		}
		std::scoped_lock lock(manager->lock);
		return manager->CopyCpuDirty(pages);
	}

	template <typename Func>
	void ConsumeCpuWrites(CpuWriteConsumer consumer, uint64_t vaddr, uint64_t size, Func&& func) {
		TrackerTrace::Call trace(TrackerTrace::Op::ConsumeCpuWrites, static_cast<uint32_t>(consumer), vaddr, size);
		std::vector<std::pair<uint64_t, uint64_t>> runs;
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->ConsumeCpuWritten(consumer, offset, bytes,
			                           [&](uint64_t address, uint64_t length) {
				                           if (!runs.empty() &&
				                               runs.back().first + runs.back().second == address) {
					                           runs.back().second += length;
				                           } else {
					                           runs.emplace_back(address, length);
				                           }
			                           });
		});
		const auto end = vaddr + size;
		for (const auto& [address, length]: runs) {
			const auto begin  = std::max(address, vaddr);
			const auto finish = std::min(address + length, end);
			if (begin < finish) {
				trace.Add(TrackerTrace::Ev::Range, begin, finish - begin);
				const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Range);
				func(begin, finish - begin);
			}
		}
	}

	template <typename Func>
	void MarkRegionAsGpuModifiedUnlessCpuDirty(uint64_t vaddr, uint64_t size, Func&& func) {
		TrackerTrace::Call trace(TrackerTrace::Op::MarkRegionAsGpuModifiedUnlessCpuDirty, vaddr, size);
		std::vector<std::pair<uint64_t, uint64_t>> runs;
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->MarkGpuDirtyUnlessCpuDirty(
			    offset, bytes,
			    [&](uint64_t address, uint64_t length) { runs.emplace_back(address, length); });
		});
		for (const auto& [address, length]: runs) {
			trace.Add(TrackerTrace::Ev::Range, address, length);
			const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Range);
			func(address, length);
		}
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func, uint32_t device_sync = 0) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		const bool         defer = !is_written && DeferProtections();
		TrackerTrace::Call trace(TrackerTrace::Op::ForEachUploadRange, vaddr, size, is_written, device_sync, defer);
		const auto         traced = [&](uint64_t address, uint64_t bytes) noexcept {
			trace.Add(TrackerTrace::Ev::Range, address, bytes);
			const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Range);
			range_func(address, bytes);
		};
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			if (!is_written && (!manager->HasCpuDirty() ||
			                    !manager->IsModified<DirtySource::Cpu>(offset, bytes))) {
				return;
			}
			if (!is_written && device_sync != 0) {
				const std::pair<uint64_t, uint64_t> range {offset, bytes};
				std::scoped_lock                    lock(manager->lock);
				DeferScope deferred(defer, manager);
				if (manager->ConsumeCpuDirtyRangesKeepingBusy(
				        std::span(&range, 1), device_sync,
				        [&](size_t, uint64_t address, uint64_t run) noexcept { traced(address, run); })) {
					m_kept_pages.store(true, std::memory_order_relaxed);
				}
				return;
			}
			manager->lock.lock();
			{
				DeferScope deferred(defer, manager);
				manager->ForEachModifiedRange<DirtySource::Cpu, true>(offset, bytes, traced);
			}
			if (!is_written) {
				manager->lock.unlock();
			}
		});
		TracedUpload(trace, upload_func);
		ApplyDeferredProtections();
		if (is_written) {
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               manager->ChangeState<DirtySource::Gpu, true>(offset, bytes);
				               manager->lock.unlock();
			               });
		}
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRanges(std::span<const std::pair<uint64_t, uint64_t>> ranges,
	                         RangeFunc&& range_func, UploadFunc&& upload_func,
	                         uint32_t device_sync = 0) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, size_t, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		const bool         defer = DeferProtections();
		TrackerTrace::Call trace(TrackerTrace::Op::ForEachUploadRanges, ranges.size(), device_sync, defer);
		if (trace.Active()) {
			for (const auto& [address, bytes]: ranges) {
				trace.Add(TrackerTrace::Ev::ArgRange, address, bytes);
			}
		}
		struct Piece {
			uint64_t region;
			uint64_t offset;
			uint64_t size;
			size_t   range;
		};
		static thread_local std::vector<Piece>                         pieces;
		static thread_local std::vector<std::pair<uint64_t, uint64_t>> offsets;
		pieces.clear();
		for (size_t index = 0; index < ranges.size(); index++) {
			const auto [address, size] = ranges[index];
			ValidateRange(address, size);
			for (auto at = address; at < address + size;) {
				const auto region = at / TRACKER_REGION_SIZE;
				const auto end    = std::min(address + size, (region + 1) * TRACKER_REGION_SIZE);
				pieces.push_back({region, at - region * TRACKER_REGION_SIZE, end - at, index});
				at = end;
			}
		}
		for (size_t first = 0; first < pieces.size();) {
			auto last = first + 1;
			while (last < pieces.size() && pieces[last].region == pieces[first].region) {
				last++;
			}
			auto* manager = GetOrCreateRegion(pieces[first].region);
			if (manager->HasCpuDirty()) {
				offsets.clear();
				for (auto piece = first; piece < last; piece++) {
					offsets.emplace_back(pieces[piece].offset, pieces[piece].size);
				}
				std::scoped_lock lock(manager->lock);
				DeferScope       deferred(defer, manager);
				const auto piece_func = [&](size_t piece, uint64_t address, uint64_t bytes) noexcept {
					trace.Add(TrackerTrace::Ev::IndexedRange, pieces[first + piece].range, address, bytes);
					const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Range);
					range_func(pieces[first + piece].range, address, bytes);
				};
				const std::span<const std::pair<uint64_t, uint64_t>> region_ranges(offsets);
				if (device_sync == 0) {
					manager->ConsumeCpuDirtyRanges(region_ranges, piece_func);
				} else if (manager->ConsumeCpuDirtyRangesKeepingBusy(region_ranges, device_sync,
				                                                    piece_func)) {
					m_kept_pages.store(true, std::memory_order_relaxed);
				}
			}
			first = last;
		}
		TracedUpload(trace, upload_func);
		ApplyDeferredProtections();
	}

	struct DeferredProtect {
		RegionManager* manager = nullptr;
		uint64_t       address = 0, size = 0;
	};
	struct UploadScope {
		bool previous;
		UploadScope() noexcept: previous(std::exchange(s_upload_takes_protections, true)) {}
		~UploadScope() { s_upload_takes_protections = previous; }
		UploadScope(const UploadScope&)            = delete;
		UploadScope& operator=(const UploadScope&) = delete;
	};
	[[nodiscard]] static std::vector<DeferredProtect>& DeferredProtects() noexcept { return s_deferred; }
#if defined(KYTY_TRACKER_REPLAY)
	template <typename Emit>
	void DebugState(Emit&& emit) const {
		for (uint64_t index = 0; index < REGION_COUNT; index++) {
			const auto* manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr) continue;
			emit(&index, 1);
			manager->DebugState(emit);
		}
		const auto     globals  = RegionManager::DebugGlobals();
		const uint64_t tail[] = {UINT64_MAX, globals[0], globals[1], globals[2], m_kept_pages.load()};
		emit(tail, std::size(tail));
	}
#endif
	[[nodiscard]] static bool AsyncReprotect() noexcept {
		static const bool on = [] {
			const char* value = std::getenv("KYTY_ASYNC_REPROTECT");
			return value == nullptr || value[0] != '0';
		}();
		return on;
	}

private:
	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	static_assert(CommandBlocks::AddressSize == TRACKER_ADDRESS_SIZE);
	static_assert(TRACKER_REGION_SIZE % CommandBlocks::BlockSize == 0);

	inline static thread_local bool                         s_upload_takes_protections = false;
#if defined(ANYPS5_KYTY) && defined(_WIN32) && (!defined(__clang__) || defined(ANYPS5_KYTY_DLL))
	static thread_local std::vector<DeferredProtect> s_deferred;
#else
	inline static thread_local std::vector<DeferredProtect> s_deferred;
#endif
	[[nodiscard]] static bool DeferProtections() noexcept { return s_upload_takes_protections && AsyncReprotect(); }
	template <typename UploadFunc>
	static void TracedUpload(TrackerTrace::Call& trace, UploadFunc& upload_func) noexcept {
		if (!trace.Active()) [[likely]] {
			upload_func();
			return;
		}
		const auto before = s_deferred.size();
		{
			const TrackerTrace::Call::Window window(trace, TrackerTrace::Callback::Upload);
			upload_func();
		}
		trace.Add(TrackerTrace::Ev::DeferredTaken, before, s_deferred.size());
	}
	[[nodiscard]] bool MayBeGpuModifiedUntraced(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || size > TRACKER_ADDRESS_SIZE || vaddr > TRACKER_ADDRESS_SIZE - size) {
			return true;
		}
		const auto last = (vaddr + size - 1) / TRACKER_REGION_SIZE;
		for (auto index = vaddr / TRACKER_REGION_SIZE; index <= last && index < REGION_COUNT; index++) {
			const auto* manager = m_regions[index].load(std::memory_order_acquire);
			if (manager != nullptr && manager->HasGpuDirty()) {
				return true;
			}
		}
		return false;
	}
	struct DeferScope {
		RegionManager*                          manager = nullptr;
		std::vector<PageManager::DeferredRange> ranges;
		DeferScope(bool defer, RegionManager* region) noexcept: manager(defer ? region : nullptr) {
			if (manager != nullptr) PageManager::SetDeferredWriteProtectSink(&ranges);
		}
		~DeferScope() {
			if (manager == nullptr) return;
			PageManager::SetDeferredWriteProtectSink(nullptr);
			for (const auto& range: ranges) {
				manager->BeginDeferredProtection();
				s_deferred.push_back({manager, range.address, range.size});
			}
		}
		DeferScope(const DeferScope&)            = delete;
		DeferScope& operator=(const DeferScope&) = delete;
	};
	static void ApplyDeferredProtections() {
		for (const auto& deferred: s_deferred) deferred.manager->ApplyDeferredProtection(deferred.address, deferred.size);
		s_deferred.clear();
	}

	template <bool create, typename Func>
	bool Iterate(uint64_t vaddr, uint64_t size, Func&& func) {
		ValidateRange(vaddr, size);
		using Result = std::invoke_result_t<Func, RegionManager*, uint64_t, uint64_t>;
		constexpr bool returns_bool = std::is_same_v<Result, bool>;
		uint64_t       remaining    = size;
		uint64_t       index        = vaddr / TRACKER_REGION_SIZE;
		uint64_t       offset       = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr && create) {
				manager = GetOrCreateRegion(index);
			}
			if (manager != nullptr) {
				if constexpr (returns_bool) {
					if (func(manager, offset, bytes)) {
						return true;
					}
				} else {
					func(manager, offset, bytes);
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return false;
	}

	static void    ValidateRange(uint64_t vaddr, uint64_t size);
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::atomic<bool> m_kept_pages {false};
	CommandBlocks     m_command_blocks;
	CommandBlocks     m_segment_blocks;

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	PageManager&                                   m_page_manager;
};

}

#endif

#endif
