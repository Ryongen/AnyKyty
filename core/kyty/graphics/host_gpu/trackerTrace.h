#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_TRACKERTRACE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_TRACKERTRACE_H_

#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::TrackerTrace {

enum class Op : uint8_t {
	Standalone                           = 0,
	PageManagerCreate                    = 1,
	PageManagerDestroy                   = 2,
	HasReadWatchers                      = 3,
	IsWatched                            = 4,
	WatchedSpan                          = 5,
	AllowExecute                         = 6,
	UpdatePageWatchers                   = 7,
	HasHotPages                          = 8,
	NoteWriteFault                       = 9,
	CollectHotPageWrites                 = 10,
	ForgetHotPages                       = 11,
	TrackerCreate                        = 32,
	TrackerDestroy                       = 33,
	IsRegionCpuModified                  = 34,
	IsRegionGpuModified                  = 35,
	MayBeGpuModified                     = 36,
	MarkRegionAsCpuModified              = 37,
	UnmarkRegionAsGpuModified            = 38,
	InvalidateRegion                     = 39,
	ForEachDownloadRange                 = 40,
	RegionKeptSync                       = 41,
	NoteGuestWriteFault                  = 42,
	OpenWriteBlock                       = 43,
	TakeKeptPages                        = 44,
	RegionCpuDirtyEpoch                  = 45,
	CpuModificationEpoch                 = 46,
	DeferredProtectionPending            = 47,
	NoteHostWrite                        = 48,
	RegionGpuCleanEpoch                  = 49,
	ForEachCpuDirtySpan                  = 50,
	CopyRegionCpuDirty                   = 51,
	ConsumeCpuWrites                     = 52,
	MarkRegionAsGpuModifiedUnlessCpuDirty = 53,
	ForEachUploadRange                   = 54,
	ForEachUploadRanges                  = 55,
	ApplyDeferredProtection              = 56,
	CpuDirtyEpoch                        = 57,
	GpuCleanEpoch                        = 58,
	TakeOpenedUploadBytes                = 59,
	Thread                               = 250,
};

enum class Ev : uint8_t {
	Protect       = 1,
	BdaMark       = 2,
	DeferProtect  = 3,
	Range         = 4,
	IndexedRange  = 5,
	Flush         = 6,
	Window        = 7,
	DeferredTaken = 8,
	ArgRange      = 9,
	Time          = 10,
	HotLog        = 11,
	Bits          = 12,
	Upload        = 13,
	ProtectResult = 14,
};

enum class Callback : uint8_t { Range = 0, Upload = 1, Flush = 2, Hot = 3 };

inline constexpr uint64_t FileMagic       = 0x314352545954594Bull;
inline constexpr uint32_t FileVersion     = 1;
inline constexpr uint64_t RecordMagic     = 0xA7;
inline constexpr size_t   MaxArgs         = 8;
inline constexpr size_t   MaxResults      = 15;
inline constexpr uint64_t RecordFixedWords = 6;

inline bool g_enabled = false;

[[nodiscard]] inline bool Enabled() noexcept {
	return g_enabled;
}

using RoleQuery = uint32_t (*)();
void SetRoleQuery(RoleQuery query) noexcept;
[[nodiscard]] uint64_t CurrentSeq() noexcept;
void Event(Ev kind, const uint64_t* words, size_t count) noexcept;

inline void Event(Ev kind, uint64_t a) noexcept {
	if (Enabled()) [[unlikely]] {
		const uint64_t words[] = {a};
		Event(kind, words, 1);
	}
}

inline void Event(Ev kind, uint64_t a, uint64_t b) noexcept {
	if (Enabled()) [[unlikely]] {
		const uint64_t words[] = {a, b};
		Event(kind, words, 2);
	}
}

inline void Event(Ev kind, uint64_t a, uint64_t b, uint64_t c) noexcept {
	if (Enabled()) [[unlikely]] {
		const uint64_t words[] = {a, b, c};
		Event(kind, words, 3);
	}
}

class Call final {
public:
	template <typename... Args>
	explicit Call(Op op, Args... args) noexcept {
		static_assert(sizeof...(Args) <= MaxArgs);
		if (Enabled()) [[unlikely]] {
			const uint64_t values[sizeof...(Args) + 1] = {static_cast<uint64_t>(args)...};
			Begin(op, values, sizeof...(Args));
		}
	}
	~Call() {
		if (m_active) [[unlikely]] {
			End();
		}
	}
	Call(const Call&)            = delete;
	Call& operator=(const Call&) = delete;

	[[nodiscard]] bool Active() const noexcept { return m_active; }

	template <typename... Results>
	void Result(Results... results) noexcept {
		static_assert(sizeof...(Results) <= MaxResults);
		if (m_active) [[unlikely]] {
			const uint64_t values[sizeof...(Results) + 1] = {static_cast<uint64_t>(results)...};
			for (size_t index = 0; index < sizeof...(Results); index++) {
				m_results[index] = values[index];
			}
			m_result_count = static_cast<uint8_t>(sizeof...(Results));
		}
	}

	void Add(Ev kind, uint64_t a, uint64_t b) noexcept {
		if (m_active) [[unlikely]] {
			const uint64_t words[] = {a, b};
			Event(kind, words, 2);
		}
	}

	void Add(Ev kind, uint64_t a, uint64_t b, uint64_t c) noexcept {
		if (m_active) [[unlikely]] {
			const uint64_t words[] = {a, b, c};
			Event(kind, words, 3);
		}
	}

	class Window final {
	public:
		Window(Call& call, Callback kind) noexcept: m_call(call.m_active ? &call : nullptr), m_kind(kind) {
			if (m_call != nullptr) [[unlikely]] {
				m_begin = CurrentSeq();
			}
		}
		~Window() {
			if (m_call != nullptr) [[unlikely]] {
				m_call->CloseWindow(m_kind, m_begin);
			}
		}
		Window(const Window&)            = delete;
		Window& operator=(const Window&) = delete;

	private:
		Call*    m_call  = nullptr;
		Callback m_kind  = Callback::Range;
		uint64_t m_begin = 0;
	};

private:
	void Begin(Op op, const uint64_t* args, size_t count) noexcept;
	void End() noexcept;
	void CloseWindow(Callback kind, uint64_t begin) noexcept;

	bool     m_active       = false;
	uint8_t  m_op           = 0;
	uint8_t  m_arg_count    = 0;
	uint8_t  m_result_count = 0;
	uint32_t m_flags;
	uint32_t m_ordinals[4];
	uint64_t m_args[MaxArgs];
	uint64_t m_results[MaxResults];
	uint64_t m_seq;
	uint64_t m_parent;
	uint64_t m_started;
	size_t   m_event_start;
	Call*    m_previous;
};

class FaultScope final {
public:
	FaultScope() noexcept {
		if (Enabled()) [[unlikely]] {
			Enter();
			m_active = true;
		}
	}
	~FaultScope() {
		if (m_active) [[unlikely]] {
			Leave();
		}
	}
	FaultScope(const FaultScope&)            = delete;
	FaultScope& operator=(const FaultScope&) = delete;

private:
	static void Enter() noexcept;
	static void Leave() noexcept;
	bool        m_active = false;
};

}

#endif
