#ifndef KYTY_TRACKER_REPLAY_REPLAY_H_
#define KYTY_TRACKER_REPLAY_REPLAY_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Replay {

uint64_t Now();
bool     ProtectAnswer();
void     OnEvent(uint8_t kind, const uint64_t* words, size_t count);
[[noreturn]] void OnFatal(std::string_view text);
bool     HotAvailable();
bool     HotWatch(uint64_t begin, uint64_t end);
void     HotUnwatch(uint64_t begin, uint64_t end);
bool     HotCollect(uint64_t begin, uint64_t end, void (*written)(void*, uint64_t, uint64_t), void* context, bool strict);

}

#endif
