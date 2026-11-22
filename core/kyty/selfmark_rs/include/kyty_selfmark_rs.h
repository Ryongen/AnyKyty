#ifndef KYTY_SELFMARK_RS_H_
#define KYTY_SELFMARK_RS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	KYTY_SELFMARK_NONE       = 0,
	KYTY_SELFMARK_PATCHED    = 1,
	KYTY_SELFMARK_SLOW_STORE = 2,
	KYTY_SELFMARK_REFUSED    = 3,
	KYTY_SELFMARK_BUSY       = 4,
	KYTY_SELFMARK_INSIDE     = 5
};

enum {
	KYTY_SELFMARK_DELTA_REFUSED  = 1,
	KYTY_SELFMARK_DELTA_REFUSALS = 32,
	KYTY_SELFMARK_DELTA_LEAST    = 0x1000
};

enum {
	KYTY_SELFMARK_REFUSAL_NOT_STORE       = 2,
	KYTY_SELFMARK_REFUSAL_RIP_RELATIVE    = 3,
	KYTY_SELFMARK_REFUSAL_STACK_BASE      = 4,
	KYTY_SELFMARK_REFUSAL_NO_BASE         = 5,
	KYTY_SELFMARK_REFUSAL_PREFIX          = 6,
	KYTY_SELFMARK_REFUSAL_ENCODING        = 7,
	KYTY_SELFMARK_REFUSAL_REGISTER_FORM   = 8,
	KYTY_SELFMARK_REFUSAL_STACK_OPERAND   = 9,
	KYTY_SELFMARK_REFUSAL_NO_SCRATCH      = 10,
	KYTY_SELFMARK_REFUSAL_NOT_GUEST_CODE  = 11,
	KYTY_SELFMARK_REFUSAL_OUT_OF_REACH    = 12,
	KYTY_SELFMARK_REFUSAL_CODE_PROTECTION = 13,
	KYTY_SELFMARK_REFUSAL_TABLE_FULL      = 14,
	KYTY_SELFMARK_REFUSAL_NO_CORE_SYNC    = 15,
	KYTY_SELFMARK_REFUSAL_NO_WINDOW       = 16,
	KYTY_SELFMARK_REFUSAL_FROZEN          = 17,
	KYTY_SELFMARK_REFUSALS                = 18
};

enum {
	KYTY_SELFMARK_NOTE_SERVED           = 0,
	KYTY_SELFMARK_NOTE_PATCHED_UNARMED  = 1,
	KYTY_SELFMARK_NOTE_GUEST_NOT_STORE  = 3,
	KYTY_SELFMARK_NOTE_GUEST_NO_WINDOW  = 4,
	KYTY_SELFMARK_NOTE_GUEST_FROZEN     = 5,
	KYTY_SELFMARK_NOTE_GUEST_OTHER      = 6,
	KYTY_SELFMARK_NOTE_GUEST_BUSY       = 7,
	KYTY_SELFMARK_NOTE_HOST_AGC         = 8,
	KYTY_SELFMARK_NOTE_HOST_OTHER       = 9,
	KYTY_SELFMARK_NOTE_UNTRACKED        = 10,
	KYTY_SELFMARK_NOTE_HOST_PREPARED    = 11,
	KYTY_SELFMARK_NOTE_WRITERS          = 16,
	KYTY_SELFMARK_NOTE_GIVEN_BACK       = 16,
	KYTY_SELFMARK_NOTE_NOT_GIVEN_BACK   = 32,
	KYTY_SELFMARK_NOTE_GIVEN_BACK_BY    = 48,
	KYTY_SELFMARK_NOTE_OWNERS           = 64,
	KYTY_SELFMARK_OWNER_KINDS           = 32,
	KYTY_SELFMARK_OWNERS                = 6,
	KYTY_SELFMARK_NOTES                 = 256
};

enum {
	KYTY_SELFMARK_FAULT_PATCHED_OTHER      = 14,
	KYTY_SELFMARK_FAULT_PATCHED_GIVEN_BACK = 16
};

typedef struct KytySelfMarkFault {
	uint32_t outcome;
	uint32_t reason;
	uint64_t entry;
	uint32_t width;
} KytySelfMarkFault;

typedef struct KytySelfMarkStats {
	uint64_t patched;
	uint64_t busy;
	uint64_t slow_faults;
	uint64_t breaks;
	uint64_t punned;
	uint64_t ahead;
	uint64_t hop_regions;
	uint64_t refusals[KYTY_SELFMARK_REFUSALS];
	uint64_t notes[KYTY_SELFMARK_NOTES];
} KytySelfMarkStats;

typedef uint64_t (*KytySelfMarkNearAllocator)(uint64_t site, uint64_t bytes);
typedef uint64_t (*KytySelfMarkFixedAllocator)(uint64_t address, uint64_t* bytes);
typedef bool (*KytySelfMarkGuestCode)(uint64_t address);

bool              kyty_selfmark_init(uint64_t guest_limit, KytySelfMarkNearAllocator allocator, KytySelfMarkFixedAllocator fixed,
                                     KytySelfMarkGuestCode guest);
bool              kyty_selfmark_enabled(void);
KytySelfMarkFault kyty_selfmark_write_fault(uint64_t address);
KytySelfMarkFault kyty_selfmark_lookup(uint64_t address);
uint64_t          kyty_selfmark_target(uint64_t entry, const uint64_t* registers);
uint64_t          kyty_selfmark_site(uint64_t entry);
void              kyty_selfmark_service(void);
void              kyty_selfmark_note(uint32_t kind);
void              kyty_selfmark_set_writer(uint32_t kind);
void              kyty_selfmark_note_owner(uint32_t owner);
uint32_t          kyty_selfmark_length(uint64_t entry);
uint64_t          kyty_selfmark_breakpoint(uint64_t address);
bool              kyty_selfmark_table_fault(uint64_t address);
bool              kyty_selfmark_arm(uint64_t address, int64_t delta);
int64_t           kyty_selfmark_armed(uint64_t address);
bool              kyty_selfmark_mark(uint64_t address);
size_t            kyty_selfmark_take(uint64_t begin, uint64_t end, uint64_t* runs, size_t capacity, uint64_t* next);
bool              kyty_selfmark_any(uint64_t begin, uint64_t end);
void              kyty_selfmark_stats(KytySelfMarkStats* stats);

#ifdef __cplusplus
}
#endif

#endif
