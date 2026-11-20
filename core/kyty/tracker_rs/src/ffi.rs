use crate::bits::{Mask, WORDS};
use crate::host::{self, Host, IndexedRangeFn, RangeFn, VoidFn};
use crate::pages::{Pages, Watch};
use crate::region::{Region, WriteBlockProbe, CPU_DIRTY_EPOCH, GPU_CLEAN_EPOCH, OPENED_UPLOAD_BYTES};
use crate::spin;
use crate::tracker::Tracker;
use crate::digest;
use core::ffi::c_void;
use core::sync::atomic::Ordering;

pub struct TrackerHandle {
    tracker: Tracker,
}

#[no_mangle]
pub extern "C" fn kyty_tr_set_host(host: *const Host) {
    host::set(unsafe { &*host });
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_create(hot_enabled: bool) -> *mut Pages {
    Box::into_raw(Box::new(Pages::new(hot_enabled)))
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_destroy(pages: *mut Pages) {
    let pages = unsafe { Box::from_raw(pages) };
    pages.check_destroy();
    drop(pages);
}

#[inline]
fn pages<'a>(pages: *const Pages) -> &'a Pages {
    unsafe { &*pages }
}

#[inline]
fn tracker<'a>(tracker: *const TrackerHandle) -> &'a Tracker {
    unsafe { &(*tracker).tracker }
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_read_granules(handle: *const Pages) -> *const u64 {
    pages(handle).read_granules() as *const u64
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_has_read_watchers(handle: *const Pages, vaddr: u64, size: u64) -> bool {
    pages(handle).has_read_watchers(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_is_watched(handle: *const Pages, vaddr: u64) -> bool {
    pages(handle).is_watched(vaddr)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_watched_span(handle: *const Pages, vaddr: u64, size: u64, first: *mut u64, end: *mut u64) -> bool {
    pages(handle).watched_span(vaddr, size, unsafe { &mut *first }, unsafe { &mut *end })
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_allow_execute(handle: *const Pages, vaddr: u64) -> bool {
    pages(handle).allow_execute(vaddr)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_update_watchers(handle: *const Pages, vaddr: u64, size: u64, track: bool, is_read: bool) {
    pages(handle).update_page_watchers(vaddr, size, track, if is_read { Watch::Access } else { Watch::Write }, None)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_reapply_protection(handle: *const Pages, vaddr: u64, size: u64) {
    pages(handle).reapply_protection(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_has_hot_pages(handle: *const Pages) -> bool {
    pages(handle).has_hot_pages()
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_note_write_fault(handle: *const Pages, vaddr: u64) {
    pages(handle).note_write_fault(vaddr)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_collect_hot_page_writes(handle: *const Pages, written: RangeFn, context: *mut c_void, vaddr: u64, size: u64) {
    pages(handle).collect_hot_page_writes(&mut |begin, end| written(context, begin, end), vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_pages_forget_hot_pages(handle: *const Pages, vaddr: u64, size: u64) {
    pages(handle).forget_hot_pages(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_tracker_create(handle: *const Pages) -> *mut TrackerHandle {
    Box::into_raw(Box::new(TrackerHandle { tracker: Tracker::new(pages(handle)) }))
}

#[no_mangle]
pub extern "C" fn kyty_tr_tracker_destroy(handle: *mut TrackerHandle) {
    drop(unsafe { Box::from_raw(handle) });
}

#[no_mangle]
pub extern "C" fn kyty_tr_is_region_cpu_modified(handle: *const TrackerHandle, vaddr: u64, size: u64) -> bool {
    tracker(handle).is_region_cpu_modified(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_is_region_gpu_modified(handle: *const TrackerHandle, vaddr: u64, size: u64) -> bool {
    tracker(handle).is_region_gpu_modified(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_may_be_gpu_modified(handle: *const TrackerHandle, vaddr: u64, size: u64) -> bool {
    tracker(handle).may_be_gpu_modified(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_mark_region_as_cpu_modified(handle: *const TrackerHandle, vaddr: u64, size: u64) {
    tracker(handle).mark_region_as_cpu_modified(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_unmark_region_as_gpu_modified(handle: *const TrackerHandle, vaddr: u64, size: u64) {
    tracker(handle).unmark_region_as_gpu_modified(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_invalidate_region(handle: *const TrackerHandle, vaddr: u64, size: u64, on_flush: VoidFn, context: *mut c_void) {
    tracker(handle).invalidate_region(vaddr, size, &mut || on_flush(context))
}

#[no_mangle]
pub extern "C" fn kyty_tr_for_each_download_range(handle: *const TrackerHandle, vaddr: u64, size: u64, clear: bool, func: RangeFn, context: *mut c_void) {
    tracker(handle).for_each_download_range(vaddr, size, clear, &mut |address, bytes| func(context, address, bytes))
}

#[no_mangle]
pub extern "C" fn kyty_tr_region_kept_sync(handle: *const TrackerHandle, index: u64) -> u32 {
    tracker(handle).region_kept_sync(index)
}

#[no_mangle]
pub extern "C" fn kyty_tr_note_guest_write_fault(handle: *const TrackerHandle, vaddr: u64) {
    tracker(handle).note_guest_write_fault(vaddr)
}

#[no_mangle]
pub extern "C" fn kyty_tr_open_write_block(handle: *const TrackerHandle, vaddr: u64, block_size: u64, open_block: bool) -> WriteBlockProbe {
    tracker(handle).open_write_block(vaddr, block_size, open_block)
}

#[no_mangle]
pub extern "C" fn kyty_tr_take_kept_pages(handle: *const TrackerHandle) -> bool {
    tracker(handle).take_kept_pages()
}

#[no_mangle]
pub extern "C" fn kyty_tr_region_cpu_dirty_epoch(handle: *const TrackerHandle, index: u64) -> u64 {
    tracker(handle).region_cpu_dirty_epoch(index)
}

#[no_mangle]
pub extern "C" fn kyty_tr_region_gpu_clean_epoch(handle: *const TrackerHandle, index: u64) -> u64 {
    tracker(handle).region_gpu_clean_epoch(index)
}

#[no_mangle]
pub extern "C" fn kyty_tr_cpu_modification_epoch(handle: *const TrackerHandle, vaddr: u64, size: u64) -> u64 {
    tracker(handle).cpu_modification_epoch(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_deferred_protection_pending(handle: *const TrackerHandle, vaddr: u64) -> bool {
    tracker(handle).deferred_protection_pending(vaddr)
}

#[no_mangle]
pub extern "C" fn kyty_tr_note_host_write(handle: *const TrackerHandle, vaddr: u64, size: u64) {
    tracker(handle).note_host_write(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_for_each_cpu_dirty_span(handle: *const TrackerHandle, vaddr: u64, size: u64, max_gap: u64, func: RangeFn, context: *mut c_void) {
    tracker(handle).for_each_cpu_dirty_span(vaddr, size, max_gap, &mut |address, bytes| func(context, address, bytes))
}

#[no_mangle]
pub extern "C" fn kyty_tr_copy_region_cpu_dirty(handle: *const TrackerHandle, index: u64, words: *mut u64) -> bool {
    let words = unsafe { &mut *(words as *mut [u64; WORDS]) };
    let mut mask = Mask(*words);
    let result = tracker(handle).copy_region_cpu_dirty(index, &mut mask);
    *words = mask.0;
    result
}

#[no_mangle]
pub extern "C" fn kyty_tr_consume_cpu_writes(handle: *const TrackerHandle, consumer: u32, vaddr: u64, size: u64, func: RangeFn, context: *mut c_void) {
    if consumer as usize >= crate::region::CONSUMERS {
        std::process::abort();
    }
    tracker(handle).consume_cpu_writes(consumer as usize, vaddr, size, &mut |address, bytes| func(context, address, bytes))
}

#[no_mangle]
pub extern "C" fn kyty_tr_mark_region_as_gpu_modified_unless_cpu_dirty(handle: *const TrackerHandle, vaddr: u64, size: u64, func: RangeFn, context: *mut c_void) {
    tracker(handle).mark_region_as_gpu_modified_unless_cpu_dirty(vaddr, size, &mut |address, bytes| func(context, address, bytes))
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn kyty_tr_for_each_upload_range(
    handle: *const TrackerHandle,
    vaddr: u64,
    size: u64,
    is_written: bool,
    device_sync: u32,
    defer: bool,
    range_func: RangeFn,
    upload_func: VoidFn,
    context: *mut c_void,
) {
    tracker(handle).for_each_upload_range(vaddr, size, is_written, device_sync, defer, &mut |address, bytes| range_func(context, address, bytes), &mut || {
        upload_func(context)
    })
}

#[no_mangle]
pub extern "C" fn kyty_tr_for_each_upload_ranges(
    handle: *const TrackerHandle,
    ranges: *const u64,
    count: usize,
    device_sync: u32,
    defer: bool,
    range_func: IndexedRangeFn,
    upload_func: VoidFn,
    context: *mut c_void,
) {
    let ranges = if count == 0 { &[][..] } else { unsafe { core::slice::from_raw_parts(ranges as *const (u64, u64), count) } };
    tracker(handle).for_each_upload_ranges(ranges, device_sync, defer, &mut |index, address, bytes| range_func(context, index, address, bytes), &mut || {
        upload_func(context)
    })
}

#[no_mangle]
pub extern "C" fn kyty_tr_apply_deferred_protection(region: *const c_void, vaddr: u64, size: u64) {
    unsafe { &*(region as *const Region) }.apply_deferred_protection(vaddr, size)
}

#[no_mangle]
pub extern "C" fn kyty_tr_region_index(region: *const c_void) -> u64 {
    unsafe { &*(region as *const Region) }.cpu_addr() / crate::pages::REGION_SIZE
}

#[no_mangle]
pub extern "C" fn kyty_tr_cpu_dirty_epoch() -> u64 {
    CPU_DIRTY_EPOCH.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn kyty_tr_gpu_clean_epoch() -> u64 {
    GPU_CLEAN_EPOCH.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn kyty_tr_take_opened_upload_bytes() -> u64 {
    OPENED_UPLOAD_BYTES.swap(0, Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn kyty_tr_take_contention(class: u32, waits: *mut u64, ns: *mut u64) {
    let (taken_waits, taken_ns) = spin::take_contention(class as usize);
    unsafe {
        *waits = taken_waits;
        *ns = taken_ns;
    }
}

#[no_mangle]
pub extern "C" fn kyty_tr_debug_state(handle: *const TrackerHandle, pages_handle: *const Pages, emit: extern "C" fn(*mut c_void, *const u64, usize), context: *mut c_void) {
    let mut out = |words: &[u64]| emit(context, words.as_ptr(), words.len());
    if !handle.is_null() {
        digest::emit_tracker(tracker(handle), &mut out);
    }
    if !pages_handle.is_null() {
        digest::emit_pages(pages(pages_handle), &mut out);
    }
}
