#include "prx/libSceAgcDriver/KytyShim/src/Faults.hpp"
#include "prx/libSceAgcDriver/KytyShim/src/MemoryShim.hpp"

#include "common/debugCounters.h"
#include "common/guardedCopy.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "kernel/memory.h"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"

#include "prx/libSceAgcDriver/KytyShim/src/FaultReplay.hpp"

#if defined(KYTY_SELF_MARKING)
#include "kyty_selfmark_rs.h"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

#if !defined(ANYPS5_KYTY)
#error "KytyShim is built only with ANYPS5_GPU_BACKEND=kyty"
#endif

namespace KytyShim {
namespace {

using GpuAccess = Libs::Graphics::PageFaultAccess;

#ifdef _WIN32
using FaultContext = CONTEXT;

void CompleteWalkerLoad(FaultContext& context, std::uint32_t length, std::uint32_t value) {
    context.Rax = value;
    context.Rip += length;
}

void RearmGuardPage(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page)) != sizeof(page) || page.State != MEM_COMMIT || (page.Protect & PAGE_GUARD) != 0) return;
    DWORD previous = 0;
    VirtualProtect(page.BaseAddress, 1, page.Protect | PAGE_GUARD, &previous);
}

std::atomic<bool> g_single_step_ready {false};
#else
using FaultContext = ucontext_t;

constexpr std::uint64_t PageFaultWrite = 1u << 1;
constexpr std::uint64_t PageFaultFetch = 1u << 4;

GpuAccess DecodeAccess(const ucontext_t& context) {
    const auto error = static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_ERR]);
    if ((error & PageFaultFetch) != 0) return GpuAccess::Execute;
    if ((error & PageFaultWrite) != 0) return GpuAccess::Write;
    return GpuAccess::Read;
}

void CompleteWalkerLoad(FaultContext& context, std::uint32_t length, std::uint32_t value) {
    context.uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(value);
    context.uc_mcontext.gregs[REG_RIP] += static_cast<greg_t>(length);
}

struct sigaction g_previous_trap {};
bool             g_trap_installed = false;
#endif

bool SwitchOn(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] != '0';
}

bool ReplayReady() {
#ifdef _WIN32
    const bool stepping = g_single_step_ready.load(std::memory_order_acquire);
#else
    const bool stepping = g_trap_installed;
#endif
    return stepping && !FaultReplay::g_broken.load(std::memory_order_relaxed) && !Libs::Graphics::GuestGpu::IsGpuThread() &&
           !Libs::Graphics::GuestGpu::IsCommandProcessorThread();
}

bool ReplayEnabled() {
    static const bool on = SwitchOn("KYTY_REPLAY_GPU_PAGE_READS");
    return on && ReplayReady();
}

bool TryReplayOnBacking(FaultContext& context, std::uint64_t address) {
    namespace Memory = Libs::LibKernel::Memory;
    if (!ReplayEnabled()) return false;
    const auto replayable = [](std::uint64_t ea, std::uint64_t size) { return Memory::ReplayableGpuRead(ea, size); };
    const auto backing    = [](std::uint64_t ea, std::uint64_t size) { return Memory::FindBackingPointer(ea, size); };
    if (!FaultReplay::Start(context, address, replayable, backing)) return false;
    Memory::NoteReplayedGpuRead();
    return true;
}

bool ReplayAfterReadbackEnabled() {
    static const bool on = SwitchOn("KYTY_REPLAY_AFTER_READBACK");
    return on && ReplayReady();
}

void FinishReplayAfterReadback(FaultContext& context, std::uint64_t address) {
    namespace Memory = Libs::LibKernel::Memory;
    std::uint64_t begin = 0;
    std::uint64_t end   = 0;
    if (!Memory::TakeReplayAfterReadback(&begin, &end)) return;
    const auto replayable = [begin, end](std::uint64_t ea, std::uint64_t size) {
        return ea + size <= end && (ea >= begin || Memory::ReplayableGpuRead(ea, begin - ea)) && !Memory::TextureGpuModified(ea, size);
    };
    const auto backing = [](std::uint64_t ea, std::uint64_t size) { return Memory::FindBackingPointer(ea, size); };
    Memory::FinishReplayAfterReadback(FaultReplay::Start(context, address, replayable, backing));
}

#ifndef _WIN32
void OnTrap(int signal, siginfo_t* info, void* native) {
#if defined(KYTY_SELF_MARKING)
    if (native != nullptr) {
        auto& context = *static_cast<ucontext_t*>(native);
        const auto resume = kyty_selfmark_breakpoint(static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_RIP]) - 1);
        if (resume != 0) {
            context.uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(resume);
            return;
        }
    }
#endif
    if (native != nullptr && FaultReplay::Finish(*static_cast<ucontext_t*>(native))) return;
    if ((g_previous_trap.sa_flags & SA_SIGINFO) != 0) {
        if (g_previous_trap.sa_sigaction != nullptr) {
            g_previous_trap.sa_sigaction(signal, info, native);
            return;
        }
    } else if (g_previous_trap.sa_handler == SIG_IGN) {
        return;
    } else if (g_previous_trap.sa_handler != SIG_DFL && g_previous_trap.sa_handler != nullptr) {
        g_previous_trap.sa_handler(signal);
        return;
    }
    ::signal(SIGTRAP, SIG_DFL);
    ::raise(SIGTRAP);
}
#endif

bool PrepareHostWrite(std::uintptr_t address, std::size_t bytes) {
    return Libs::LibKernel::Memory::TryPrepareHostWrite(address, bytes);
}

#if defined(KYTY_SELF_MARKING)
constexpr std::uint64_t MarkPage     = 4096;
constexpr std::uint64_t MarkGranule  = 64 * 1024;
constexpr std::uint64_t MarkReach    = 0x70000000;
constexpr std::uint64_t MarkStep     = 16 * 1024 * 1024;

bool GuestCodeAddress(std::uint64_t address) {
    if (GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(address), 1)) return true;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) || info.Type != MEM_IMAGE) return false;
    const auto* image    = static_cast<const unsigned char*>(info.AllocationBase);
    const auto* dos      = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt       = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const auto  rva      = address - reinterpret_cast<std::uint64_t>(image);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sections[i].Name, ".elf", 4) == 0 && rva >= sections[i].VirtualAddress &&
            rva < std::uint64_t{sections[i].VirtualAddress} + sections[i].Misc.VirtualSize) {
            return true;
        }
    }
    return false;
#else
    dl_find_object object{};
    if (_dl_find_object(reinterpret_cast<void*>(address), &object) != 0 || object.dlfo_link_map == nullptr) return false;
    const std::string_view name = object.dlfo_link_map->l_name != nullptr ? object.dlfo_link_map->l_name : "";
    return name.ends_with(".guest.prx");
#endif
}

std::uint64_t ExecutableAt(std::uint64_t address, std::uint64_t bytes, bool replace) {
#ifdef _WIN32
    (void)replace;
    return reinterpret_cast<std::uint64_t>(VirtualAlloc(reinterpret_cast<void*>(address), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* const mapped = ::mmap(reinterpret_cast<void*>(address), bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS | (replace ? MAP_FIXED : MAP_FIXED_NOREPLACE), -1, 0);
    if (mapped == MAP_FAILED) return 0;
    if (reinterpret_cast<std::uint64_t>(mapped) != address) {
        ::munmap(mapped, bytes);
        return 0;
    }
    return address;
#endif
}

std::uint64_t ExecutableNear(std::uint64_t site, std::uint64_t bytes) {
    const auto size = (bytes + MarkGranule - 1) & ~(MarkGranule - 1);
    const auto base = site & ~(MarkGranule - 1);
    if (GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(site), 1)) {
        try {
            void* const memory = GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(base, size, MarkGranule);
            if (memory == nullptr) return 0;
            const auto address = reinterpret_cast<std::uint64_t>(memory);
            if (address + size > site + MarkReach) {
                GuestArena::GuestArenaRelease_nid_postfix(memory, size);
                return 0;
            }
#ifdef _WIN32
            GuestArena::GuestArenaCommit_nid_postfix(memory, size, PAGE_EXECUTE_READWRITE, MarkGranule);
            return address;
#else
            return ExecutableAt(address, size, true);
#endif
        } catch (...) {
            return 0;
        }
    }
    for (std::uint64_t distance = MarkStep; distance < MarkReach; distance += MarkStep) {
        if (const auto above = ExecutableAt(base + distance, size, false); above != 0) return above;
        if (base > distance + MarkGranule) {
            if (const auto below = ExecutableAt(base - distance, size, false); below != 0) return below;
        }
    }
    return 0;
}

std::uint64_t ExecutableFixed(std::uint64_t address, std::uint64_t* bytes) {
    if (GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(address), 1)) {
        const auto base = address & ~(MarkGranule - 1);
        try {
            void* const memory = GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(base, MarkGranule, MarkGranule);
            if (memory == nullptr) return 0;
            if (reinterpret_cast<std::uint64_t>(memory) != base) {
                GuestArena::GuestArenaRelease_nid_postfix(memory, MarkGranule);
                return 0;
            }
#ifdef _WIN32
            GuestArena::GuestArenaCommit_nid_postfix(memory, MarkGranule, PAGE_EXECUTE_READWRITE, MarkGranule);
#else
            if (ExecutableAt(base, MarkGranule, true) == 0) {
                GuestArena::GuestArenaRelease_nid_postfix(memory, MarkGranule);
                return 0;
            }
#endif
            *bytes = MarkGranule;
            return base;
        } catch (...) {
            return 0;
        }
    }
#ifdef _WIN32
    constexpr std::uint64_t unit = MarkGranule;
#else
    constexpr std::uint64_t unit = MarkPage;
#endif
    const auto base = address & ~(unit - 1);
    if (ExecutableAt(base, unit, false) == 0) return 0;
    *bytes = unit;
    return base;
}

std::uint32_t HostWriter(std::uint64_t rip) {
#ifdef _WIN32
    (void)rip;
    return KYTY_SELFMARK_NOTE_HOST_OTHER;
#else
    dl_find_object object{};
    if (_dl_find_object(reinterpret_cast<void*>(rip), &object) != 0 || object.dlfo_link_map == nullptr) return KYTY_SELFMARK_NOTE_HOST_OTHER;
    const std::string_view name = object.dlfo_link_map->l_name != nullptr ? object.dlfo_link_map->l_name : "";
    const auto             file = name.substr(name.find_last_of('/') + 1);
    return file.starts_with("libSceAgc.") ? KYTY_SELFMARK_NOTE_HOST_AGC : KYTY_SELFMARK_NOTE_HOST_OTHER;
#endif
}

std::uint32_t WriterKind(const KytySelfMarkFault& fault, std::uint64_t rip) {
    switch (fault.outcome) {
        case KYTY_SELFMARK_PATCHED:
        case KYTY_SELFMARK_SLOW_STORE: return KYTY_SELFMARK_NOTE_PATCHED_UNARMED;
        case KYTY_SELFMARK_BUSY: return KYTY_SELFMARK_NOTE_GUEST_BUSY;
        case KYTY_SELFMARK_REFUSED:
            switch (fault.reason) {
                case KYTY_SELFMARK_REFUSAL_SHORT: return KYTY_SELFMARK_NOTE_GUEST_SHORT;
                case KYTY_SELFMARK_REFUSAL_NOT_STORE: return KYTY_SELFMARK_NOTE_GUEST_NOT_STORE;
                case KYTY_SELFMARK_REFUSAL_NO_WINDOW: return KYTY_SELFMARK_NOTE_GUEST_NO_WINDOW;
                case KYTY_SELFMARK_REFUSAL_FROZEN: return KYTY_SELFMARK_NOTE_GUEST_FROZEN;
                case KYTY_SELFMARK_REFUSAL_NOT_GUEST_CODE: return HostWriter(rip);
                default: return KYTY_SELFMARK_NOTE_GUEST_OTHER;
            }
        default: return GuestCodeAddress(rip) ? KYTY_SELFMARK_NOTE_UNTRACKED : HostWriter(rip);
    }
}

void SetInstruction(FaultContext& context, std::uint64_t address) {
#ifdef _WIN32
    context.Rip = address;
#else
    context.uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(address);
#endif
}

std::uint64_t MarkedTarget(const FaultContext& context, std::uint64_t entry) {
#ifdef _WIN32
    const std::uint64_t registers[16] = {context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rsp, context.Rbp, context.Rsi, context.Rdi,
                                         context.R8,  context.R9,  context.R10, context.R11, context.R12, context.R13, context.R14, context.R15};
#else
    const auto&         g             = context.uc_mcontext.gregs;
    const std::uint64_t registers[16] = {static_cast<std::uint64_t>(g[REG_RAX]), static_cast<std::uint64_t>(g[REG_RCX]), static_cast<std::uint64_t>(g[REG_RDX]),
                                         static_cast<std::uint64_t>(g[REG_RBX]), static_cast<std::uint64_t>(g[REG_RSP]), static_cast<std::uint64_t>(g[REG_RBP]),
                                         static_cast<std::uint64_t>(g[REG_RSI]), static_cast<std::uint64_t>(g[REG_RDI]), static_cast<std::uint64_t>(g[REG_R8]),
                                         static_cast<std::uint64_t>(g[REG_R9]),  static_cast<std::uint64_t>(g[REG_R10]), static_cast<std::uint64_t>(g[REG_R11]),
                                         static_cast<std::uint64_t>(g[REG_R12]), static_cast<std::uint64_t>(g[REG_R13]), static_cast<std::uint64_t>(g[REG_R14]),
                                         static_cast<std::uint64_t>(g[REG_R15])};
#endif
    return kyty_selfmark_target(entry, registers);
}

bool PageArmed(std::uint64_t page) {
    return static_cast<std::uint64_t>(kyty_selfmark_armed(page)) >= KYTY_SELFMARK_DELTA_LEAST;
}

bool MarkedFastPath(std::uint64_t first, std::uint64_t last) {
    const auto delta = kyty_selfmark_armed(first);
    return static_cast<std::uint64_t>(delta) >= KYTY_SELFMARK_DELTA_LEAST && (last == first || kyty_selfmark_armed(last) == delta);
}

struct MarkedWrite {
    std::uint64_t entry  = 0;
    std::uint64_t first  = 0;
    std::uint64_t last   = 0;
    bool          resume = false;
    bool          served = false;
};

MarkedWrite MarkWrite(const FaultContext& context, std::uint64_t address, std::uint64_t rip) {
    namespace Memory = Libs::LibKernel::Memory;
    MarkedWrite write;
    const auto  page  = address & ~(MarkPage - 1);
    auto        fault = kyty_selfmark_lookup(rip);
    if (fault.outcome == KYTY_SELFMARK_NONE && kyty_selfmark_enabled() && (PageArmed(page) || Memory::GuestRangeWatched(page, MarkPage))) {
#ifndef _WIN32
        GuestDeliveryHold_nid_no_patch();
#endif
        fault = kyty_selfmark_write_fault(rip);
#ifndef _WIN32
        GuestDeliveryRelease_nid_no_patch();
#endif
    }
    if (fault.outcome != KYTY_SELFMARK_PATCHED && fault.outcome != KYTY_SELFMARK_SLOW_STORE) {
        const auto kind = WriterKind(fault, rip);
        kyty_selfmark_note(kind);
        kyty_selfmark_set_writer(kind);
        if (PageArmed(page)) {
            kyty_selfmark_note(KYTY_SELFMARK_NOTE_GIVEN_BACK + kind);
            Memory::UnmarkWrittenPages(page, MarkPage, fault.outcome == KYTY_SELFMARK_REFUSED, static_cast<std::uint8_t>(kind + 1));
        }
        return write;
    }
    const auto target = MarkedTarget(context, fault.entry);
    write.entry       = fault.entry;
    write.resume      = fault.outcome == KYTY_SELFMARK_SLOW_STORE;
    write.first       = target & ~(MarkPage - 1);
    write.last        = (target + (fault.width != 0 ? fault.width - 1 : 0)) & ~(MarkPage - 1);
    const auto arm    = [](std::uint64_t at) { return kyty_selfmark_armed(at) != KYTY_SELFMARK_DELTA_REFUSED && Memory::MarkWrittenPage(at); };
    write.served      = arm(write.first) && (write.last == write.first || arm(write.last)) && MarkedFastPath(write.first, write.last);
    if (write.served) {
        kyty_selfmark_note(KYTY_SELFMARK_NOTE_SERVED);
    } else {
        const auto    state  = Memory::MarkedPageState(page);
        const auto    by     = (state >> 8u) < KYTY_SELFMARK_NOTE_WRITERS ? state >> 8u : 0u;
        std::uint32_t writer = KYTY_SELFMARK_FAULT_PATCHED_OTHER;
        kyty_selfmark_note(KYTY_SELFMARK_NOTE_PATCHED_UNARMED);
        kyty_selfmark_note(KYTY_SELFMARK_NOTE_PAGE_STATE + (state & 0xFFu));
        if ((state & 0xFFu) == KYTY_SELFMARK_PAGE_GIVEN_BACK) {
            kyty_selfmark_note(KYTY_SELFMARK_NOTE_GIVEN_BACK_BY + by);
            writer = KYTY_SELFMARK_FAULT_PATCHED_GIVEN_BACK + by;
        } else if ((state & 0xFFu) == KYTY_SELFMARK_PAGE_GPU_WRITTEN) {
            writer = KYTY_SELFMARK_FAULT_PATCHED_GPU_WRITTEN;
        } else if ((state & 0xFFu) == KYTY_SELFMARK_PAGE_MARKED) {
            writer = KYTY_SELFMARK_FAULT_PATCHED_MARKED;
        }
        kyty_selfmark_set_writer(writer);
    }
    return write;
}

void FinishMarkedWrite(FaultContext& context, const MarkedWrite& write) {
    if (write.entry == 0) return;
    if (!write.served && !MarkedFastPath(write.first, write.last) && (PageArmed(write.first) || PageArmed(write.last))) {
        kyty_selfmark_note(KYTY_SELFMARK_NOTE_GIVEN_BACK + KYTY_SELFMARK_NOTE_PATCHED_UNARMED);
        Libs::LibKernel::Memory::UnmarkWrittenPages(write.first, write.last + MarkPage - write.first, true, KYTY_SELFMARK_NOTE_PATCHED_UNARMED + 1);
    }
    if (write.resume) SetInstruction(context, write.entry);
}
#endif

bool ResolveFault(FaultContext& context, GpuAccess access, std::uint64_t address, std::uint64_t rip) {
    namespace Memory = Libs::LibKernel::Memory;
    namespace Walker = Libs::Graphics::ShaderRecompiler::IR;
    if (access == GpuAccess::Read) {
        if (const auto length = Walker::SrtWalkerLoadLength(rip); length != 0) {
            std::uint32_t value = 0;
            if (Memory::MayFaultOnGpuRead(address, sizeof(value)) && Memory::TryReadAroundGpuWrites(address, &value, sizeof(value))) {
                CompleteWalkerLoad(context, length, value);
                return true;
            }
        }
    }
    if (const auto length = Walker::SrtWalkerAheadLoadLength(rip, address); length != 0) {
        CompleteWalkerLoad(context, length, 0);
        return true;
    }
    if (access == GpuAccess::Read && TryReplayOnBacking(context, address)) {
        return true;
    }
#if defined(KYTY_SELF_MARKING)
    MarkedWrite marked;
    if (access == GpuAccess::Write) {
        marked = MarkWrite(context, address, rip);
        if (marked.served) {
            FinishMarkedWrite(context, marked);
            return true;
        }
    }
#endif
    const bool after = access == GpuAccess::Read && ReplayAfterReadbackEnabled();
    if (after) Memory::AllowReplayAfterReadback();
    if (Memory::HandleGpuFault(access, address)) {
        Common::DebugCounters::Add(Common::DebugCounters::Counter::GpuFaults);
        if (after) FinishReplayAfterReadback(context, address);
#if defined(KYTY_SELF_MARKING)
        FinishMarkedWrite(context, marked);
#endif
        return true;
    }
#if defined(KYTY_SELF_MARKING)
    if (marked.resume) SetInstruction(context, kyty_selfmark_site(marked.entry));
#endif
    if (after) FinishReplayAfterReadback(context, address);
    if (const auto length = Walker::SrtWalkerLoadLength(rip); length != 0) {
        CompleteWalkerLoad(context, length, 0);
        return true;
    }
    return false;
}

}

namespace {

void InstallSelfMarking() {
#if defined(KYTY_SELF_MARKING)
    std::uintptr_t base  = 0;
    std::size_t    bytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &bytes);
    if (bytes != 0) kyty_selfmark_init(base + bytes, &ExecutableNear, &ExecutableFixed, &GuestCodeAddress);
#endif
}

}

#ifdef _WIN32
bool HandleHostFault(void* exceptionPointers) {
    auto* exception = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    if (exception == nullptr || exception->ExceptionRecord == nullptr || exception->ContextRecord == nullptr) return false;
    const auto& record = *exception->ExceptionRecord;
#if defined(KYTY_SELF_MARKING)
    if (record.ExceptionCode == EXCEPTION_BREAKPOINT) {
        const auto resume = kyty_selfmark_breakpoint(static_cast<std::uint64_t>(exception->ContextRecord->Rip));
        if (resume == 0) return false;
        exception->ContextRecord->Rip = resume;
        return true;
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2 &&
        kyty_selfmark_table_fault(static_cast<std::uint64_t>(record.ExceptionInformation[1]))) {
        return true;
    }
#endif
    if (record.ExceptionCode == EXCEPTION_SINGLE_STEP) return FaultReplay::Finish(*exception->ContextRecord);
    const bool guardPage = record.ExceptionCode == STATUS_GUARD_PAGE_VIOLATION;
    if ((record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION && !guardPage) || record.NumberParameters < 2) return false;
    const auto access = record.ExceptionInformation[0] == 8 ? GpuAccess::Execute : record.ExceptionInformation[0] == 1 ? GpuAccess::Write : GpuAccess::Read;
    if (!guardPage && Common::ResumeGuardedCopy(exception->ContextRecord)) return true;
    const DWORD lastError = GetLastError();
    const bool handled = ResolveFault(*exception->ContextRecord, access, static_cast<std::uint64_t>(record.ExceptionInformation[1]), static_cast<std::uint64_t>(exception->ContextRecord->Rip));
    if (handled && guardPage) RearmGuardPage(static_cast<std::uintptr_t>(record.ExceptionInformation[1]));
    SetLastError(lastError);
    return handled;
}

void InstallFaultChain() {
    InstallSelfMarking();
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(&PrepareHostWrite);
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(&HandleHostFault);
    g_single_step_ready.store(true, std::memory_order_release);
}

void RemoveFaultChain() {
    g_single_step_ready.store(false, std::memory_order_release);
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(nullptr);
}
#else
bool HandleHostFault(int signal, siginfo_t* info, void* native) {
    if ((signal != SIGSEGV && signal != SIGBUS) || info == nullptr || native == nullptr) return false;
    auto& context = *static_cast<ucontext_t*>(native);
    return ResolveFault(context, DecodeAccess(context), reinterpret_cast<std::uint64_t>(info->si_addr), static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_RIP]));
}

void InstallFaultChain() {
    InstallSelfMarking();
    if (!g_trap_installed) {
        struct sigaction action {};
        action.sa_sigaction = &OnTrap;
        action.sa_flags     = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        g_trap_installed = ::sigaction(SIGTRAP, &action, &g_previous_trap) == 0;
    }
    GuestArena::GuestArenaSetHostWriteHold_nid_no_patch(&Libs::LibKernel::Memory::Shim::HoldHostWrite, &Libs::LibKernel::Memory::Shim::ReleaseHostWrite);
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(&PrepareHostWrite);
    GuestWriteWatch::GuestWriteWatchSetGpuFaultHandler_nid_no_patch(&HandleHostFault);
}

void RemoveFaultChain() {
    if (g_trap_installed) {
        ::sigaction(SIGTRAP, &g_previous_trap, nullptr);
        g_trap_installed = false;
    }
    GuestWriteWatch::GuestWriteWatchSetGpuFaultHandler_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetHostWritePreparer_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetHostWriteHold_nid_no_patch(nullptr, nullptr);
}
#endif

}
