#ifndef KYTY_COMMON_TIMER_H_
#define KYTY_COMMON_TIMER_H_

#include <chrono>
#include <cstdint>

namespace Replay {
uint64_t Now();
}

namespace Common {

inline uint64_t SteadyNs() {
	return Replay::Now();
}

class Timer final {
public:
	[[nodiscard]] double GetTimeS() const {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
	}

private:
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

}

#endif
