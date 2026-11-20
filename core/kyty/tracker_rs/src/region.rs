use crate::arena::Zeroable;
use crate::bits::{AtomicBits, Mask, PAGES};
use crate::host::{self, Fatal};
use crate::pages::{DeferTarget, Pages, Watch, PAGE_SIZE, REGION_SIZE};
use crate::spin::{LockClass, SpinLock};
use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicU64, Ordering};

pub static CPU_DIRTY_EPOCH: AtomicU64 = AtomicU64::new(0);
pub static GPU_CLEAN_EPOCH: AtomicU64 = AtomicU64::new(0);
pub static OPENED_UPLOAD_BYTES: AtomicU64 = AtomicU64::new(0);

pub const CONSUMERS: usize = 2;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Source {
    Cpu,
    Gpu,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Change {
    Mark,
    Clear,
}

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct WriteBlockProbe {
    pub opened: u32,
    pub gpu: bool,
    pub tracked: bool,
}

pub struct BusyPages {
    pub seen: [u32; PAGES],
    pub cleaned: [u32; PAGES],
    pub writes: [u8; PAGES],
    pub faulted: Mask,
}

pub struct Locked {
    pub cpu_written: [Mask; CONSUMERS],
    pub gpu_written: Mask,
    pub gpu_noted: Mask,
    pub block_opened: Mask,
    pub any_block_opened: bool,
    pub busy: Option<Box<BusyPages>>,
}

pub struct Region {
    pub lock: SpinLock<Locked>,
    pages: AtomicPtr<Pages>,
    cpu_addr: AtomicU64,
    pub cpu_dirty: AtomicBits,
    pub gpu_dirty: AtomicBits,
    pub has_cpu_dirty: AtomicBool,
    pub has_gpu_dirty: AtomicBool,
    pub cpu_dirty_epoch: AtomicU64,
    pub gpu_clean_epoch: AtomicU64,
    pub kept_sync: AtomicU32,
    pub host_write_epoch: AtomicU64,
    pub deferred_protects: AtomicU32,
}

unsafe impl Zeroable for Region {}

pub struct Defer<'a>(pub &'a Region);

impl DeferTarget for Defer<'_> {
    fn defer(&self, address: u64, size: u64) {
        self.0.deferred_protects.fetch_add(1, Ordering::AcqRel);
        (host::get().defer_protect)(self.0 as *const Region as *const c_void, address, size);
    }
}

fn page_range(offset: u64, size: u64) -> (usize, usize) {
    ((offset / PAGE_SIZE) as usize, ((offset + size + PAGE_SIZE - 1) / PAGE_SIZE) as usize)
}

impl Region {
    pub fn init(&self, pages: &Pages, cpu_addr: u64) {
        self.lock.set_class(LockClass::Tracker);
        self.pages.store(pages as *const Pages as *mut Pages, Ordering::Relaxed);
        self.cpu_addr.store(cpu_addr, Ordering::Relaxed);
        self.cpu_dirty.fill();
        let locked = unsafe { self.lock.value_unlocked() };
        locked.cpu_written = [Mask::FULL; CONSUMERS];
        self.has_cpu_dirty.store(true, Ordering::Relaxed);
        self.cpu_dirty_epoch.store(1, Ordering::Relaxed);
        self.gpu_clean_epoch.store(1, Ordering::Relaxed);
        CPU_DIRTY_EPOCH.fetch_add(1, Ordering::AcqRel);
    }

    #[inline]
    pub fn cpu_addr(&self) -> u64 {
        self.cpu_addr.load(Ordering::Relaxed)
    }

    #[inline]
    fn pages(&self) -> &Pages {
        unsafe { &*self.pages.load(Ordering::Relaxed) }
    }

    fn bits(&self, source: Source) -> &AtomicBits {
        match source {
            Source::Cpu => &self.cpu_dirty,
            Source::Gpu => &self.gpu_dirty,
        }
    }

    pub fn cpu_modification_epoch(&self) -> u64 {
        if self.deferred_protects.load(Ordering::Acquire) != 0 {
            return 0;
        }
        self.cpu_dirty_epoch.load(Ordering::Acquire) + self.gpu_clean_epoch.load(Ordering::Acquire) + self.host_write_epoch.load(Ordering::Acquire)
    }

    pub fn deferred_protection_pending(&self) -> bool {
        self.deferred_protects.load(Ordering::Acquire) != 0
    }

    pub fn apply_deferred_protection(&self, vaddr: u64, size: u64) {
        self.pages().reapply_protection(vaddr, size);
        self.host_write_epoch.fetch_add(1, Ordering::AcqRel);
        self.deferred_protects.fetch_sub(1, Ordering::AcqRel);
    }

    pub fn note_host_write(&self) {
        self.host_write_epoch.fetch_add(1, Ordering::Release);
    }

    pub fn is_modified(&self, source: Source, offset: u64, size: u64) -> bool {
        let (start, end) = page_range(offset, size);
        self.bits(source).any(start, end)
    }

    pub fn open_write_block(&self, locked: &mut Locked, offset: u64, block_size: u64, open_block: bool) -> WriteBlockProbe {
        let page = (offset / PAGE_SIZE) as usize;
        let first = (offset / block_size * block_size / PAGE_SIZE) as usize;
        let last = first + (block_size / PAGE_SIZE) as usize;
        let mut probe = WriteBlockProbe { opened: 0, gpu: locked.gpu_written.any(first, last), tracked: !self.cpu_dirty.get(page) };
        self.note_guest_write_fault(locked, offset);
        if !open_block || self.gpu_dirty.get(page) {
            return probe;
        }
        let mut open = self.gpu_dirty.not_masked(first, last);
        let mut noted = locked.gpu_noted.masked(first, last);
        noted.unset(page);
        open.and_assign(&noted.not());
        let mut clean = self.cpu_dirty.not_masked(first, last);
        clean.and_assign(&open);
        for (begin, end) in clean.ranges() {
            probe.opened += (end - begin) as u32;
        }
        for (begin, end) in open.ranges() {
            self.change_state(locked, Source::Cpu, Change::Mark, begin as u64 * PAGE_SIZE, (end - begin) as u64 * PAGE_SIZE, None);
        }
        clean.unset(page);
        if !clean.none() {
            locked.block_opened.or_assign(&clean);
            locked.any_block_opened = true;
        }
        probe
    }

    pub fn change_state(&self, locked: &mut Locked, source: Source, change: Change, offset: u64, size: u64, sink: Option<&dyn DeferTarget>) {
        let (start, end) = page_range(offset, size);
        if source == Source::Cpu && change == Change::Mark && self.gpu_dirty.any(start, end) {
            host::fatal(Fatal::CpuDirtyConflictsGpuDirty, self.cpu_addr() + offset, size);
        }
        let bits = self.bits(source);
        let changed = match change {
            Change::Mark => bits.not_masked(start, end),
            Change::Clear => bits.masked(start, end),
        };
        match change {
            Change::Mark => bits.set_range(start, end),
            Change::Clear => bits.unset_range(start, end),
        }
        match (source, change) {
            (Source::Cpu, Change::Mark) => {
                for written in locked.cpu_written.iter_mut() {
                    written.set_range(start, end);
                }
                self.cpu_dirty_epoch.fetch_add(1, Ordering::AcqRel);
                CPU_DIRTY_EPOCH.fetch_add(1, Ordering::AcqRel);
                (host::get().bda_mark)(self.cpu_addr() / REGION_SIZE);
                self.has_cpu_dirty.store(!self.cpu_dirty.none(), Ordering::Release);
                self.update_protection(&changed, false, Watch::Write, sink);
            }
            (Source::Cpu, Change::Clear) => {
                Self::note_opened_upload(locked, None, &changed);
                self.has_cpu_dirty.store(!self.cpu_dirty.none(), Ordering::Release);
                self.update_protection(&changed, true, Watch::Write, sink);
            }
            (Source::Gpu, Change::Clear) => {
                self.gpu_clean_epoch.fetch_add(1, Ordering::AcqRel);
                GPU_CLEAN_EPOCH.fetch_add(1, Ordering::AcqRel);
                self.update_protection(&changed, false, Watch::Access, sink);
            }
            (Source::Gpu, Change::Mark) => {
                locked.gpu_written.set_range(start, end);
                self.update_protection(&changed, true, Watch::Access, sink);
            }
        }
    }

    pub fn for_each_modified_range(
        &self,
        locked: &mut Locked,
        source: Source,
        clear: bool,
        offset: u64,
        size: u64,
        sink: Option<&dyn DeferTarget>,
        func: &mut dyn FnMut(u64, u64),
    ) {
        let (start, end) = page_range(offset, size);
        let bits = self.bits(source);
        if !bits.any(start, end) {
            return;
        }
        let mask = bits.masked(start, end);
        if clear {
            bits.unset_range(start, end);
            match source {
                Source::Cpu => {
                    Self::note_opened_upload(locked, Some(&mask), &mask);
                    self.has_cpu_dirty.store(!self.cpu_dirty.none(), Ordering::Release);
                    self.update_protection(&mask, true, Watch::Write, sink);
                }
                Source::Gpu => {
                    self.gpu_clean_epoch.fetch_add(1, Ordering::AcqRel);
                    GPU_CLEAN_EPOCH.fetch_add(1, Ordering::AcqRel);
                    self.update_protection(&mask, false, Watch::Access, sink);
                }
            }
        }
        let base = self.cpu_addr();
        for (first, last) in mask.ranges() {
            func(base + first as u64 * PAGE_SIZE, (last - first) as u64 * PAGE_SIZE);
        }
    }

    fn gather(&self, ranges: &[(u64, u64)]) -> Mask {
        let mut mask = Mask::EMPTY;
        for &(offset, size) in ranges {
            let (start, end) = page_range(offset, size);
            if self.cpu_dirty.first_range_from(start).0 < end {
                mask.or_assign(&self.cpu_dirty.masked(start, end));
            }
        }
        mask
    }

    fn report(&self, ranges: &[(u64, u64)], mask: &Mask, func: &mut dyn FnMut(usize, u64, u64)) {
        let base = self.cpu_addr();
        for (index, &(offset, size)) in ranges.iter().enumerate() {
            let (start, end) = page_range(offset, size);
            for (first, last) in mask.masked(start, end).ranges() {
                func(index, base + first as u64 * PAGE_SIZE, (last - first) as u64 * PAGE_SIZE);
            }
        }
    }

    pub fn consume_cpu_dirty_ranges(&self, locked: &mut Locked, ranges: &[(u64, u64)], sink: Option<&dyn DeferTarget>, func: &mut dyn FnMut(usize, u64, u64)) {
        let mask = self.gather(ranges);
        if mask.none() {
            return;
        }
        for (first, last) in mask.ranges() {
            self.cpu_dirty.unset_range(first, last);
        }
        Self::note_opened_upload(locked, Some(&mask), &mask);
        self.has_cpu_dirty.store(!self.cpu_dirty.none(), Ordering::Release);
        self.update_protection(&mask, true, Watch::Write, sink);
        self.report(ranges, &mask, func);
    }

    pub fn consume_cpu_dirty_ranges_keeping_busy(
        &self,
        locked: &mut Locked,
        ranges: &[(u64, u64)],
        sync: u32,
        sink: Option<&dyn DeferTarget>,
        func: &mut dyn FnMut(usize, u64, u64),
    ) -> bool {
        const MAX_GAP: u32 = 3;
        const MIN_WRITES: u8 = 3;
        const REVALIDATE_PERIOD: u32 = 12;
        let mask = self.gather(ranges);
        if mask.none() {
            return false;
        }
        if locked.busy.is_none() {
            locked.busy = Some(Box::new(BusyPages { seen: [0; PAGES], cleaned: [0; PAGES], writes: [0; PAGES], faulted: Mask::EMPTY }));
        }
        let gpu_written = locked.gpu_written;
        let busy = locked.busy.as_mut().unwrap();
        let mut clean = Mask::EMPTY;
        let mut kept = false;
        for (first, last) in mask.ranges() {
            for page in first..last {
                if busy.faulted.get(page) {
                    busy.faulted.unset(page);
                    let soon = busy.seen[page] != 0 && sync.wrapping_sub(busy.seen[page]) <= MAX_GAP;
                    busy.writes[page] = if soon { (busy.writes[page] as u32 + 1).min(255) as u8 } else { 1 };
                }
                busy.seen[page] = sync;
                if busy.writes[page] >= MIN_WRITES && sync.wrapping_sub(busy.cleaned[page]) < REVALIDATE_PERIOD && !gpu_written.get(page) {
                    kept = true;
                } else {
                    busy.writes[page] = busy.writes[page].min(MIN_WRITES - 1);
                    busy.cleaned[page] = sync;
                    clean.set(page);
                    self.cpu_dirty.unset(page);
                }
            }
        }
        Self::note_opened_upload(locked, Some(&mask), &clean);
        self.has_cpu_dirty.store(!self.cpu_dirty.none(), Ordering::Release);
        self.update_protection(&clean, true, Watch::Write, sink);
        if kept {
            self.kept_sync.store(sync, Ordering::Relaxed);
        }
        self.report(ranges, &mask, func);
        kept
    }

    pub fn note_guest_write_fault(&self, locked: &mut Locked, offset: u64) {
        if let Some(busy) = locked.busy.as_mut() {
            busy.faulted.set((offset / PAGE_SIZE) as usize);
        }
    }

    pub fn consume_cpu_written(&self, locked: &mut Locked, consumer: usize, offset: u64, size: u64, func: &mut dyn FnMut(u64, u64)) {
        let (start, end) = page_range(offset, size);
        let written = &mut locked.cpu_written[consumer];
        let mut mask = written.masked(start, end);
        mask.or_assign(&self.cpu_dirty.masked(start, end));
        written.unset_range(start, end);
        let base = self.cpu_addr();
        for (first, last) in mask.ranges() {
            func(base + first as u64 * PAGE_SIZE, (last - first) as u64 * PAGE_SIZE);
        }
    }

    pub fn mark_gpu_dirty_unless_cpu_dirty(&self, locked: &mut Locked, offset: u64, size: u64, func: &mut dyn FnMut(u64, u64)) {
        let (start, end) = page_range(offset, size);
        locked.gpu_written.set_range(start, end);
        locked.gpu_noted.set_range(start, end);
        let mask = self.cpu_dirty.not_masked(start, end);
        if mask.none() {
            return;
        }
        let mut either = self.cpu_dirty.load();
        either.or_assign(&self.gpu_dirty.load());
        let changed = either.not().masked(start, end);
        for (first, last) in mask.ranges() {
            self.gpu_dirty.set_range(first, last);
        }
        self.update_protection(&changed, true, Watch::Access, None);
        let base = self.cpu_addr();
        for (first, last) in mask.ranges() {
            func(base + first as u64 * PAGE_SIZE, (last - first) as u64 * PAGE_SIZE);
        }
    }

    pub fn copy_cpu_dirty(&self) -> Mask {
        self.cpu_dirty.load()
    }

    pub fn has_cpu_dirty(&self) -> bool {
        self.has_cpu_dirty.load(Ordering::Acquire)
    }

    pub fn has_gpu_dirty(&self) -> bool {
        self.has_gpu_dirty.load(Ordering::Acquire)
    }

    fn note_opened_upload(locked: &mut Locked, uploaded: Option<&Mask>, cleaned: &Mask) {
        if !locked.any_block_opened {
            return;
        }
        if let Some(uploaded) = uploaded {
            let mut hit = *uploaded;
            hit.and_assign(&locked.block_opened);
            let pages = hit.count();
            if pages != 0 {
                OPENED_UPLOAD_BYTES.fetch_add(pages * PAGE_SIZE, Ordering::Relaxed);
            }
        }
        locked.block_opened.and_assign(&cleaned.not());
        locked.any_block_opened = !locked.block_opened.none();
    }

    fn update_protection(&self, changed: &Mask, track: bool, watch: Watch, sink: Option<&dyn DeferTarget>) {
        if changed.none() {
            return;
        }
        if watch == Watch::Access {
            self.has_gpu_dirty.store(!self.gpu_dirty.none(), Ordering::Release);
        }
        self.pages().update_page_watchers_for_region(self.cpu_addr(), changed, track, watch, sink);
    }
}
