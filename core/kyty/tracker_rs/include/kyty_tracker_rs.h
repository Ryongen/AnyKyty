#ifndef KYTY_TRACKER_RS_H_
#define KYTY_TRACKER_RS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KytyTrPages   KytyTrPages;
typedef struct KytyTrTracker KytyTrTracker;

typedef void (*KytyTrRangeFn)(void* context, uint64_t address, uint64_t size);
typedef void (*KytyTrIndexedRangeFn)(void* context, size_t index, uint64_t address, uint64_t size);
typedef void (*KytyTrVoidFn)(void* context);
typedef void (*KytyTrEmitFn)(void* context, const uint64_t* words, size_t count);

typedef struct KytyTrHost {
	bool (*protect)(uint64_t vaddr, uint64_t size, uint32_t mode);
	void (*bda_mark)(uint64_t region);
	void (*defer_protect)(const void* region, uint64_t vaddr, uint64_t size);
	bool (*has_reserved_core)(void);
	uint64_t (*steady_ns)(void);
	void (*fatal)(uint32_t code, uint64_t a, uint64_t b);
	bool (*hot_watch)(uint64_t begin, uint64_t end);
	void (*hot_unwatch)(uint64_t begin, uint64_t end);
	bool (*hot_collect)(uint64_t begin, uint64_t end, KytyTrRangeFn written, void* context, bool strict);
	void (*hot_log)(const uint64_t* values, size_t count);
} KytyTrHost;

typedef struct KytyTrWriteBlockProbe {
	uint32_t opened;
	bool     gpu;
	bool     tracked;
} KytyTrWriteBlockProbe;

enum {
	KYTY_TR_FATAL_TRACKER_INVALID_RANGE   = 1,
	KYTY_TR_FATAL_TRACKER_INVALID_REGION  = 2,
	KYTY_TR_FATAL_CPU_CONFLICTS_GPU       = 3,
	KYTY_TR_FATAL_LOCK_REENTERED          = 4,
	KYTY_TR_FATAL_UNLOCK_BY_OTHER_THREAD  = 5,
	KYTY_TR_FATAL_PAGES_INVALID_RANGE     = 6,
	KYTY_TR_FATAL_READ_WATCHER_OVERFLOW   = 7,
	KYTY_TR_FATAL_READ_WATCHER_UNDERFLOW  = 8,
	KYTY_TR_FATAL_WRITE_WATCHER_OVERFLOW  = 9,
	KYTY_TR_FATAL_WRITE_WATCHER_UNDERFLOW = 10,
	KYTY_TR_FATAL_UNTRACK_UNKNOWN_PAGE    = 11,
	KYTY_TR_FATAL_UNTRACK_UNKNOWN_REGION  = 12,
	KYTY_TR_FATAL_PROTECT_FAILED          = 13,
	KYTY_TR_FATAL_DESTROYED_WITH_LIVE     = 14,
	KYTY_TR_FATAL_RESERVATION_FAILED      = 15
};

void kyty_tr_set_host(const KytyTrHost* host);

KytyTrPages*    kyty_tr_pages_create(bool hot_enabled);
void            kyty_tr_pages_destroy(KytyTrPages* pages);
const uint64_t* kyty_tr_pages_read_granules(const KytyTrPages* pages);
bool            kyty_tr_pages_has_read_watchers(const KytyTrPages* pages, uint64_t vaddr, uint64_t size);
bool            kyty_tr_pages_is_watched(const KytyTrPages* pages, uint64_t vaddr);
bool            kyty_tr_pages_watched_span(const KytyTrPages* pages, uint64_t vaddr, uint64_t size, uint64_t* first, uint64_t* end);
bool            kyty_tr_pages_allow_execute(const KytyTrPages* pages, uint64_t vaddr);
void            kyty_tr_pages_update_watchers(const KytyTrPages* pages, uint64_t vaddr, uint64_t size, bool track, bool is_read);
void            kyty_tr_pages_reapply_protection(const KytyTrPages* pages, uint64_t vaddr, uint64_t size);
bool            kyty_tr_pages_has_hot_pages(const KytyTrPages* pages);
void            kyty_tr_pages_note_write_fault(const KytyTrPages* pages, uint64_t vaddr);
void kyty_tr_pages_collect_hot_page_writes(const KytyTrPages* pages, KytyTrRangeFn written, void* context, uint64_t vaddr, uint64_t size);
void kyty_tr_pages_forget_hot_pages(const KytyTrPages* pages, uint64_t vaddr, uint64_t size);

KytyTrTracker* kyty_tr_tracker_create(const KytyTrPages* pages);
void           kyty_tr_tracker_destroy(KytyTrTracker* tracker);
bool           kyty_tr_is_region_cpu_modified(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
bool           kyty_tr_is_region_gpu_modified(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
bool           kyty_tr_may_be_gpu_modified(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
void           kyty_tr_mark_region_as_cpu_modified(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
void           kyty_tr_unmark_region_as_gpu_modified(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
void kyty_tr_invalidate_region(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size, KytyTrVoidFn on_flush, void* context);
void kyty_tr_for_each_download_range(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size, bool clear, KytyTrRangeFn func,
                                     void* context);
uint32_t              kyty_tr_region_kept_sync(const KytyTrTracker* tracker, uint64_t index);
void                  kyty_tr_note_guest_write_fault(const KytyTrTracker* tracker, uint64_t vaddr);
KytyTrWriteBlockProbe kyty_tr_open_write_block(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t block_size, bool open_block);
bool                  kyty_tr_take_kept_pages(const KytyTrTracker* tracker);
uint64_t              kyty_tr_region_cpu_dirty_epoch(const KytyTrTracker* tracker, uint64_t index);
uint64_t              kyty_tr_region_gpu_clean_epoch(const KytyTrTracker* tracker, uint64_t index);
uint64_t              kyty_tr_cpu_modification_epoch(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
bool                  kyty_tr_deferred_protection_pending(const KytyTrTracker* tracker, uint64_t vaddr);
void                  kyty_tr_note_host_write(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size);
void kyty_tr_for_each_cpu_dirty_span(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size, uint64_t max_gap, KytyTrRangeFn func,
                                     void* context);
bool kyty_tr_copy_region_cpu_dirty(const KytyTrTracker* tracker, uint64_t index, uint64_t* words);
void kyty_tr_consume_cpu_writes(const KytyTrTracker* tracker, uint32_t consumer, uint64_t vaddr, uint64_t size, KytyTrRangeFn func,
                                void* context);
void kyty_tr_mark_region_as_gpu_modified_unless_cpu_dirty(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size,
                                                          KytyTrRangeFn func, void* context);
void kyty_tr_for_each_upload_range(const KytyTrTracker* tracker, uint64_t vaddr, uint64_t size, bool is_written, uint32_t device_sync,
                                   bool defer, KytyTrRangeFn range_func, KytyTrVoidFn upload_func, void* context);
void kyty_tr_for_each_upload_ranges(const KytyTrTracker* tracker, const uint64_t* ranges, size_t count, uint32_t device_sync, bool defer,
                                    KytyTrIndexedRangeFn range_func, KytyTrVoidFn upload_func, void* context);
void     kyty_tr_apply_deferred_protection(const void* region, uint64_t vaddr, uint64_t size);
uint64_t kyty_tr_region_index(const void* region);
uint64_t kyty_tr_cpu_dirty_epoch(void);
uint64_t kyty_tr_gpu_clean_epoch(void);
uint64_t kyty_tr_take_opened_upload_bytes(void);
void     kyty_tr_take_contention(uint32_t lock_class, uint64_t* waits, uint64_t* ns);
void     kyty_tr_debug_state(const KytyTrTracker* tracker, const KytyTrPages* pages, KytyTrEmitFn emit, void* context);

#ifdef __cplusplus
}
#endif

#endif
