use crate::pages::Pages;
use crate::region::{CPU_DIRTY_EPOCH, GPU_CLEAN_EPOCH, OPENED_UPLOAD_BYTES};
use crate::tracker::Tracker;
use core::sync::atomic::Ordering::Relaxed;

pub const MARK: u64 = u64::MAX;

pub fn emit_tracker(tracker: &Tracker, out: &mut dyn FnMut(&[u64])) {
    tracker.regions().for_each(|index, region| {
        let locked = unsafe { region.lock.value_unlocked() };
        out(&[index as u64]);
        out(&region.cpu_dirty.load().0);
        out(&region.gpu_dirty.load().0);
        out(&locked.cpu_written[0].0);
        out(&locked.cpu_written[1].0);
        out(&locked.gpu_written.0);
        out(&locked.gpu_noted.0);
        out(&locked.block_opened.0);
        out(&[
            locked.any_block_opened as u64,
            region.has_cpu_dirty.load(Relaxed) as u64,
            region.has_gpu_dirty.load(Relaxed) as u64,
            region.cpu_dirty_epoch.load(Relaxed),
            region.gpu_clean_epoch.load(Relaxed),
            region.host_write_epoch.load(Relaxed),
            region.deferred_protects.load(Relaxed) as u64,
            region.kept_sync.load(Relaxed) as u64,
            locked.busy.is_some() as u64,
        ]);
        if let Some(busy) = locked.busy.as_ref() {
            for page in 0..crate::bits::PAGES {
                out(&[busy.seen[page] as u64 | ((busy.cleaned[page] as u64) << 32)]);
            }
            for chunk in busy.writes.chunks(8) {
                let mut word = 0u64;
                for (byte, value) in chunk.iter().enumerate() {
                    word |= (*value as u64) << (byte * 8);
                }
                out(&[word]);
            }
            out(&busy.faulted.0);
        }
    });
    out(&[MARK, CPU_DIRTY_EPOCH.load(Relaxed), GPU_CLEAN_EPOCH.load(Relaxed), OPENED_UPLOAD_BYTES.load(Relaxed), tracker.kept_pages_flag() as u64]);
}

pub fn emit_pages(pages: &Pages, out: &mut dyn FnMut(&[u64])) {
    pages.regions().for_each(|index, region| {
        let states = unsafe { region.lock.value_unlocked() };
        out(&[index as u64]);
        for (page, state) in states.iter().enumerate() {
            out(&[state.write_watchers as u64
                | (state.access_watchers as u64) << 8
                | (state.hot as u64) << 16
                | (state.write_faults as u64) << 24
                | (state.fault_window as u64) << 32
                | (region.read_hint[page].load(Relaxed) as u64) << 40]);
        }
    });
    out(&[MARK]);
    for (index, word) in pages.read_granule_words().iter().enumerate() {
        let value = word.load(Relaxed);
        if value != 0 {
            out(&[index as u64, value]);
        }
    }
    out(&[MARK]);
    for (index, word) in pages.hot_granule_words().iter().enumerate() {
        let value = word.load(Relaxed);
        if value != 0 {
            out(&[index as u64, value]);
        }
    }
    out(&[MARK]);
    let (hot_pages, hot_count, hot_any) = pages.hot_snapshot();
    for (page, last) in hot_pages {
        out(&[page, last as u64]);
    }
    let counters = &pages.counters;
    out(&[
        MARK,
        hot_count as u64,
        hot_any as u64,
        counters.guest_write_faults.load(Relaxed),
        counters.protects[0].load(Relaxed),
        counters.protects[1].load(Relaxed),
        counters.protects[2].load(Relaxed),
        counters.protects[3].load(Relaxed),
        counters.hot_last_line.load(Relaxed),
        counters.hot_scans.load(Relaxed),
        counters.hot_scan_ns.load(Relaxed),
        counters.hot_collected.load(Relaxed),
        counters.hot_idle.load(Relaxed),
        counters.hot_lost.load(Relaxed),
        counters.hot_page_total.load(Relaxed),
        counters.hot_run_total.load(Relaxed),
        counters.hot_scoped_scans.load(Relaxed),
        counters.hot_promoted.load(Relaxed),
        counters.hot_refused.load(Relaxed),
    ]);
}
