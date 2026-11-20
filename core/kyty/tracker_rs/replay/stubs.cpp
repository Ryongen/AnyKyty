#include "replay.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hotWriteWatch.h"
#include "graphics/host_gpu/trackerTrace.h"
#include "kernel/memory.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace Log {

void Flush() {}

void WriteToConsoleAndLog(std::string_view) {}

void WriteToSessionLog(std::string_view text) {
	if (text.starts_with("PageManager fatal: ")) {
		Replay::OnFatal(text);
	}
}

}

namespace Common {

void DbgExitHandler(char const*, int, std::string_view text) {
	Replay::OnFatal(text);
}

void DbgExitIfHandler(char const* expr, char const*, int) {
	Replay::OnFatal(std::string("exit if ") + expr);
}

void DbgNotImplementedHandler(char const* expr, char const*, int) {
	Replay::OnFatal(std::string("not implemented ") + expr);
}

namespace Thread {

bool HasReservedCore() {
	return false;
}

}

}

namespace Libs::LibKernel::Memory {

bool ProtectGuestHostMemory(uint64_t, uint64_t, Common::VirtualMemory::Mode) {
	return Replay::ProtectAnswer();
}

}

namespace Libs::Graphics {

HotWriteWatch::HotWriteWatch() noexcept {
	if (Replay::HotAvailable()) {
		m_uffd    = 1;
		m_pagemap = 1;
	}
}

HotWriteWatch::~HotWriteWatch() = default;

HotWriteWatch& HotWriteWatch::Get() noexcept {
	static HotWriteWatch watch;
	return watch;
}

bool HotWriteWatch::Watch(uint64_t begin, uint64_t end) const noexcept {
	return Replay::HotWatch(begin, end);
}

void HotWriteWatch::Unwatch(uint64_t begin, uint64_t end) const noexcept {
	Replay::HotUnwatch(begin, end);
}

bool HotWriteWatch::Collect(uint64_t begin, uint64_t end, Written written, void* context, bool strict) const noexcept {
	return Replay::HotCollect(begin, end, written, context, strict);
}

namespace TrackerTrace {

void SetRoleQuery(RoleQuery) noexcept {}

uint64_t CurrentSeq() noexcept {
	return 0;
}

void Event(Ev kind, const uint64_t* words, size_t count) noexcept {
	Replay::OnEvent(static_cast<uint8_t>(kind), words, count);
}

void Call::Begin(Op, const uint64_t*, size_t) noexcept {}

void Call::End() noexcept {}

void Call::CloseWindow(Callback, uint64_t) noexcept {}

void FaultScope::Enter() noexcept {}

void FaultScope::Leave() noexcept {}

}

}
