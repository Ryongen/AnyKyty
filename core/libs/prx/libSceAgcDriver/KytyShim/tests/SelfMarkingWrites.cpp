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
constexpr std::array<uint8_t, 8> ShortStore {0x48, 0x89, 0x37, 0xC3, 0x2E, 0x2E, 0x2E, 0x2E};
constexpr std::array<uint8_t, 4> Exchange {0x48, 0x87, 0x37, 0xC3};

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

bool HoldsJump(Store store, uint32_t pads) {
	const auto* code = reinterpret_cast<const volatile uint8_t*>(store);
	for (uint32_t pad = 0; pad < pads; pad++) {
		if (code[pad] != 0x2E) return false;
	}
	return code[pads] == 0xE9;
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
	if (!HoldsJump(store, 2)) return "the write site was not replaced by a padded jump";
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

const char* CheckShortSiteIsCorrected(Fixture& fixture, Store store, Store brief, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	fixture.Clean(block);
	WriteOnThread(brief, target, 0x7001);
	if (Read(target) != 0x7001) return "the first write of a short site did not reach guest memory";
	fixture.renderer.CollectHotPageWrites();
	fixture.Clean(block);
	if (fixture.buffers.IsRegionCpuDirty(block, Block)) return "the upload after the first short write did not leave the pages clean";
	if (!HoldsJump(brief, 0)) return "the short write site was not replaced by a jump";
	if (std::memcmp(reinterpret_cast<const void*>(reinterpret_cast<uint64_t>(brief) + 3), ShortStore.data() + 3, ShortStore.size() - 3) != 0) {
		return "the bytes that follow the short write site were modified";
	}

	auto before = g_faults.load();
	WriteOnThread(brief, target, 0x7002);
	if (g_faults.load() != before + 1) return "the first write of a corrected short site did not take exactly one fault";
	if (Read(target) != 0x7002) return "the corrected short write did not reach guest memory";
	if (kyty_selfmark_armed(page) < KYTY_SELFMARK_DELTA_LEAST) return "the page was not armed by the short site";
	if (!kyty_selfmark_any(page, page + Page)) return "the corrected short write left no card";
	if (fixture.buffers.IsRegionCpuDirty(page, Page)) return "the short write was reported before the synchronisation point";
	fixture.renderer.CollectHotPageWrites();
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "the synchronisation point did not report the short write";
	if (!fixture.GpuHolds(target, 0x7002)) return "the GPU buffer does not hold the corrected short write";

	for (uint64_t round = 0; round < 8; round++) {
		const auto value = 0x7100 + round;
		before           = g_faults.load();
		WriteOnThread(brief, target, value);
		WriteOnThread(store, target + 8 - Displacement, value + 0x100);
		if (g_faults.load() != before) return "a short and a long corrected site took a fault on the page they share";
		if (Read(target) != value || Read(target + 8) != value + 0x100) return "the writes of the two sites did not reach guest memory";
		if (kyty_selfmark_armed(page) < KYTY_SELFMARK_DELTA_LEAST) return "the page shared by a short and a long site was given back to faults";
		fixture.renderer.CollectHotPageWrites(page, Page);
		if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "a collection of the shared page did not report the writes";
		if (!fixture.GpuHolds(target, value) || !fixture.GpuHolds(target + 8, value + 0x100)) return "the GPU buffer does not hold the writes of the two sites";
	}
	return nullptr;
}

const char* CheckUnpatchedSiteKeepsTheFault(Fixture& fixture, Store store, Store unpatched, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	auto       before = g_faults.load();
	WriteOnThread(unpatched, target + 8, 0x4444);
	if (g_faults.load() != before + 1) return "a write site that cannot be corrected did not fault once";
	if (*reinterpret_cast<const volatile uint8_t*>(unpatched) != Exchange[0]) return "a write site that cannot be corrected was modified";
	if (Read(target + 8) != 0x4444) return "the write of an uncorrected site did not reach guest memory";
	if (kyty_selfmark_armed(page) != KYTY_SELFMARK_DELTA_REFUSED) return "the page was not given back to faults after a write that cannot mark it";
	if (kyty_selfmark_armed(page + 2 * Page) != KYTY_SELFMARK_DELTA_REFUSED) return "the block of a page given back to faults was left marked";
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "the fault of an uncorrected site did not report the page at once";

	before = g_faults.load();
	WriteOnThread(store, target - Displacement, 0x5555);
	WriteOnThread(unpatched, target + 8, 0x5556);
	if (g_faults.load() != before) return "a write to an open page took a fault";
	if (Read(target) != 0x5555 || Read(target + 8) != 0x5556) return "writes to an open page did not reach guest memory";
	if (kyty_selfmark_any(page, page + Page)) return "a write to an open page left a card";
	if (!fixture.GpuHolds(target, 0x5555) || !fixture.GpuHolds(target + 8, 0x5556)) return "the GPU buffer does not hold the writes to an open page";

	before = g_faults.load();
	WriteOnThread(store, target - Displacement, 0x6666);
	if (g_faults.load() != before + 1) return "a corrected site did not fault once on a page given back to faults";
	if (Read(target) != 0x6666) return "the write to a page given back to faults did not reach guest memory";
	if (kyty_selfmark_armed(page) != KYTY_SELFMARK_DELTA_REFUSED) return "a page given back to faults was marked again";
	if (!fixture.buffers.IsRegionCpuDirty(page, Page)) return "the fault of a corrected site on a page given back to faults did not report it at once";
	if (kyty_selfmark_any(page, page + Page)) return "a write to a page given back to faults left a card";
	if (!fixture.GpuHolds(target, 0x6666)) return "the GPU buffer does not hold the write to a page given back to faults";
	return nullptr;
}

const char* CheckSwitchedOffShortSites(Fixture& fixture, Store store, Store brief, uint64_t block) {
	const auto page   = block + Page;
	const auto target = page + Slot;
	fixture.renderer.CollectHotPageWrites();
	fixture.Clean(block);
	kyty_selfmark_set_short(false);
	fixture.renderer.ForgetAllWritten();
	if (std::memcmp(reinterpret_cast<const void*>(brief), ShortStore.data(), ShortStore.size()) != 0) return "the short site did not get its bytes back";
	if (kyty_selfmark_armed(page) != 0) return "a page stayed marked after the switch was turned";
	WriteOnThread(store, target + 8 - Displacement, 0x8001);
	fixture.renderer.CollectHotPageWrites();
	fixture.Clean(block);
	if (kyty_selfmark_armed(page) < KYTY_SELFMARK_DELTA_LEAST) return "the long site did not mark the page again";
	auto before = g_faults.load();
	WriteOnThread(brief, target, 0x8002);
	if (g_faults.load() != before + 1) return "a short site did not fault while short sites are switched off";
	if (Read(target) != 0x8002) return "the write of a switched off short site did not reach guest memory";
	if (kyty_selfmark_armed(page) != KYTY_SELFMARK_DELTA_REFUSED) return "the block was not given back to faults by a switched off short site";
	if ((fixture.renderer.MarkState(page) & 0xFFu) != KYTY_SELFMARK_PAGE_GIVEN_BACK) return "the page of a block given back is not reported as such";
	if ((fixture.renderer.MarkState(page) >> 8u) != KYTY_SELFMARK_NOTE_GUEST_SHORT + 1) return "the writer that gave the block back was not recorded";
	if (!fixture.GpuHolds(target, 0x8002)) return "the GPU buffer does not hold the write of a switched off short site";

	kyty_selfmark_set_short(true);
	fixture.renderer.ForgetAllWritten();
	if (!HoldsJump(brief, 0)) return "the short site did not get its jump back";
	if (kyty_selfmark_armed(page) != 0) return "the refusal of the block survived the switch";
	fixture.renderer.CollectHotPageWrites();
	fixture.Clean(block);
	before = g_faults.load();
	WriteOnThread(brief, target, 0x8003);
	if (g_faults.load() != before + 1) return "a short site switched back on did not take one fault to mark its page";
	if (kyty_selfmark_armed(page) < KYTY_SELFMARK_DELTA_LEAST) return "a short site switched back on did not mark its page";
	before = g_faults.load();
	WriteOnThread(brief, target, 0x8004);
	if (g_faults.load() != before) return "a short site switched back on took a second fault";
	fixture.renderer.CollectHotPageWrites();
	if (!fixture.GpuHolds(target, 0x8004)) return "the GPU buffer does not hold the write of a short site switched back on";
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

int CheckSelfMarkingWrites(GraphicContext& graphics, uint64_t address, Store store, Store brief, Store unpatched) {
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
	if (failures == 0) failures += Report(CheckUnpatchedSiteKeepsTheFault(fixture, store, unpatched, first));
	if (failures == 0) failures += Report(CheckShortSiteIsCorrected(fixture, store, brief, BlockAt(address, 2)));
	if (failures == 0) failures += Report(CheckSwitchedOffShortSites(fixture, store, brief, BlockAt(address, 3)));
	if (failures == 0) failures += Report(CheckWritersAndCollector(fixture, store, BlockAt(address, 1)));

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
	std::memcpy(code + 0x200, Exchange.data(), Exchange.size());
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
