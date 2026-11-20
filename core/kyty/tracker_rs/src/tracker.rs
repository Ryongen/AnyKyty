use crate::arena::Arena;
use crate::bits::Mask;
use crate::host::{self, Fatal};
use crate::pages::{guest_range_valid, Pages, ADDRESS_SIZE, REGION_COUNT, REGION_SIZE};
use crate::region::{Change, Defer, Locked, Region, Source, WriteBlockProbe};
use crate::spin::Guard;
use core::cell::RefCell;
use core::sync::atomic::{AtomicBool, Ordering};

pub struct Tracker {
    regions: Arena<Region>,
    pages: *const Pages,
    kept_pages: AtomicBool,
}

unsafe impl Send for Tracker {}
unsafe impl Sync for Tracker {}

#[derive(Clone, Copy)]
struct Piece {
    region: u64,
    offset: u64,
    size: u64,
    range: usize,
}

thread_local! {
    static PIECES: RefCell<Vec<Piece>> = const { RefCell::new(Vec::new()) };
    static OFFSETS: RefCell<Vec<(u64, u64)>> = const { RefCell::new(Vec::new()) };
}

fn validate_range(vaddr: u64, size: u64) {
    if !guest_range_valid(vaddr, size) {
        host::fatal(Fatal::TrackerInvalidRange, vaddr, size);
    }
}

fn check_index(index: u64) {
    if index >= REGION_COUNT as u64 {
        host::fatal(Fatal::TrackerInvalidRegion, index, 0);
    }
}

struct Held<'a> {
    region: &'a Region,
    offset: u64,
    bytes: u64,
    guard: Guard<'a, Locked>,
}

impl Tracker {
    pub fn new(pages: &Pages) -> Self {
        Tracker { regions: Arena::new(REGION_COUNT), pages: pages as *const Pages, kept_pages: AtomicBool::new(false) }
    }

    pub fn regions(&self) -> &Arena<Region> {
        &self.regions
    }

    pub fn kept_pages_flag(&self) -> bool {
        self.kept_pages.load(Ordering::Relaxed)
    }

    fn pages(&self) -> &Pages {
        unsafe { &*self.pages }
    }

    #[inline]
    fn find(&self, index: u64) -> Option<&Region> {
        self.regions.find(index as usize)
    }

    fn get_or_create(&self, index: u64) -> &Region {
        let pages = self.pages();
        self.regions.get_or_create(index as usize, |region| region.init(pages, index * REGION_SIZE))
    }

    fn iterate<'a>(&'a self, vaddr: u64, size: u64, create: bool, mut func: impl FnMut(&'a Region, u64, u64) -> bool) -> bool {
        validate_range(vaddr, size);
        let mut remaining = size;
        let mut index = vaddr / REGION_SIZE;
        let mut offset = vaddr % REGION_SIZE;
        while remaining != 0 {
            let bytes = (REGION_SIZE - offset).min(remaining);
            let region = match self.find(index) {
                Some(region) => Some(region),
                None if create => Some(self.get_or_create(index)),
                None => None,
            };
            if let Some(region) = region {
                if func(region, offset, bytes) {
                    return true;
                }
            }
            remaining -= bytes;
            offset = 0;
            index += 1;
        }
        false
    }

    pub fn is_region_cpu_modified(&self, vaddr: u64, size: u64) -> bool {
        self.iterate(vaddr, size, true, |region, offset, bytes| region.is_modified(Source::Cpu, offset, bytes))
    }

    pub fn is_region_gpu_modified(&self, vaddr: u64, size: u64) -> bool {
        if !self.may_be_gpu_modified(vaddr, size) {
            validate_range(vaddr, size);
            return false;
        }
        self.iterate(vaddr, size, false, |region, offset, bytes| region.is_modified(Source::Gpu, offset, bytes))
    }

    pub fn may_be_gpu_modified(&self, vaddr: u64, size: u64) -> bool {
        if size == 0 || size > ADDRESS_SIZE || vaddr > ADDRESS_SIZE - size {
            return true;
        }
        let last = (vaddr + size - 1) / REGION_SIZE;
        let mut index = vaddr / REGION_SIZE;
        while index <= last && index < REGION_COUNT as u64 {
            if let Some(region) = self.find(index) {
                if region.has_gpu_dirty() {
                    return true;
                }
            }
            index += 1;
        }
        false
    }

    pub fn mark_region_as_cpu_modified(&self, vaddr: u64, size: u64) {
        self.iterate(vaddr, size, false, |region, offset, bytes| {
            let mut locked = region.lock.lock();
            region.change_state(&mut locked, Source::Cpu, Change::Mark, offset, bytes, None);
            false
        });
    }

    pub fn unmark_region_as_gpu_modified(&self, vaddr: u64, size: u64) {
        self.iterate(vaddr, size, false, |region, offset, bytes| {
            let mut locked = region.lock.lock();
            region.change_state(&mut locked, Source::Gpu, Change::Clear, offset, bytes, None);
            false
        });
    }

    pub fn invalidate_region(&self, vaddr: u64, size: u64, on_flush: &mut dyn FnMut()) {
        self.iterate(vaddr, size, false, |region, offset, bytes| {
            let should_flush = {
                let mut locked = region.lock.lock();
                if region.is_modified(Source::Gpu, offset, bytes) {
                    true
                } else {
                    region.change_state(&mut locked, Source::Cpu, Change::Mark, offset, bytes, None);
                    false
                }
            };
            if should_flush {
                on_flush();
            }
            false
        });
    }

    pub fn for_each_download_range(&self, vaddr: u64, size: u64, clear: bool, func: &mut dyn FnMut(u64, u64)) {
        self.iterate(vaddr, size, false, |region, offset, bytes| {
            let mut locked = region.lock.lock();
            region.for_each_modified_range(&mut locked, Source::Gpu, false, offset, bytes, None, func);
            if clear {
                region.change_state(&mut locked, Source::Gpu, Change::Clear, offset, bytes, None);
            }
            false
        });
    }

    pub fn region_kept_sync(&self, index: u64) -> u32 {
        check_index(index);
        self.find(index).map_or(0, |region| region.kept_sync.load(Ordering::Relaxed))
    }

    pub fn note_guest_write_fault(&self, vaddr: u64) {
        let index = vaddr / REGION_SIZE;
        if index >= REGION_COUNT as u64 {
            return;
        }
        if let Some(region) = self.find(index) {
            let mut locked = region.lock.lock();
            region.note_guest_write_fault(&mut locked, vaddr % REGION_SIZE);
        }
    }

    pub fn open_write_block(&self, vaddr: u64, block_size: u64, open_block: bool) -> WriteBlockProbe {
        let index = vaddr / REGION_SIZE;
        if index >= REGION_COUNT as u64 {
            return WriteBlockProbe::default();
        }
        let Some(region) = self.find(index) else {
            return WriteBlockProbe::default();
        };
        let mut locked = region.lock.lock();
        region.open_write_block(&mut locked, vaddr % REGION_SIZE, block_size, open_block)
    }

    pub fn take_kept_pages(&self) -> bool {
        self.kept_pages.swap(false, Ordering::Relaxed)
    }

    pub fn region_cpu_dirty_epoch(&self, index: u64) -> u64 {
        check_index(index);
        self.find(index).map_or(0, |region| region.cpu_dirty_epoch.load(Ordering::Acquire))
    }

    pub fn region_gpu_clean_epoch(&self, index: u64) -> u64 {
        check_index(index);
        self.find(index).map_or(0, |region| region.gpu_clean_epoch.load(Ordering::Acquire))
    }

    pub fn cpu_modification_epoch(&self, vaddr: u64, size: u64) -> u64 {
        if size == 0 || !guest_range_valid(vaddr, size) || vaddr / REGION_SIZE != (vaddr + size - 1) / REGION_SIZE {
            return 0;
        }
        self.find(vaddr / REGION_SIZE).map_or(0, |region| region.cpu_modification_epoch())
    }

    pub fn deferred_protection_pending(&self, vaddr: u64) -> bool {
        if !guest_range_valid(vaddr, 1) {
            return false;
        }
        self.find(vaddr / REGION_SIZE).is_some_and(|region| region.deferred_protection_pending())
    }

    pub fn note_host_write(&self, vaddr: u64, size: u64) {
        if size == 0 || !guest_range_valid(vaddr, size) {
            return;
        }
        let mut index = vaddr / REGION_SIZE;
        while index * REGION_SIZE < vaddr + size {
            if let Some(region) = self.find(index) {
                region.note_host_write();
            }
            index += 1;
        }
    }

    pub fn for_each_cpu_dirty_span(&self, vaddr: u64, size: u64, max_gap: u64, func: &mut dyn FnMut(u64, u64)) {
        validate_range(vaddr, size);
        let end = vaddr + size;
        let mut spans: Vec<(u64, u64)> = Vec::new();
        let mut add = |begin: u64, finish: u64| {
            let begin = begin.max(vaddr);
            let finish = finish.min(end);
            if begin >= finish {
                return;
            }
            match spans.last_mut() {
                Some(last) if begin <= last.1 + max_gap => last.1 = finish,
                _ => spans.push((begin, finish)),
            }
        };
        let mut index = vaddr / REGION_SIZE;
        while index * REGION_SIZE < end {
            let start = (index * REGION_SIZE).max(vaddr);
            let finish = ((index + 1) * REGION_SIZE).min(end);
            match self.find(index) {
                None => add(start, finish),
                Some(region) if !region.has_cpu_dirty() => {}
                Some(region) => {
                    let mut locked = region.lock.lock();
                    region.for_each_modified_range(&mut locked, Source::Cpu, false, start - index * REGION_SIZE, finish - start, None, &mut |address, bytes| {
                        add(address, address + bytes)
                    });
                }
            }
            index += 1;
        }
        for (begin, finish) in spans {
            func(begin, finish - begin);
        }
    }

    pub fn copy_region_cpu_dirty(&self, index: u64, pages: &mut Mask) -> bool {
        check_index(index);
        let Some(region) = self.find(index) else {
            *pages = Mask::FULL;
            return true;
        };
        if !region.has_cpu_dirty() {
            return false;
        }
        let _locked = region.lock.lock();
        *pages = region.copy_cpu_dirty();
        !pages.none()
    }

    pub fn consume_cpu_writes(&self, consumer: usize, vaddr: u64, size: u64, func: &mut dyn FnMut(u64, u64)) {
        let mut runs: Vec<(u64, u64)> = Vec::new();
        self.iterate(vaddr, size, true, |region, offset, bytes| {
            let mut locked = region.lock.lock();
            region.consume_cpu_written(&mut locked, consumer, offset, bytes, &mut |address, length| match runs.last_mut() {
                Some(last) if last.0 + last.1 == address => last.1 += length,
                _ => runs.push((address, length)),
            });
            false
        });
        let end = vaddr + size;
        for (address, length) in runs {
            let begin = address.max(vaddr);
            let finish = (address + length).min(end);
            if begin < finish {
                func(begin, finish - begin);
            }
        }
    }

    pub fn mark_region_as_gpu_modified_unless_cpu_dirty(&self, vaddr: u64, size: u64, func: &mut dyn FnMut(u64, u64)) {
        let mut runs: Vec<(u64, u64)> = Vec::new();
        self.iterate(vaddr, size, true, |region, offset, bytes| {
            let mut locked = region.lock.lock();
            region.mark_gpu_dirty_unless_cpu_dirty(&mut locked, offset, bytes, &mut |address, length| runs.push((address, length)));
            false
        });
        for (address, length) in runs {
            func(address, length);
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn for_each_upload_range(
        &self,
        vaddr: u64,
        size: u64,
        is_written: bool,
        device_sync: u32,
        defer: bool,
        range_func: &mut dyn FnMut(u64, u64),
        upload_func: &mut dyn FnMut(),
    ) {
        let mut held: Vec<Held<'_>> = Vec::new();
        self.iterate(vaddr, size, true, |region, offset, bytes| {
            if !is_written && (!region.has_cpu_dirty() || !region.is_modified(Source::Cpu, offset, bytes)) {
                return false;
            }
            let sink = Defer(region);
            let sink = if defer { Some(&sink as &dyn crate::pages::DeferTarget) } else { None };
            if !is_written && device_sync != 0 {
                let range = [(offset, bytes)];
                let mut locked = region.lock.lock();
                if region.consume_cpu_dirty_ranges_keeping_busy(&mut locked, &range, device_sync, sink, &mut |_, address, run| range_func(address, run)) {
                    self.kept_pages.store(true, Ordering::Relaxed);
                }
                return false;
            }
            let mut guard = region.lock.lock();
            region.for_each_modified_range(&mut guard, Source::Cpu, true, offset, bytes, sink, range_func);
            if is_written {
                held.push(Held { region, offset, bytes, guard });
            }
            false
        });
        upload_func();
        for mut item in held {
            item.region.change_state(&mut item.guard, Source::Gpu, Change::Mark, item.offset, item.bytes, None);
        }
    }

    pub fn for_each_upload_ranges(
        &self,
        ranges: &[(u64, u64)],
        device_sync: u32,
        defer: bool,
        range_func: &mut dyn FnMut(usize, u64, u64),
        upload_func: &mut dyn FnMut(),
    ) {
        let mut pieces = PIECES.with(|cell| core::mem::take(&mut *cell.borrow_mut()));
        let mut offsets = OFFSETS.with(|cell| core::mem::take(&mut *cell.borrow_mut()));
        pieces.clear();
        for (index, &(address, size)) in ranges.iter().enumerate() {
            validate_range(address, size);
            let mut at = address;
            while at < address + size {
                let region = at / REGION_SIZE;
                let end = (address + size).min((region + 1) * REGION_SIZE);
                pieces.push(Piece { region, offset: at - region * REGION_SIZE, size: end - at, range: index });
                at = end;
            }
        }
        let mut first = 0;
        while first < pieces.len() {
            let mut last = first + 1;
            while last < pieces.len() && pieces[last].region == pieces[first].region {
                last += 1;
            }
            let region = self.get_or_create(pieces[first].region);
            if region.has_cpu_dirty() {
                offsets.clear();
                for piece in &pieces[first..last] {
                    offsets.push((piece.offset, piece.size));
                }
                let mut locked = region.lock.lock();
                let sink = Defer(region);
                let sink = if defer { Some(&sink as &dyn crate::pages::DeferTarget) } else { None };
                let group = &pieces[first..last];
                let mut piece_func = |piece: usize, address: u64, bytes: u64| range_func(group[piece].range, address, bytes);
                if device_sync == 0 {
                    region.consume_cpu_dirty_ranges(&mut locked, &offsets, sink, &mut piece_func);
                } else if region.consume_cpu_dirty_ranges_keeping_busy(&mut locked, &offsets, device_sync, sink, &mut piece_func) {
                    self.kept_pages.store(true, Ordering::Relaxed);
                }
            }
            first = last;
        }
        PIECES.with(|cell| *cell.borrow_mut() = pieces);
        OFFSETS.with(|cell| *cell.borrow_mut() = offsets);
        upload_func();
    }
}
