use crate::os;
use core::sync::atomic::{AtomicI64, AtomicPtr, AtomicU64, AtomicU8, Ordering};

pub const REFUSED: i64 = 1;
const CHUNK: u64 = 64 * 1024;
const DELTA_CHUNKS: usize = 1 << 16;
const CARD_CHUNKS: usize = 1 << 13;

static DELTA: AtomicPtr<AtomicI64> = AtomicPtr::new(core::ptr::null_mut());
static CARDS: AtomicPtr<AtomicU8> = AtomicPtr::new(core::ptr::null_mut());
static PAGES: AtomicU64 = AtomicU64::new(0);
static DELTA_COMMITTED: [AtomicU8; DELTA_CHUNKS] = [const { AtomicU8::new(0) }; DELTA_CHUNKS];
static CARD_COMMITTED: [AtomicU8; CARD_CHUNKS] = [const { AtomicU8::new(0) }; CARD_CHUNKS];

pub fn pages() -> u64 {
    PAGES.load(Ordering::Acquire)
}

pub fn delta_base() -> u64 {
    DELTA.load(Ordering::Acquire) as u64
}

pub fn card_base() -> u64 {
    CARDS.load(Ordering::Acquire) as u64
}

pub fn init(limit: u64) -> bool {
    if pages() != 0 {
        return true;
    }
    let count = limit.div_ceil(os::PAGE);
    if count == 0 || count >= 1 << 31 || count * 8 > DELTA_CHUNKS as u64 * CHUNK || count > CARD_CHUNKS as u64 * CHUNK {
        return false;
    }
    let delta = os::reserve(count as usize * 8);
    if delta.is_null() {
        return false;
    }
    let cards = os::reserve(count as usize);
    if cards.is_null() {
        os::release(delta, count as usize * 8);
        return false;
    }
    DELTA.store(delta as *mut AtomicI64, Ordering::Release);
    CARDS.store(cards as *mut AtomicU8, Ordering::Release);
    PAGES.store(count, Ordering::Release);
    true
}

fn committed(flags: &[AtomicU8], base: u64, offset: u64) -> bool {
    if !os::lazy() {
        return true;
    }
    let chunk = (offset / CHUNK) as usize;
    if flags[chunk].load(Ordering::Acquire) != 0 {
        return true;
    }
    if !os::commit((base + chunk as u64 * CHUNK) as *mut u8, CHUNK as usize) {
        return false;
    }
    flags[chunk].store(1, Ordering::Release);
    true
}

pub fn table_fault(address: u64) -> bool {
    if !os::lazy() {
        return false;
    }
    let count = pages();
    let delta = delta_base();
    if count != 0 && address >= delta && address < delta + count * 8 {
        return committed(&DELTA_COMMITTED, delta, address - delta);
    }
    let cards = card_base();
    if count != 0 && address >= cards && address < cards + count {
        return committed(&CARD_COMMITTED, cards, address - cards);
    }
    false
}

pub fn arm(address: u64, delta: i64) -> bool {
    let page = address / os::PAGE;
    if page >= pages() || (delta != REFUSED && delta as u64 % os::PAGE != 0) {
        return false;
    }
    let table = delta_base();
    let cards = card_base();
    if !committed(&DELTA_COMMITTED, table, page * 8) || !committed(&CARD_COMMITTED, cards, page) {
        return false;
    }
    unsafe { &*(table as *const AtomicI64).add(page as usize) }.store(delta, Ordering::Release);
    true
}

pub fn armed(address: u64) -> i64 {
    let page = address / os::PAGE;
    if page >= pages() || (os::lazy() && DELTA_COMMITTED[(page * 8 / CHUNK) as usize].load(Ordering::Acquire) == 0) {
        return 0;
    }
    unsafe { &*(delta_base() as *const AtomicI64).add(page as usize) }.load(Ordering::Acquire)
}

pub fn mark(address: u64) -> bool {
    let page = address / os::PAGE;
    if page >= pages() || !committed(&CARD_COMMITTED, card_base(), page) {
        return false;
    }
    unsafe { &*(card_base() as *const AtomicU8).add(page as usize) }.store(1, Ordering::Release);
    true
}

fn readable(page: u64) -> bool {
    !os::lazy() || CARD_COMMITTED[(page / CHUNK) as usize].load(Ordering::Acquire) != 0
}

pub unsafe fn take(begin: u64, end: u64, runs: *mut u64, capacity: usize, next: *mut u64) -> usize {
    let count = pages();
    let cards = card_base() as *const AtomicU8;
    let last = end.div_ceil(os::PAGE).min(count);
    let mut page = begin / os::PAGE;
    let mut written = 0usize;
    let mut open = false;
    let mut start = 0u64;
    while page < last {
        if !readable(page) {
            if open {
                *runs.add(written * 2) = start * os::PAGE;
                *runs.add(written * 2 + 1) = page * os::PAGE;
                written += 1;
                open = false;
            }
            page = (page / CHUNK + 1) * CHUNK;
            continue;
        }
        if !open && written == capacity {
            *next = page * os::PAGE;
            return written;
        }
        if page % 8 == 0 && page + 8 <= last && (*(cards.add(page as usize) as *const AtomicU64)).load(Ordering::Relaxed) == 0 {
            if open {
                *runs.add(written * 2) = start * os::PAGE;
                *runs.add(written * 2 + 1) = page * os::PAGE;
                written += 1;
                open = false;
            }
            page += 8;
            continue;
        }
        let card = &*cards.add(page as usize);
        let set = card.load(Ordering::Relaxed) != 0 && card.swap(0, Ordering::SeqCst) != 0;
        if set && !open {
            open = true;
            start = page;
        } else if !set && open {
            *runs.add(written * 2) = start * os::PAGE;
            *runs.add(written * 2 + 1) = page * os::PAGE;
            written += 1;
            open = false;
        }
        page += 1;
    }
    if open {
        *runs.add(written * 2) = start * os::PAGE;
        *runs.add(written * 2 + 1) = page.min(last) * os::PAGE;
        written += 1;
    }
    *next = end;
    written
}

pub fn any(begin: u64, end: u64) -> bool {
    let count = pages();
    let cards = card_base() as *const AtomicU8;
    let last = end.div_ceil(os::PAGE).min(count);
    let mut page = begin / os::PAGE;
    while page < last {
        if !readable(page) {
            page = (page / CHUNK + 1) * CHUNK;
            continue;
        }
        if page % 8 == 0 && page + 8 <= last {
            if unsafe { &*(cards.add(page as usize) as *const AtomicU64) }.load(Ordering::Relaxed) != 0 {
                return true;
            }
            page += 8;
            continue;
        }
        if unsafe { &*cards.add(page as usize) }.load(Ordering::Relaxed) != 0 {
            return true;
        }
        page += 1;
    }
    false
}
