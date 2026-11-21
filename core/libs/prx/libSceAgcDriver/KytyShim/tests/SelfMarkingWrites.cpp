#include "TestVulkanDevice.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"
#include "kyty_selfmark_rs.h"
#include "prx/libSceAgcDriver/KytyShim/src/Faults.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/include/Api.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/mman.h>

namespace {

using namespace Libs::Graphics;
namespace Memory = Libs::LibKernel::Memory;

constexpr uint64_t Page         = TRACKER_PAGE_SIZE;
constexpr uint64_t Block        = 0x10000;
constexpr uint64_t GuestBytes   = 4 * Block;
constexpr uint64_t CodeBytes    = Block;
constexpr int      CpuReadWrite = 0x03;
constexpr uint64_t Slot         = 0x40;
constexpr int64_t  Displacement = 0x100;

constexpr std::array<uint8_t, 8> LongStore {0x48, 0x89, 0xB7, 0x00, 0x01, 0x00, 0x00, 0xC3};
constexpr std::array<uint8_t, 4> ShortStore {0x48, 0x89, 0x37, 0xC3};
constexpr std::array<uint8_t, 4> AddToMemory {0x48, 0x01, 0x37, 0xC3};

using Store = void (*)(uint64_t pointer, uint64_t value);

std::atomic<uint64_t> g_faults {0};
struct sigaction      g_previous {};

void CountFault(int signal, siginfo_t* info, void* context) {
	g_faults.fetch_add(1, std::memory_order_relaxed);
	g_previous.sa_sigaction(signal, info, context);
}

uint64_t BlockAt(uint64_t address, uint32_t index) {
	return (address + Block - 1) / Block * Block + uint64_t {index} * Block;
}

uint64_t Read(uint64_t address) {
	uint64_t value = 0;
	if (!Memory::TryReadBacking(address, &value, sizeof(value))) {
		return ~uint64_t {0};
	}
	return value;
}

void WriteOnThread(Store store, uint64_t pointer, uint64_t value) {
	std::thread writer([&] { store(pointer, value); });
	writer.join();
}

struct Fixture {
	GraphicContext&   graphics;
	RenderContext&    renderer;
	BufferCache&      buffers;
	CommandScheduler& scheduler;

	[[nodiscard]] bool GpuHolds(uint64_t address, uint64_t value) {
		const auto block            = address / Block * Block;
		const auto [buffer, offset] = buffers.ObtainBuffer(block, Block, false, false);
		Buffer copied(graphics, scheduler, MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eTransferDst, sizeof(value));
		copied.CopyFrom(scheduler.Current(), *buffer, offset + (address - block), 0, sizeof(value));
		const auto last = scheduler.CurrentTick();
		scheduler.Wait(last);
		scheduler.WaitPriorityOperations(last);
		copied.Invalidate(0, sizeof(value));
		uint64_t held = 0;
		std::memcpy(&held, copied.Mapped().data(), sizeof(held));
		return held == value;
	}

	void Clean(uint64_t block) {
		(void)buffers.ObtainBuffer(block, Block, false, false);
	}
};

const char* CheckFirstFaultsCorrectTheSite(Fixture& fixture, Store store, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	fixture.Clean(block);
	if (fixture.buffers.IsRegionCpuDirty(block, Block)) return "a buffer read did not leave its pages clean";

	WriteOnThread(store, target - Displacement, 0x1111);
	if (Read(target) != 0x1111) return "the first write did not reach guest memory";
	fixture.renderer.CollectHotPageWrites();
	fixture.Clean(block);
	if (fixture.buffers.IsRegionCpuDirty(block, Block)) return "the upload after the first write did not leave the pages clean";

	const auto before = g_faults.load();
	WriteOnThread(store, target - Displacement, 0x2222);
	if (g_faults.load() != before + 1) return "the write that corrects its site did not take exactly one fault";
	if (*reinterpret_cast<const volatile uint8_t*>(store) != 0xE9) return "the write site was not replaced by a jump";
	if (Read(target) != 0x2222) return "the corrected write did not reach guest memory";
	if (kyty_selfmark_armed(page) == 0) return "the page was not armed for self-marking writes";
	if (!kyty_selfmark_any(page, page + Page)) return "the corrected write left no card";
	if (fixture.buffers.IsRegionCpuDirty(page, Page)) return "the page was reported written before the synchronisation point";

	fixture.renderer.CollectHotPageWrites();
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "the synchronisation point did not report the written page";
	if (fixture.buffers.IsRegionCpuDirty(page + Page, Page)) return "the synchronisation point reported a page nobody wrote";
	if (kyty_selfmark_armed(page + Page) == 0) return "the first fault did not arm the other clean pages of the block";
	if (kyty_selfmark_any(page, page + Page)) return "the card survived its collection";
	if (!fixture.GpuHolds(target, 0x2222)) return "the GPU buffer does not hold the corrected write";
	if (fixture.buffers.IsRegionCpuDirty(page, Page)) return "the upload did not leave the self-marked page clean";
	if (kyty_selfmark_armed(page) == 0) return "the upload disarmed the self-marked page";
	return nullptr;
}

const char* CheckCorrectedWritesTakeNoFault(Fixture& fixture, Store store, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	for (uint64_t round = 0; round < 8; round++) {
		const auto value  = 0x3000 + round;
		const auto before = g_faults.load();
		WriteOnThread(store, target - Displacement, value);
		if (g_faults.load() != before) return "a corrected write to a self-marked page took a fault";
		if (Read(target) != value) return "a corrected write did not reach guest memory";
		if (fixture.buffers.IsRegionCpuDirty(page, Page)) return "a corrected write was reported before the synchronisation point";
		fixture.renderer.CollectHotPageWrites(page, Page);
		if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "a collection of the page's range did not report the write";
		if (!fixture.GpuHolds(target, value)) return "the GPU buffer does not hold a corrected write";
	}
	return nullptr;
}

const char* CheckShortSiteIsServedAtItsFault(Fixture& fixture, Store store, Store shorter, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	for (uint64_t round = 0; round < 3; round++) {
		const auto value  = 0x4440 + round;
		const auto before = g_faults.load();
		WriteOnThread(shorter, target + 8, value);
		if (g_faults.load() != before + 1) return "a short write site did not fault exactly once";
		if (*reinterpret_cast<const volatile uint8_t*>(shorter) != ShortStore[0]) return "a short write site was modified";
		if (Read(target + 8) != value) return "the write of a short site did not reach guest memory";
		if (kyty_selfmark_armed(page) == 0 || kyty_selfmark_armed(page) == KYTY_SELFMARK_DELTA_REFUSED) return "a short site gave the page back to faults";
		if (!kyty_selfmark_any(page, page + Page)) return "the write of a short site left no card";
		if (fixture.buffers.IsRegionCpuDirty(page, Page)) return "the write of a short site was reported before the synchronisation point";
		fixture.renderer.CollectHotPageWrites();
		if (!fixture.GpuHolds(target + 8, value)) return "the GPU buffer does not hold the write of a short site";
	}
	const auto before = g_faults.load();
	std::thread loop([&] {
		for (uint64_t round = 0; round < 256; round++) {
			shorter(target + 8, 0x5000 + round);
		}
	});
	loop.join();
	const auto faults = g_faults.load() - before;
	if (faults < 2 || faults > 60) return "a short site that keeps writing one page was not given the page after a few faults";
	if (Read(target + 8) != 0x5000 + 255) return "the last write of a short site in a loop is not in guest memory";
	if (kyty_selfmark_armed(page) != KYTY_SELFMARK_DELTA_REFUSED) return "the page of a short site in a loop was not given back to faults";
	if (kyty_selfmark_armed(page + 2 * Page) != KYTY_SELFMARK_DELTA_REFUSED) return "the block of a page given back to faults was left marked";
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "a page given back to faults was not reported at once";
	fixture.renderer.CollectHotPageWrites();
	if (!fixture.GpuHolds(target + 8, 0x5000 + 255)) return "the GPU buffer does not hold the last write of a short site in a loop";

	auto again = g_faults.load();
	WriteOnThread(store, target - Displacement, 0x6666);
	if (g_faults.load() != again + 1) return "a corrected site did not fault once on a page given back to faults";
	if (Read(target) != 0x6666) return "the write to a page given back to faults did not reach guest memory";
	if (kyty_selfmark_armed(page) != KYTY_SELFMARK_DELTA_REFUSED) return "a page given back to faults was marked again";
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "the fault of a corrected site on a page given back to faults did not report it at once";
	again = g_faults.load();
	WriteOnThread(store, target - Displacement, 0x6667);
	WriteOnThread(shorter, target + 8, 0x6668);
	if (g_faults.load() != again) return "a write to an open page took a fault";
	if (kyty_selfmark_any(page, page + Page)) return "a write to an open page left a card";
	if (!fixture.GpuHolds(target, 0x6667) || !fixture.GpuHolds(target + 8, 0x6668)) return "the GPU buffer does not hold the writes to an open page";
	return nullptr;
}

const char* CheckUnpatchableSiteKeepsTheFault(Fixture& fixture, Store store, Store adder, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	fixture.Clean(block);
	WriteOnThread(store, target - Displacement, 0x7000);
	if (kyty_selfmark_armed(page) == 0 || kyty_selfmark_armed(page) == KYTY_SELFMARK_DELTA_REFUSED) return "a corrected site did not arm a clean page";
	const auto before = g_faults.load();
	WriteOnThread(adder, target, 0x0123);
	if (g_faults.load() != before + 1) return "a write site that cannot be served did not fault once";
	if (*reinterpret_cast<const volatile uint8_t*>(adder) != AddToMemory[0]) return "a write site that cannot be served was modified";
	if (Read(target) != 0x7123) return "the write of a site that cannot be served did not reach guest memory";
	if (kyty_selfmark_armed(page) != KYTY_SELFMARK_DELTA_REFUSED) return "the page was not given back to faults after a write that cannot mark it";
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "the fault of a site that cannot be served did not report the page at once";
	fixture.renderer.CollectHotPageWrites();
	if (!fixture.GpuHolds(target, 0x7123)) return "the GPU buffer does not hold the write of a site that cannot be served";
	return nullptr;
}

const char* CheckWritersAndCollector(Fixture& fixture, Store store, uint64_t block) {
	constexpr uint32_t Writers = 4;
	constexpr uint64_t Rounds  = 20000;
	std::atomic<bool>  go {false};
	std::vector<std::thread> writers;
	for (uint32_t writer = 0; writer < Writers; writer++) {
		writers.emplace_back([&, writer] {
			while (!go.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			const auto target = block + (2 + writer) * Page + Slot;
			for (uint64_t round = 1; round <= Rounds; round++) {
				store(target - Displacement, (uint64_t {writer} << 32u) | round);
			}
		});
	}
	fixture.Clean(block);
	go.store(true, std::memory_order_release);
	for (uint32_t spin = 0; spin < 200; spin++) {
		fixture.renderer.CollectHotPageWrites();
		fixture.Clean(block);
	}
	for (auto& writer: writers) {
		writer.join();
	}
	fixture.renderer.CollectHotPageWrites();
	for (uint32_t writer = 0; writer < Writers; writer++) {
		const auto target = block + (2 + writer) * Page + Slot;
		const auto value  = (uint64_t {writer} << 32u) | Rounds;
		if (Read(target) != value) return "the last write of a concurrent writer is not in guest memory";
		if (!fixture.GpuHolds(target, value)) return "the GPU buffer does not hold the last write of a concurrent writer";
	}
	return nullptr;
}

int Report(const char* failure) {
	if (failure == nullptr) {
		return 0;
	}
	std::fprintf(stderr, "kyty_shim_self_marking_writes: FAILED: %s\n", failure);
	return 1;
}

int CheckSelfMarkingWrites(GraphicContext& graphics, uint64_t address, Store store, Store shorter, Store adder) {
	RenderContext    renderer(graphics);
	CommandProcessor producer(renderer, 0);
	producer.Reset();
	auto& scheduler = renderer.GetCommandScheduler();
	auto& buffers   = renderer.GetBufferCache();
	scheduler.Begin(producer.GetCtx(), producer.GetUcfg(), producer.GetShCtx());
	Memory::InstallGpuResources(&renderer);
	KytyShim::InstallFaultChain();
	struct sigaction counting {};
	counting.sa_sigaction = &CountFault;
	counting.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigemptyset(&counting.sa_mask);
	sigaction(SIGSEGV, &counting, &g_previous);

	Fixture    fixture {graphics, renderer, buffers, scheduler};
	const auto first    = BlockAt(address, 0);
	int        failures = Report(CheckFirstFaultsCorrectTheSite(fixture, store, first));
	if (failures == 0) failures += Report(CheckCorrectedWritesTakeNoFault(fixture, store, first));
	if (failures == 0) failures += Report(CheckShortSiteIsServedAtItsFault(fixture, store, shorter, first));
	if (failures == 0) failures += Report(CheckWritersAndCollector(fixture, store, BlockAt(address, 1)));
	if (failures == 0) failures += Report(CheckUnpatchableSiteKeepsTheFault(fixture, store, adder, BlockAt(address, 2)));

	sigaction(SIGSEGV, &g_previous, nullptr);
	renderer.UnmarkWritten(address, GuestBytes, PageManager::MarkLeave::Forget);
	renderer.CollectHotPageWrites();
	const auto last = scheduler.CurrentTick();
	scheduler.Wait(last);
	scheduler.WaitPriorityOperations(last);
	(void)buffers.DropFreedRange(address, GuestBytes, nullptr, nullptr);
	KytyShim::RemoveFaultChain();
	Memory::InstallGpuResources(nullptr);
	return failures;
}

}

extern "C" int RunKytySelfMarkingWritesTest() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load(Config::ConfigOptions {});
	KytyShimTests::TestVulkanDevice gpu;
	if (const auto* unavailable = gpu.Initialize()) {
		const bool required = std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr;
		std::fprintf(stderr, "kyty_shim_self_marking_writes: %s, no usable Vulkan device (%s)\n", required ? "FAILED" : "skipped", unavailable);
		return required ? 1 : 77;
	}
	if (!gpu.graphics.CreateAllocator()) {
		std::fputs("kyty_shim_self_marking_writes: FAILED: could not create the allocator\n", stderr);
		return 1;
	}
	void*              guest  = nullptr;
	constexpr uint64_t Mapped = GuestBytes + Block;
	if (sceKernelMapFlexibleMemory(&guest, Mapped, CpuReadWrite, 0) != 0 || guest == nullptr) {
		std::fputs("kyty_shim_self_marking_writes: FAILED: could not map guest backing\n", stderr);
		return 1;
	}
	auto* code = static_cast<uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(CodeBytes, CodeBytes));
	if (code == nullptr ||
	    ::mmap(code, CodeBytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != code) {
		std::fputs("kyty_shim_self_marking_writes: FAILED: could not map guest code\n", stderr);
		return 1;
	}
	std::memcpy(code, LongStore.data(), LongStore.size());
	std::memcpy(code + 0x100, ShortStore.data(), ShortStore.size());
	std::memcpy(code + 0x200, AddToMemory.data(), AddToMemory.size());
	if (::mprotect(code, CodeBytes, PROT_READ | PROT_EXEC) != 0) {
		std::fputs("kyty_shim_self_marking_writes: FAILED: could not protect guest code\n", stderr);
		return 1;
	}
	const auto address  = reinterpret_cast<uint64_t>(guest);
	const auto failures = CheckSelfMarkingWrites(gpu.graphics, BlockAt(address, 0), reinterpret_cast<Store>(code), reinterpret_cast<Store>(code + 0x100),
	                                             reinterpret_cast<Store>(code + 0x200));
	if (sceKernelMunmap(address, Mapped) != 0) {
		std::fputs("kyty_shim_self_marking_writes: FAILED: could not unmap guest backing\n", stderr);
		return 1;
	}
	if (failures != 0) {
		return 1;
	}
	std::puts("kyty_shim_self_marking_writes: ok");
	return 0;
}
