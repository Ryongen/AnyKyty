#if defined(ANYPS5_KYTY_RUST_TRACKER)

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "common/timer.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/bdaDirtyRegions.h"
#include "graphics/host_gpu/hotWriteWatch.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/trackerTrace.h"
#include "kernel/memory.h"

#include "kyty_tracker_rs.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Libs::Graphics {

namespace {

[[noreturn]] void PagesFatal(const char* text) {
	std::fprintf(stderr, "PageManager fatal: %s\n", text);
	std::fflush(stderr);
	Log::WriteToSessionLog(std::string("PageManager fatal: ") + text + "\n");
	Log::Flush();
	std::_Exit(322);
}

bool Protect(uint64_t vaddr, uint64_t size, uint32_t mode) {
	TrackerTrace::Event(TrackerTrace::Ev::Protect, vaddr, size, mode);
	return LibKernel::Memory::ProtectGuestHostMemory(vaddr, size, static_cast<Common::VirtualMemory::Mode>(mode));
}

void BdaMark(uint64_t region) {
	TrackerTrace::Event(TrackerTrace::Ev::BdaMark, region, BdaDirtyRegions::g_translating);
	BdaDirtyRegions::Mark(region);
}

void DeferProtect(const void* region, uint64_t vaddr, uint64_t size) {
	TrackerTrace::Event(TrackerTrace::Ev::DeferProtect, vaddr, size);
	MemoryTracker::PushDeferred(region, vaddr, size);
}

bool HasReservedCore() {
	return Common::Thread::HasReservedCore();
}

uint64_t SteadyNs() {
	return Common::SteadyNs();
}

void Fatal(uint32_t code, uint64_t a, uint64_t b) {
	char text[256];
	switch (code) {
		case KYTY_TR_FATAL_TRACKER_INVALID_RANGE: EXIT("invalid memory tracker range\n");
		case KYTY_TR_FATAL_TRACKER_INVALID_REGION: EXIT("invalid memory tracker region\n");
		case KYTY_TR_FATAL_CPU_CONFLICTS_GPU: EXIT("CPU dirty state conflicts with GPU dirty state\n");
		case KYTY_TR_FATAL_LOCK_REENTERED: Common::DbgNotImplementedHandler("m_owner.load(std::memory_order_relaxed) == thread", __FILE__, __LINE__);
		case KYTY_TR_FATAL_UNLOCK_BY_OTHER_THREAD:
			Common::DbgNotImplementedHandler("m_owner.load(std::memory_order_relaxed) != CurrentThread()", __FILE__, __LINE__);
		case KYTY_TR_FATAL_PAGES_INVALID_RANGE:
			std::snprintf(text, sizeof(text), "invalid range vaddr=0x%016" PRIx64 ", size=0x%016" PRIx64, a, b);
			break;
		case KYTY_TR_FATAL_READ_WATCHER_OVERFLOW: std::snprintf(text, sizeof(text), "read-watcher overflow at 0x%016" PRIx64, a); break;
		case KYTY_TR_FATAL_READ_WATCHER_UNDERFLOW: std::snprintf(text, sizeof(text), "read-watcher underflow at 0x%016" PRIx64, a); break;
		case KYTY_TR_FATAL_WRITE_WATCHER_OVERFLOW: std::snprintf(text, sizeof(text), "write-watcher overflow at 0x%016" PRIx64, a); break;
		case KYTY_TR_FATAL_WRITE_WATCHER_UNDERFLOW: std::snprintf(text, sizeof(text), "write-watcher underflow at 0x%016" PRIx64, a); break;
		case KYTY_TR_FATAL_UNTRACK_UNKNOWN_PAGE: std::snprintf(text, sizeof(text), "untracking unknown page 0x%016" PRIx64, a); break;
		case KYTY_TR_FATAL_UNTRACK_UNKNOWN_REGION: std::snprintf(text, sizeof(text), "untracking unknown region 0x%016" PRIx64, a); break;
		case KYTY_TR_FATAL_PROTECT_FAILED:
			std::snprintf(text, sizeof(text), "address-space protection failed at 0x%016" PRIx64 ", mode=0x%08" PRIx32, a,
			              static_cast<uint32_t>(b));
			break;
		case KYTY_TR_FATAL_DESTROYED_WITH_LIVE: std::snprintf(text, sizeof(text), "destroyed with live page state"); break;
		default: std::snprintf(text, sizeof(text), "memory tracker reservation of 0x%016" PRIx64 " bytes failed", a); break;
	}
	PagesFatal(text);
}

bool HotWatch(uint64_t begin, uint64_t end) {
	return HotWriteWatch::Get().Watch(begin, end);
}

void HotUnwatch(uint64_t begin, uint64_t end) {
	HotWriteWatch::Get().Unwatch(begin, end);
}

bool HotCollect(uint64_t begin, uint64_t end, KytyTrRangeFn written, void* context, bool strict) {
	return HotWriteWatch::Get().Collect(begin, end, written, context, strict);
}

void HotLog(const uint64_t* values, size_t count) {
	if (count < 16) {
		return;
	}
	TrackerTrace::Event(TrackerTrace::Ev::HotLog, values, count);
	const auto [elapsed, faults, read_only, writable, no_access, other, page_total, run_total, scans, scoped, scan_ns, collected, promoted,
	            idle, lost, refused] = std::array<uint64_t, 16> {values[0], values[1], values[2], values[3], values[4], values[5],
	                                                             values[6], values[7], values[8], values[9], values[10], values[11],
	                                                             values[12], values[13], values[14], values[15]};
	char line[896];
	std::snprintf(line, sizeof(line),
	              "[kyty-hot-pages] summary of the last %.0f s: %" PRIu64 " guest write faults, %" PRIu64 " page protections (%" PRIu64
	              " read only, %" PRIu64 " writable, %" PRIu64 " no access, %" PRIu64 " other); %" PRIu64
	              " pages watched without a fault in %" PRIu64 " clusters, %" PRIu64 " collections and %" PRIu64
	              " before GPU writes in %.3f s, %" PRIu64 " page writes collected, %" PRIu64 " pages promoted, %" PRIu64
	              " demoted when idle, %" PRIu64 " that lost their watch, %" PRIu64 " refused\n",
	              static_cast<double>(elapsed) / 1e9, faults, read_only + writable + no_access + other, read_only, writable, no_access, other,
	              page_total, run_total, scans, scoped, static_cast<double>(scan_ns) / 1e9, collected, promoted, idle, lost, refused);
	Log::WriteToConsoleAndLog(line);
}

const KytyTrHost g_host {
    .protect           = Protect,
    .bda_mark          = BdaMark,
    .defer_protect     = DeferProtect,
    .has_reserved_core = HasReservedCore,
    .steady_ns         = SteadyNs,
    .fatal             = Fatal,
    .hot_watch         = HotWatch,
    .hot_unwatch       = HotUnwatch,
    .hot_collect       = HotCollect,
    .hot_log           = HotLog,
};

}

const KytyTrHost& TrackerRustHost() noexcept {
	return g_host;
}

}

#endif
