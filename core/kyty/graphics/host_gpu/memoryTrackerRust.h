#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKERRUST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKERRUST_H_

#include "common/assert.h"
#include "graphics/host_gpu/commandBlocks.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionManager.h"

#include "kyty_tracker_rs.h"

#include <cstdlib>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size) { return kyty_tr_is_region_cpu_modified(m_rust, vaddr, size); }
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size) { return kyty_tr_is_region_gpu_modified(m_rust, vaddr, size); }
	[[nodiscard]] bool MayBeGpuModified(uint64_t vaddr, uint64_t size) const { return kyty_tr_may_be_gpu_modified(m_rust, vaddr, size); }
	void MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) { kyty_tr_mark_region_as_cpu_modified(m_rust, vaddr, size); }
	void UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) { kyty_tr_unmark_region_as_gpu_modified(m_rust, vaddr, size); }

	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		kyty_tr_invalidate_region(m_rust, vaddr, size, &CallVoid<std::remove_reference_t<Flush>>, Context(on_flush));
	}

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size, const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size, const char* operation);
#else
	void ValidateGpuDirtyPages(const RangeSet&, uint64_t, uint64_t, const char*) const noexcept {}
	void ValidateGpuDirtyOwnership(const RangeSet&, uint64_t, uint64_t, const char*) {}
#endif

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		kyty_tr_for_each_download_range(m_rust, vaddr, size, clear, &CallRange<std::remove_reference_t<Func>>, Context(func));
	}

	[[nodiscard]] uint32_t RegionKeptSync(uint64_t index) const { return kyty_tr_region_kept_sync(m_rust, index); }

	void NoteGuestWriteFault(uint64_t vaddr) { kyty_tr_note_guest_write_fault(m_rust, vaddr); }

	[[nodiscard]] RegionManager::WriteBlockProbe OpenWriteBlock(uint64_t vaddr, uint64_t block_size, bool open_block) {
		const auto probe = kyty_tr_open_write_block(m_rust, vaddr, block_size, open_block);
		return {.opened = probe.opened, .gpu = probe.gpu, .tracked = probe.tracked};
	}

	void MarkCommandBlocks(uint64_t vaddr, uint64_t size) { m_command_blocks.Mark(vaddr, size); }
	void MarkSegmentBlocks(uint64_t vaddr, uint64_t size) { m_segment_blocks.Mark(vaddr, size); }
	void ForgetCommandBlocks(uint64_t vaddr, uint64_t size) {
		m_command_blocks.Forget(vaddr, size);
		m_segment_blocks.Forget(vaddr, size);
	}
	[[nodiscard]] bool IsCommandBlock(uint64_t vaddr) const noexcept { return m_command_blocks.Contains(vaddr); }
	[[nodiscard]] bool IsSegmentBlock(uint64_t vaddr) const noexcept { return m_segment_blocks.Contains(vaddr); }

	[[nodiscard]] bool TakeKeptPages() { return kyty_tr_take_kept_pages(m_rust); }
	[[nodiscard]] uint64_t RegionCpuDirtyEpoch(uint64_t index) const { return kyty_tr_region_cpu_dirty_epoch(m_rust, index); }
	[[nodiscard]] uint64_t CpuModificationEpoch(uint64_t vaddr, uint64_t size) const {
		return kyty_tr_cpu_modification_epoch(m_rust, vaddr, size);
	}
	[[nodiscard]] bool DeferredProtectionPending(uint64_t vaddr) const { return kyty_tr_deferred_protection_pending(m_rust, vaddr); }
	void NoteHostWrite(uint64_t vaddr, uint64_t size) { kyty_tr_note_host_write(m_rust, vaddr, size); }
	[[nodiscard]] uint64_t RegionGpuCleanEpoch(uint64_t index) const { return kyty_tr_region_gpu_clean_epoch(m_rust, index); }

	template <typename Func>
	void ForEachCpuDirtySpan(uint64_t vaddr, uint64_t size, uint64_t max_gap, Func&& func) {
		kyty_tr_for_each_cpu_dirty_span(m_rust, vaddr, size, max_gap, &CallRange<std::remove_reference_t<Func>>, Context(func));
	}

	bool CopyRegionCpuDirty(uint64_t index, RegionBits& pages) {
		uint64_t words[TRACKER_REGION_PAGES / 64];
		for (size_t word = 0; word < TRACKER_REGION_PAGES / 64; word++) {
			words[word] = pages.Word(word);
		}
		const bool result = kyty_tr_copy_region_cpu_dirty(m_rust, index, words);
		for (size_t word = 0; word < TRACKER_REGION_PAGES / 64; word++) {
			pages.SetWord(word, words[word]);
		}
		return result;
	}

	template <typename Func>
	void ConsumeCpuWrites(CpuWriteConsumer consumer, uint64_t vaddr, uint64_t size, Func&& func) {
		kyty_tr_consume_cpu_writes(m_rust, static_cast<uint32_t>(consumer), vaddr, size, &CallRange<std::remove_reference_t<Func>>, Context(func));
	}

	template <typename Func>
	void MarkRegionAsGpuModifiedUnlessCpuDirty(uint64_t vaddr, uint64_t size, Func&& func) {
		kyty_tr_mark_region_as_gpu_modified_unless_cpu_dirty(m_rust, vaddr, size, &CallRange<std::remove_reference_t<Func>>, Context(func));
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func, UploadFunc&& upload_func,
	                        uint32_t device_sync = 0) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		const bool defer = !is_written && DeferProtections();
		Upload<std::remove_reference_t<RangeFunc>, std::remove_reference_t<UploadFunc>> context {range_func, upload_func};
		kyty_tr_for_each_upload_range(m_rust, vaddr, size, is_written, device_sync, defer, &context.Range, &context.Uploaded, Context(context));
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRanges(std::span<const std::pair<uint64_t, uint64_t>> ranges, RangeFunc&& range_func, UploadFunc&& upload_func,
	                         uint32_t device_sync = 0) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, size_t, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		static_assert(sizeof(std::pair<uint64_t, uint64_t>) == 2 * sizeof(uint64_t));
		const bool defer = DeferProtections();
		Upload<std::remove_reference_t<RangeFunc>, std::remove_reference_t<UploadFunc>> context {range_func, upload_func};
		kyty_tr_for_each_upload_ranges(m_rust, reinterpret_cast<const uint64_t*>(ranges.data()), ranges.size(), device_sync, defer,
		                               &context.Indexed, &context.Uploaded, Context(context));
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
	[[nodiscard]] static bool AsyncReprotect() noexcept {
		static const bool on = [] {
			const char* value = std::getenv("KYTY_ASYNC_REPROTECT");
			return value == nullptr || value[0] != '0';
		}();
		return on;
	}
	static void PushDeferred(const void* region, uint64_t address, uint64_t size) {
		s_deferred.push_back({const_cast<RegionManager*>(static_cast<const RegionManager*>(region)), address, size});
	}
	[[nodiscard]] const KytyTrTracker* RustHandle() const noexcept { return m_rust; }

private:
	template <typename Value>
	static void* Context(Value& value) noexcept {
		return const_cast<void*>(static_cast<const void*>(std::addressof(value)));
	}

	template <typename Func>
	static void CallVoid(void* context) {
		(*static_cast<Func*>(context))();
	}

	template <typename Func>
	static void CallRange(void* context, uint64_t address, uint64_t size) {
		(*static_cast<Func*>(context))(address, size);
	}

	template <typename RangeFunc, typename UploadFunc>
	struct Upload {
		RangeFunc&  range_func;
		UploadFunc& upload_func;

		static void Range(void* context, uint64_t address, uint64_t size) {
			static_cast<Upload*>(context)->range_func(address, size);
		}
		static void Indexed(void* context, size_t index, uint64_t address, uint64_t size) {
			static_cast<Upload*>(context)->range_func(index, address, size);
		}
		static void Uploaded(void* context) {
			static_cast<Upload*>(context)->upload_func();
			ApplyDeferredProtections();
		}
	};

	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	static_assert(CommandBlocks::AddressSize == TRACKER_ADDRESS_SIZE);
	static_assert(TRACKER_REGION_SIZE % CommandBlocks::BlockSize == 0);

	inline static thread_local bool s_upload_takes_protections = false;
#if defined(ANYPS5_KYTY) && defined(_WIN32) && (!defined(__clang__) || defined(ANYPS5_KYTY_DLL))
	static thread_local std::vector<DeferredProtect> s_deferred;
#else
	inline static thread_local std::vector<DeferredProtect> s_deferred;
#endif
	[[nodiscard]] static bool DeferProtections() noexcept { return s_upload_takes_protections && AsyncReprotect(); }
	static void ApplyDeferredProtections() {
		for (const auto& deferred: s_deferred) RegionManager::ApplyDeferred(deferred.manager, deferred.address, deferred.size);
		s_deferred.clear();
	}

	CommandBlocks  m_command_blocks;
	CommandBlocks  m_segment_blocks;
	KytyTrTracker* m_rust = nullptr;
	PageManager&   m_page_manager;
};

}

#endif
