use crate::arena::{Arena, Zeroable};
use crate::bits::Mask;
use crate::host::{self, Fatal};
use crate::spin::{LockClass, SpinLock};
use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicU8, Ordering};
use std::collections::BTreeMap;
use std::sync::Mutex;

pub const PAGE_SIZE: u64 = 4096;
pub const REGION_SIZE: u64 = 4 << 20;
pub const ADDRESS_SIZE: u64 = 1 << 44;
pub const REGION_COUNT: usize = (ADDRESS_SIZE / REGION_SIZE) as usize;
pub const REGION_PAGES: usize = (REGION_SIZE / PAGE_SIZE) as usize;
pub const LOWER_ADDRESS_SIZE: u64 = 1 << 40;
pub const EXTENDED_BASE: u64 = 0x0800_0000_0000;
pub const EXTENDED_SIZE: u64 = 512 << 30;
pub const READ_GRANULE_SIZE: u64 = 64 * 1024;
const READ_GRANULE_PAGES: usize = (READ_GRANULE_SIZE / PAGE_SIZE) as usize;
const HOT_FAULTS: u8 = 2;
const HOT_WINDOW_NS: u64 = 1_000_000_000;
const HOT_IDLE_SECONDS: u32 = 4;
const HOT_PAGE_CAP: u32 = 16384;
const HOT_IDLE_PER_SWEEP: usize = 1024;
const HOT_GRANULE_SIZE: u64 = 2 * 1024 * 1024;
const HOT_CLUSTER_GAP: u64 = 1024 * 1024;
const PENDING_SLOTS: usize = 2 * HOT_PAGE_CAP as usize;

pub fn guest_range_valid(address: u64, size: u64) -> bool {
    if address == 0 || size == 0 {
        return false;
    }
    if address < LOWER_ADDRESS_SIZE {
        return size <= LOWER_ADDRESS_SIZE - address;
    }
    address >= EXTENDED_BASE && address - EXTENDED_BASE < EXTENDED_SIZE && size <= EXTENDED_SIZE - (address - EXTENDED_BASE)
}

pub const fn align_down(value: u64, alignment: u64) -> u64 {
    value & !(alignment - 1)
}

pub const fn align_up(value: u64, alignment: u64) -> u64 {
    (value.wrapping_add(alignment - 1)) & !(alignment - 1)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum Mode {
    NoAccess = 0,
    Read = 1,
    ReadWrite = 3,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Hot {
    Cold = 0,
    On = 1,
    Refused = 2,
    Promoting = 3,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct PageState {
    pub write_watchers: u8,
    pub access_watchers: u8,
    pub hot: Hot,
    pub write_faults: u8,
    pub fault_window: u8,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Watch {
    Write,
    Access,
}

impl PageState {
    pub fn write_protected(&self) -> bool {
        self.write_watchers != 0 && self.hot != Hot::On
    }

    pub fn perms(&self) -> Mode {
        if self.access_watchers != 0 {
            Mode::NoAccess
        } else if self.write_protected() {
            Mode::Read
        } else {
            Mode::ReadWrite
        }
    }
}

pub struct PageRegion {
    pub lock: SpinLock<[PageState; REGION_PAGES]>,
    pub read_hint: [AtomicU8; REGION_PAGES],
}

unsafe impl Zeroable for PageRegion {}

pub trait DeferTarget {
    fn defer(&self, address: u64, size: u64);
}

struct HotSet {
    pages: BTreeMap<u64, u32>,
    runs: Vec<(u64, u64)>,
    clusters: Vec<(u64, u64)>,
    runs_dirty: bool,
    last_sweep: u32,
}

struct PendingRing {
    slots: Box<[AtomicU64]>,
    head: AtomicU64,
    tail: AtomicU64,
}

impl PendingRing {
    fn new() -> Self {
        PendingRing { slots: (0..PENDING_SLOTS).map(|_| AtomicU64::new(0)).collect(), head: AtomicU64::new(0), tail: AtomicU64::new(0) }
    }

    fn push(&self, page: u64) {
        let index = self.tail.fetch_add(1, Ordering::AcqRel);
        let slot = &self.slots[(index % PENDING_SLOTS as u64) as usize];
        while slot.compare_exchange(0, page, Ordering::AcqRel, Ordering::Acquire).is_err() {
            core::hint::spin_loop();
        }
    }

    fn drain(&self, out: &mut Vec<u64>) {
        let tail = self.tail.load(Ordering::Acquire);
        let mut head = self.head.load(Ordering::Relaxed);
        while head < tail {
            let slot = &self.slots[(head % PENDING_SLOTS as u64) as usize];
            loop {
                let page = slot.swap(0, Ordering::AcqRel);
                if page != 0 {
                    out.push(page);
                    break;
                }
                core::hint::spin_loop();
            }
            head += 1;
        }
        self.head.store(head, Ordering::Relaxed);
    }
}

#[derive(Default)]
pub struct Counters {
    pub hot_last_line: AtomicU64,
    pub hot_scans: AtomicU64,
    pub hot_scan_ns: AtomicU64,
    pub hot_collected: AtomicU64,
    pub hot_idle: AtomicU64,
    pub hot_lost: AtomicU64,
    pub hot_page_total: AtomicU64,
    pub hot_run_total: AtomicU64,
    pub guest_write_faults: AtomicU64,
    pub protects: [AtomicU64; 4],
    pub hot_scoped_scans: AtomicU64,
    pub hot_promoted: AtomicU64,
    pub hot_refused: AtomicU64,
}

pub struct Pages {
    regions: Arena<PageRegion>,
    read_granules: Box<[AtomicU64]>,
    hot_granules: Box<[AtomicU64]>,
    hot_enabled: bool,
    hot: Mutex<HotSet>,
    pending: PendingRing,
    hot_any: AtomicBool,
    hot_count: AtomicU32,
    pub counters: Counters,
}

fn zeroed_atomics(count: usize) -> Box<[AtomicU64]> {
    let layout = std::alloc::Layout::array::<AtomicU64>(count).unwrap();
    unsafe {
        let memory = std::alloc::alloc_zeroed(layout) as *mut AtomicU64;
        if memory.is_null() {
            std::alloc::handle_alloc_error(layout);
        }
        Box::from_raw(core::ptr::slice_from_raw_parts_mut(memory, count))
    }
}

fn validate_range(vaddr: u64, size: u64) {
    if !guest_range_valid(vaddr, size) {
        host::fatal(Fatal::PagesInvalidRange, vaddr, size);
    }
}

extern "C" fn push_pages(context: *mut c_void, begin: u64, end: u64) {
    let pages = unsafe { &mut *(context as *mut Vec<u64>) };
    let mut page = begin;
    while page < end {
        pages.push(page);
        page += PAGE_SIZE;
    }
}

impl Pages {
    pub fn new(hot_enabled: bool) -> Self {
        Pages {
            regions: Arena::new(REGION_COUNT),
            read_granules: zeroed_atomics((ADDRESS_SIZE / READ_GRANULE_SIZE / 64) as usize),
            hot_granules: zeroed_atomics((ADDRESS_SIZE / HOT_GRANULE_SIZE / 64) as usize),
            hot_enabled,
            hot: Mutex::new(HotSet { pages: BTreeMap::new(), runs: Vec::new(), clusters: Vec::new(), runs_dirty: false, last_sweep: 0 }),
            pending: PendingRing::new(),
            hot_any: AtomicBool::new(false),
            hot_count: AtomicU32::new(0),
            counters: Counters::default(),
        }
    }

    pub fn hot_enabled(&self) -> bool {
        self.hot_enabled
    }

    pub fn read_granules(&self) -> *const AtomicU64 {
        self.read_granules.as_ptr()
    }

    pub fn regions(&self) -> &Arena<PageRegion> {
        &self.regions
    }

    pub fn hot_granule_words(&self) -> &[AtomicU64] {
        &self.hot_granules
    }

    pub fn read_granule_words(&self) -> &[AtomicU64] {
        &self.read_granules
    }

    pub fn hot_snapshot(&self) -> (Vec<(u64, u32)>, u32, bool) {
        let hot = self.hot.lock().unwrap();
        (hot.pages.iter().map(|(page, last)| (*page, *last)).collect(), self.hot_count.load(Ordering::Relaxed), self.hot_any.load(Ordering::Relaxed))
    }

    #[inline]
    fn find_region(&self, vaddr: u64) -> Option<&PageRegion> {
        self.regions.find((vaddr / REGION_SIZE) as usize)
    }

    fn get_or_create_region(&self, vaddr: u64) -> &PageRegion {
        self.regions.get_or_create((vaddr / REGION_SIZE) as usize, |region| region.lock.set_class(LockClass::Pages))
    }

    fn protect(&self, vaddr: u64, size: u64, mode: Mode) {
        let kind = match mode {
            Mode::Read => 0,
            Mode::ReadWrite => 1,
            Mode::NoAccess => 2,
        };
        self.counters.protects[kind].fetch_add(1, Ordering::Relaxed);
        if !(host::get().protect)(vaddr, size, mode as u32) {
            host::fatal(Fatal::ProtectFailed, vaddr, mode as u64);
        }
    }

    fn mark_read_granule(&self, address: u64) {
        let granule = address / READ_GRANULE_SIZE;
        self.read_granules[(granule / 64) as usize].fetch_or(1u64 << (granule % 64), Ordering::Release);
    }

    fn sync_read_granules(&self, region: &PageRegion, base: u64, first: usize, last: usize) {
        if first >= last {
            return;
        }
        for granule_index in first / READ_GRANULE_PAGES..=(last - 1) / READ_GRANULE_PAGES {
            let mut any = false;
            for page in granule_index * READ_GRANULE_PAGES..(granule_index + 1) * READ_GRANULE_PAGES {
                any |= region.read_hint[page].load(Ordering::Relaxed) != 0;
            }
            let granule = base / READ_GRANULE_SIZE + granule_index as u64;
            let bit = 1u64 << (granule % 64);
            let word = &self.read_granules[(granule / 64) as usize];
            if any {
                word.fetch_or(bit, Ordering::Release);
            } else if word.load(Ordering::Relaxed) & bit != 0 {
                word.fetch_and(!bit, Ordering::Release);
            }
        }
    }

    fn may_have_read_watchers(&self, vaddr: u64, last_byte: u64) -> bool {
        if last_byte >= ADDRESS_SIZE {
            return true;
        }
        let mut granule = vaddr / READ_GRANULE_SIZE;
        while granule <= last_byte / READ_GRANULE_SIZE {
            if (self.read_granules[(granule / 64) as usize].load(Ordering::Acquire) >> (granule % 64)) & 1 != 0 {
                return true;
            }
            granule += 1;
        }
        false
    }

    #[allow(clippy::too_many_arguments)]
    fn update_region_watchers(
        &self,
        region: &PageRegion,
        base: u64,
        first: usize,
        last: usize,
        track: bool,
        watch: Watch,
        mask: Option<&Mask>,
        sink: Option<&dyn DeferTarget>,
    ) {
        let all = &Mask::EMPTY;
        match (track, watch, mask) {
            (true, Watch::Write, None) => self.update_region_watchers_with::<true, false, false>(region, base, first, last, all, sink),
            (true, Watch::Write, Some(mask)) => self.update_region_watchers_with::<true, false, true>(region, base, first, last, mask, sink),
            (true, Watch::Access, None) => self.update_region_watchers_with::<true, true, false>(region, base, first, last, all, sink),
            (true, Watch::Access, Some(mask)) => self.update_region_watchers_with::<true, true, true>(region, base, first, last, mask, sink),
            (false, Watch::Write, None) => self.update_region_watchers_with::<false, false, false>(region, base, first, last, all, sink),
            (false, Watch::Write, Some(mask)) => self.update_region_watchers_with::<false, false, true>(region, base, first, last, mask, sink),
            (false, Watch::Access, None) => self.update_region_watchers_with::<false, true, false>(region, base, first, last, all, sink),
            (false, Watch::Access, Some(mask)) => self.update_region_watchers_with::<false, true, true>(region, base, first, last, mask, sink),
        }
    }

    #[inline(always)]
    fn update_region_watchers_with<const TRACK: bool, const READ: bool, const MASKED: bool>(
        &self,
        region: &PageRegion,
        base: u64,
        first: usize,
        last: usize,
        mask: &Mask,
        sink: Option<&dyn DeferTarget>,
    ) {
        let mut guard = region.lock.lock();
        let states: &mut [PageState; REGION_PAGES] = &mut guard;
        let mut perms = states[first].perms();
        let mut range_begin: usize = 0;
        let mut range_bytes: u64 = 0;
        let mut potential: u64 = 0;
        let release = |range_begin: usize, range_bytes: &mut u64, potential: &mut u64, perms: Mode| {
            if *range_bytes != 0 {
                let address = base + range_begin as u64 * PAGE_SIZE;
                match sink {
                    Some(target) if TRACK && !READ => target.defer(address, *range_bytes),
                    _ => self.protect(address, *range_bytes, perms),
                }
                *range_bytes = 0;
                *potential = 0;
            }
        };
        for (page_index, page) in states.iter_mut().enumerate().take(last).skip(first) {
            let address = base + page_index as u64 * PAGE_SIZE;
            let update = !MASKED || mask.get(page_index);
            let old_perms = page.perms();
            let counter = if READ { &mut page.access_watchers } else { &mut page.write_watchers };
            if update {
                if TRACK {
                    if *counter == u8::MAX {
                        host::fatal(if READ { Fatal::ReadWatcherOverflow } else { Fatal::WriteWatcherOverflow }, address, 0);
                    }
                    *counter += 1;
                } else {
                    if *counter == 0 {
                        host::fatal(if READ { Fatal::ReadWatcherUnderflow } else { Fatal::WriteWatcherUnderflow }, address, 0);
                    }
                    *counter -= 1;
                }
            }
            let new_count = *counter;
            let new_perms = page.perms();
            if READ {
                let faults = page.access_watchers != 0;
                if faults {
                    self.mark_read_granule(address);
                }
                region.read_hint[page_index].store(faults as u8, Ordering::Release);
            }
            if new_perms != perms {
                release(range_begin, &mut range_bytes, &mut potential, perms);
                perms = new_perms;
            } else if range_bytes != 0 {
                potential += PAGE_SIZE;
            }
            if !update {
                continue;
            }
            let edge = if TRACK { new_count == 1 } else { new_count == 0 };
            if edge && old_perms != new_perms {
                if range_bytes == 0 {
                    range_begin = page_index;
                    potential = PAGE_SIZE;
                }
                range_bytes = potential;
            }
        }
        release(range_begin, &mut range_bytes, &mut potential, perms);
        drop(guard);
        if READ {
            self.sync_read_granules(region, base, first, last);
        }
    }

    pub fn update_page_watchers(&self, vaddr: u64, size: u64, track: bool, watch: Watch, sink: Option<&dyn DeferTarget>) {
        validate_range(vaddr, size);
        let begin = align_down(vaddr, PAGE_SIZE);
        let end = align_up(vaddr + size, PAGE_SIZE);
        let mut chunk = begin;
        while chunk < end {
            let chunk_end = end.min(align_up(chunk + 1, REGION_SIZE));
            let base = align_down(chunk, REGION_SIZE);
            let region = if track { Some(self.get_or_create_region(chunk)) } else { self.find_region(chunk) };
            let Some(region) = region else {
                host::fatal(Fatal::UntrackUnknownPage, chunk, 0);
            };
            let first = ((chunk - base) / PAGE_SIZE) as usize;
            let last = ((chunk_end - base) / PAGE_SIZE) as usize;
            self.update_region_watchers(region, base, first, last, track, watch, None, sink);
            chunk = chunk_end;
        }
    }

    pub fn update_page_watchers_for_region(&self, base: u64, mask: &Mask, track: bool, watch: Watch, sink: Option<&dyn DeferTarget>) {
        let start_range = mask.first_range();
        let end_range = mask.last_range();
        let first = start_range.0;
        let last = end_range.1;
        if start_range.1 == end_range.1 {
            self.update_page_watchers(base + first as u64 * PAGE_SIZE, (last - first) as u64 * PAGE_SIZE, track, watch, sink);
            return;
        }
        let region = if track { Some(self.get_or_create_region(base)) } else { self.find_region(base) };
        let Some(region) = region else {
            host::fatal(Fatal::UntrackUnknownRegion, base, 0);
        };
        self.update_region_watchers(region, base, first, last, track, watch, Some(mask), sink);
    }

    pub fn reapply_protection(&self, vaddr: u64, size: u64) {
        validate_range(vaddr, size);
        let begin = align_down(vaddr, PAGE_SIZE);
        let end = align_up(vaddr + size, PAGE_SIZE);
        let mut chunk = begin;
        while chunk < end {
            let chunk_end = end.min(align_up(chunk + 1, REGION_SIZE));
            let base = align_down(chunk, REGION_SIZE);
            if let Some(region) = self.find_region(chunk) {
                let pages = region.lock.lock();
                let first = ((chunk - base) / PAGE_SIZE) as usize;
                let last = ((chunk_end - base) / PAGE_SIZE) as usize;
                let mut run_begin = first;
                let mut run_perms = Mode::ReadWrite;
                let mut in_run = false;
                for page_index in first..=last {
                    let watched =
                        page_index < last && (pages[page_index].write_protected() || pages[page_index].access_watchers != 0);
                    let perms = if watched { pages[page_index].perms() } else { run_perms };
                    if in_run && (!watched || perms != run_perms) {
                        self.protect(base + run_begin as u64 * PAGE_SIZE, (page_index - run_begin) as u64 * PAGE_SIZE, run_perms);
                        in_run = false;
                    }
                    if watched && !in_run {
                        run_begin = page_index;
                        run_perms = perms;
                        in_run = true;
                    }
                }
            }
            chunk = chunk_end;
        }
    }

    pub fn has_read_watchers(&self, vaddr: u64, size: u64) -> bool {
        if !guest_range_valid(vaddr, size) {
            return false;
        }
        if !self.may_have_read_watchers(vaddr, vaddr + size - 1) {
            return false;
        }
        let end = align_down(vaddr + size - 1, PAGE_SIZE);
        let mut page = align_down(vaddr, PAGE_SIZE);
        loop {
            if let Some(region) = self.find_region(page) {
                if region.read_hint[((page % REGION_SIZE) / PAGE_SIZE) as usize].load(Ordering::Acquire) != 0 {
                    return true;
                }
            }
            if page == end {
                return false;
            }
            page += PAGE_SIZE;
        }
    }

    pub fn is_watched(&self, vaddr: u64) -> bool {
        let Some(region) = self.find_region(vaddr) else {
            return false;
        };
        let page = ((vaddr % REGION_SIZE) / PAGE_SIZE) as usize;
        let pages = region.lock.lock();
        pages[page].write_watchers != 0 || pages[page].access_watchers != 0
    }

    pub fn watched_span(&self, vaddr: u64, size: u64, first: &mut u64, end: &mut u64) -> bool {
        let mut any = false;
        let stop = if u64::MAX - vaddr < size { u64::MAX } else { vaddr + size };
        let mut at = align_down(vaddr, PAGE_SIZE);
        while at < stop && at < ADDRESS_SIZE {
            let until = stop.min((at / REGION_SIZE + 1) * REGION_SIZE);
            if let Some(region) = self.find_region(at) {
                let pages = region.lock.lock();
                let mut page = at;
                while page < until {
                    let state = &pages[((page % REGION_SIZE) / PAGE_SIZE) as usize];
                    if state.write_watchers != 0 || state.access_watchers != 0 {
                        if !any {
                            *first = page;
                        }
                        any = true;
                        *end = page + PAGE_SIZE;
                    }
                    page += PAGE_SIZE;
                }
            }
            at = until;
        }
        any
    }

    pub fn allow_execute(&self, vaddr: u64) -> bool {
        let Some(region) = self.find_region(vaddr) else {
            return false;
        };
        let page = ((vaddr % REGION_SIZE) / PAGE_SIZE) as usize;
        let pages = region.lock.lock();
        let perms = pages[page].perms();
        if perms == Mode::NoAccess {
            return false;
        }
        (host::get().protect)(align_down(vaddr, PAGE_SIZE), PAGE_SIZE, perms as u32 | 4)
    }

    fn mark_hot_granule(&self, page: u64) {
        let granule = page / HOT_GRANULE_SIZE;
        self.hot_granules[(granule / 64) as usize].fetch_or(1u64 << (granule % 64), Ordering::Release);
    }

    fn may_have_hot_pages(&self, vaddr: u64, size: u64) -> bool {
        if size == 0 || vaddr >= ADDRESS_SIZE {
            return false;
        }
        let last = (vaddr + size - 1).min(ADDRESS_SIZE - 1);
        let mut granule = vaddr / HOT_GRANULE_SIZE;
        while granule <= last / HOT_GRANULE_SIZE {
            if (self.hot_granules[(granule / 64) as usize].load(Ordering::Acquire) >> (granule % 64)) & 1 != 0 {
                return true;
            }
            granule += 1;
        }
        false
    }

    pub fn has_hot_pages(&self) -> bool {
        self.hot_any.load(Ordering::Acquire)
    }

    pub fn note_write_fault(&self, vaddr: u64) {
        self.counters.guest_write_faults.fetch_add(1, Ordering::Relaxed);
        if !self.hot_enabled || !guest_range_valid(vaddr, 1) {
            return;
        }
        let page = align_down(vaddr, PAGE_SIZE);
        let window = ((host::get().steady_ns)() / HOT_WINDOW_NS) as u8;
        let Some(region) = self.find_region(page) else {
            return;
        };
        let index = ((page % REGION_SIZE) / PAGE_SIZE) as usize;
        {
            let mut pages = region.lock.lock();
            let state = &mut pages[index];
            if state.hot != Hot::Cold || state.access_watchers != 0 {
                return;
            }
            if state.fault_window != window {
                state.fault_window = window;
                state.write_faults = 0;
            }
            state.write_faults = state.write_faults.wrapping_add(1);
            if state.write_faults < HOT_FAULTS {
                return;
            }
            state.write_faults = 0;
            if self.hot_count.load(Ordering::Relaxed) >= HOT_PAGE_CAP {
                return;
            }
            state.hot = Hot::Promoting;
        }
        if !(host::get().hot_watch)(page, page + PAGE_SIZE) {
            let mut pages = region.lock.lock();
            if pages[index].hot == Hot::Promoting {
                pages[index].hot = Hot::Refused;
            }
            self.counters.hot_refused.fetch_add(1, Ordering::Relaxed);
            return;
        }
        self.hot_count.fetch_add(1, Ordering::Relaxed);
        self.mark_hot_granule(page);
        self.pending.push(page);
        self.counters.hot_promoted.fetch_add(1, Ordering::Relaxed);
        self.hot_any.store(true, Ordering::Release);
        let mut pages = region.lock.lock();
        let state = &mut pages[index];
        if state.hot != Hot::Promoting {
            return;
        }
        state.hot = Hot::On;
        if state.write_watchers != 0 && state.access_watchers == 0 {
            self.protect(page, PAGE_SIZE, Mode::ReadWrite);
        }
    }

    fn merge_pending(&self, hot: &mut HotSet, now: u32) {
        let mut pending = Vec::new();
        self.pending.drain(&mut pending);
        for page in pending {
            let on = match self.find_region(page) {
                Some(region) => {
                    let pages = region.lock.lock();
                    let state = pages[((page % REGION_SIZE) / PAGE_SIZE) as usize].hot;
                    state == Hot::On || state == Hot::Promoting
                }
                None => false,
            };
            if on && !hot.pages.contains_key(&page) {
                hot.pages.insert(page, now);
                hot.runs_dirty = true;
            } else {
                self.hot_count.fetch_sub(1, Ordering::Relaxed);
            }
        }
    }

    fn rebuild_runs(hot: &mut HotSet) {
        hot.runs.clear();
        hot.clusters.clear();
        for &page in hot.pages.keys() {
            match hot.runs.last_mut() {
                Some(run) if run.1 == page => run.1 = page + PAGE_SIZE,
                _ => hot.runs.push((page, page + PAGE_SIZE)),
            }
        }
        for index in 0..hot.runs.len() {
            let run = hot.runs[index];
            match hot.clusters.last_mut() {
                Some(cluster) if run.0 - cluster.1 <= HOT_CLUSTER_GAP => cluster.1 = run.1,
                _ => hot.clusters.push(run),
            }
        }
        hot.runs_dirty = false;
    }

    fn demote(&self, page: u64, keep_protection: bool) {
        let Some(region) = self.find_region(page) else {
            return;
        };
        let mut pages = region.lock.lock();
        let state = &mut pages[((page % REGION_SIZE) / PAGE_SIZE) as usize];
        if state.hot != Hot::On && state.hot != Hot::Promoting {
            return;
        }
        state.hot = Hot::Cold;
        state.write_faults = 0;
        if keep_protection && state.write_protected() && state.access_watchers == 0 {
            self.protect(page, PAGE_SIZE, Mode::Read);
        }
    }

    fn log_hot_summary(&self, now: u64) {
        let counters = &self.counters;
        let last = counters.hot_last_line.load(Ordering::Relaxed);
        if last == 0 {
            let _ = counters.hot_last_line.compare_exchange(0, now, Ordering::Relaxed, Ordering::Relaxed);
            return;
        }
        if now.wrapping_sub(last) < 10_000_000_000
            || counters.hot_last_line.compare_exchange(last, now, Ordering::Relaxed, Ordering::Relaxed).is_err()
        {
            return;
        }
        let take = |value: &AtomicU64| value.swap(0, Ordering::Relaxed);
        let faults = take(&counters.guest_write_faults);
        let read_only = take(&counters.protects[0]);
        let writable = take(&counters.protects[1]);
        let no_access = take(&counters.protects[2]);
        let other = take(&counters.protects[3]);
        let scans = take(&counters.hot_scans);
        let scoped = take(&counters.hot_scoped_scans);
        let scan_ns = take(&counters.hot_scan_ns);
        let collected = take(&counters.hot_collected);
        let promoted = take(&counters.hot_promoted);
        let idle = take(&counters.hot_idle);
        let lost = take(&counters.hot_lost);
        let refused = take(&counters.hot_refused);
        let values = [
            now.wrapping_sub(last),
            faults,
            read_only,
            writable,
            no_access,
            other,
            counters.hot_page_total.load(Ordering::Relaxed),
            counters.hot_run_total.load(Ordering::Relaxed),
            scans,
            scoped,
            scan_ns,
            collected,
            promoted,
            idle,
            lost,
            refused,
        ];
        (host::get().hot_log)(values.as_ptr(), values.len());
    }

    pub fn collect_hot_page_writes(&self, written: &mut dyn FnMut(u64, u64), vaddr: u64, size: u64) {
        let scoped = size != 0;
        let host = host::get();
        if !scoped {
            self.log_hot_summary((host.steady_ns)());
        }
        if !self.hot_any.load(Ordering::Acquire) {
            return;
        }
        if scoped && (vaddr > u64::MAX - size || !self.may_have_hot_pages(vaddr, size)) {
            return;
        }
        let mut reported: Vec<u64> = Vec::new();
        let mut lost: Vec<u64> = Vec::new();
        let mut idle: Vec<u64> = Vec::new();
        let started = (host.steady_ns)();
        let now = (started / 1_000_000_000) as u32;
        let collect = |reported: &mut Vec<u64>, begin: u64, end: u64, strict: bool| -> bool {
            let before = reported.len();
            if (host.hot_collect)(begin, end, push_pages, reported as *mut Vec<u64> as *mut c_void, strict) {
                return true;
            }
            reported.truncate(before);
            false
        };
        {
            let mut guard = self.hot.lock().unwrap();
            let hot = &mut *guard;
            self.merge_pending(hot, now);
            let lose = |hot: &HotSet, lost: &mut Vec<u64>, begin: u64, end: u64| {
                for (&page, _) in hot.pages.range(begin..) {
                    if page >= end {
                        break;
                    }
                    lost.push(page);
                }
            };
            if scoped {
                let end = vaddr + size;
                let mut first = 0;
                let mut last = 0;
                for (&page, _) in hot.pages.range(align_down(vaddr, PAGE_SIZE)..) {
                    if page >= end {
                        break;
                    }
                    if last == 0 {
                        first = page;
                    }
                    last = page + PAGE_SIZE;
                }
                if last != 0 && !collect(&mut reported, first, last, false) {
                    lose(hot, &mut lost, first, last);
                }
            } else {
                if hot.runs_dirty {
                    Self::rebuild_runs(hot);
                }
                if now != hot.last_sweep {
                    for index in 0..hot.runs.len() {
                        let (begin, end) = hot.runs[index];
                        if !collect(&mut reported, begin, end, true) {
                            push_pages(&mut lost as *mut Vec<u64> as *mut c_void, begin, end);
                        }
                    }
                } else {
                    for index in 0..hot.clusters.len() {
                        let (begin, end) = hot.clusters[index];
                        if !collect(&mut reported, begin, end, false) {
                            lose(hot, &mut lost, begin, end);
                        }
                    }
                }
            }
            for page in &reported {
                if let Some(last) = hot.pages.get_mut(page) {
                    *last = now;
                }
            }
            if !scoped && now != hot.last_sweep {
                hot.last_sweep = now;
                for (&page, &last) in hot.pages.iter() {
                    if now.wrapping_sub(last) >= HOT_IDLE_SECONDS && idle.len() < HOT_IDLE_PER_SWEEP {
                        idle.push(page);
                    }
                }
            }
            for pages in [&lost, &idle] {
                let mut removed = 0u32;
                for page in pages.iter() {
                    if hot.pages.remove(page).is_some() {
                        removed += 1;
                    }
                }
                hot.runs_dirty |= removed != 0;
                self.hot_count.fetch_sub(removed, Ordering::Relaxed);
            }
            self.hot_any.store(!hot.pages.is_empty() || self.hot_count.load(Ordering::Relaxed) != 0, Ordering::Release);
            let counters = &self.counters;
            counters.hot_page_total.store(hot.pages.len() as u64, Ordering::Relaxed);
            if !scoped {
                counters.hot_run_total.store(hot.clusters.len() as u64, Ordering::Relaxed);
            }
            counters.hot_lost.fetch_add(lost.len() as u64, Ordering::Relaxed);
            counters.hot_idle.fetch_add(idle.len() as u64, Ordering::Relaxed);
            counters.hot_collected.fetch_add((reported.len() + lost.len()) as u64, Ordering::Relaxed);
            if scoped {
                counters.hot_scoped_scans.fetch_add(1, Ordering::Relaxed);
            } else {
                counters.hot_scans.fetch_add(1, Ordering::Relaxed);
            }
            counters.hot_scan_ns.fetch_add((host.steady_ns)().wrapping_sub(started), Ordering::Relaxed);
        }
        for &page in &lost {
            self.demote(page, true);
            (host.hot_unwatch)(page, page + PAGE_SIZE);
            reported.push(page);
        }
        for &page in &idle {
            self.demote(page, true);
            if !(host.hot_collect)(page, page + PAGE_SIZE, push_pages, &mut reported as *mut Vec<u64> as *mut c_void, true) {
                reported.push(page);
            }
            (host.hot_unwatch)(page, page + PAGE_SIZE);
        }
        if reported.is_empty() {
            return;
        }
        reported.sort_unstable();
        reported.dedup();
        let mut begin = reported[0];
        let mut end = begin + PAGE_SIZE;
        for &page in &reported[1..] {
            if page != end {
                written(begin, end);
                begin = page;
            }
            end = page + PAGE_SIZE;
        }
        written(begin, end);
    }

    pub fn forget_hot_pages(&self, vaddr: u64, size: u64) {
        if !self.hot_any.load(Ordering::Acquire) || size == 0 || vaddr > u64::MAX - size {
            return;
        }
        let pages = {
            let mut guard = self.hot.lock().unwrap();
            let hot = &mut *guard;
            self.merge_pending(hot, ((host::get().steady_ns)() / 1_000_000_000) as u32);
            let found: Vec<u64> = hot.pages.range(align_down(vaddr, PAGE_SIZE)..vaddr + size).map(|(&page, _)| page).collect();
            for page in &found {
                hot.pages.remove(page);
            }
            if found.is_empty() {
                return;
            }
            hot.runs_dirty = true;
            self.hot_count.fetch_sub(found.len() as u32, Ordering::Relaxed);
            self.hot_any.store(!hot.pages.is_empty() || self.hot_count.load(Ordering::Relaxed) != 0, Ordering::Release);
            found
        };
        for page in pages {
            self.demote(page, false);
            (host::get().hot_unwatch)(page, page + PAGE_SIZE);
        }
    }

    pub fn check_destroy(&self) {
        self.regions.for_each(|_, region| {
            let pages = region.lock.lock();
            for page in pages.iter() {
                if page.write_watchers != 0 || page.access_watchers != 0 {
                    host::fatal(Fatal::DestroyedWithLivePages, 0, 0);
                }
            }
        });
    }
}
