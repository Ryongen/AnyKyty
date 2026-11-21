use crate::cards;
use crate::decode::{self, Refusal, MAX_LEN, REFUSAL_COUNT};
use crate::emit::{self, Tables, SLOT, SLOW_AT};
use crate::os;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicU8, AtomicUsize, Ordering};

pub const OUTCOME_NONE: u32 = 0;
pub const OUTCOME_PATCHED: u32 = 1;
pub const OUTCOME_SLOW_STORE: u32 = 2;
pub const OUTCOME_REFUSED: u32 = 3;
pub const OUTCOME_BUSY: u32 = 4;
pub const OUTCOME_INSIDE: u32 = 5;
pub const OUTCOME_REDIRECT: u32 = 6;

pub type NearAllocator = unsafe extern "C" fn(near: u64, bytes: u64) -> u64;
pub type GuestCode = unsafe extern "C" fn(address: u64) -> bool;

pub const ARENA_BYTES: usize = 256 * 1024;
const ARENA_SLOTS: u32 = (ARENA_BYTES / SLOT) as u32;
const ARENAS: usize = 64;
const SITE_SLOTS: usize = 1 << 15;
const PROBES: usize = 96;
const REFUSED: u64 = 1 << 63;
const REDIRECT: u64 = 1 << 62;
const REACH: u64 = 0x7FF0_0000;

static ENABLED: AtomicBool = AtomicBool::new(false);
static LOCK: AtomicBool = AtomicBool::new(false);
static PATCHING: AtomicU64 = AtomicU64::new(0);
static ALLOCATOR: AtomicUsize = AtomicUsize::new(0);
static GUEST: AtomicUsize = AtomicUsize::new(0);
static SITE_KEYS: [AtomicU64; SITE_SLOTS] = [const { AtomicU64::new(0) }; SITE_SLOTS];
static SITE_VALUES: [AtomicU64; SITE_SLOTS] = [const { AtomicU64::new(0) }; SITE_SLOTS];
static ARENA_BASE: [AtomicU64; ARENAS] = [const { AtomicU64::new(0) }; ARENAS];
static ARENA_USED: [AtomicU32; ARENAS] = [const { AtomicU32::new(0) }; ARENAS];
static ARENA_COUNT: AtomicUsize = AtomicUsize::new(0);
static WANTED: AtomicU64 = AtomicU64::new(0);
static UNREACHABLE: AtomicU64 = AtomicU64::new(0);

pub static PATCHED: AtomicU64 = AtomicU64::new(0);
pub static REFUSALS: [AtomicU64; REFUSAL_COUNT] = [const { AtomicU64::new(0) }; REFUSAL_COUNT];
pub static BUSY: AtomicU64 = AtomicU64::new(0);
pub static SLOW_FAULTS: AtomicU64 = AtomicU64::new(0);
pub static BREAKS: AtomicU64 = AtomicU64::new(0);
pub static REDIRECT_SITES: AtomicU64 = AtomicU64::new(0);
pub static REDIRECT_FAULTS: AtomicU64 = AtomicU64::new(0);

pub fn configure(allocator: Option<NearAllocator>, guest: Option<GuestCode>) {
    ALLOCATOR.store(allocator.map_or(0, |function| function as usize), Ordering::Release);
    GUEST.store(guest.map_or(0, |function| function as usize), Ordering::Release);
}

pub fn set_enabled(on: bool) {
    ENABLED.store(on, Ordering::Release);
}

pub fn enabled() -> bool {
    ENABLED.load(Ordering::Acquire)
}

fn slot_of(site: u64) -> usize {
    (site.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 49) as usize
}

fn site_get(site: u64) -> u64 {
    let mut at = slot_of(site);
    for _ in 0..PROBES {
        let key = SITE_KEYS[at].load(Ordering::Acquire);
        if key == site {
            return SITE_VALUES[at].load(Ordering::Acquire);
        }
        if key == 0 {
            return 0;
        }
        at = (at + 1) & (SITE_SLOTS - 1);
    }
    0
}

fn site_put(site: u64, value: u64) -> bool {
    let mut at = slot_of(site);
    for _ in 0..PROBES {
        let key = SITE_KEYS[at].load(Ordering::Acquire);
        if key == site {
            SITE_VALUES[at].store(value, Ordering::Release);
            return true;
        }
        if key == 0 {
            SITE_VALUES[at].store(value, Ordering::Release);
            SITE_KEYS[at].store(site, Ordering::Release);
            return true;
        }
        at = (at + 1) & (SITE_SLOTS - 1);
    }
    false
}

fn refuse(site: u64, refusal: Refusal) -> (u32, u64) {
    REFUSALS[refusal as usize].fetch_add(1, Ordering::Relaxed);
    site_put(site, REFUSED | refusal as u64);
    (OUTCOME_REFUSED, refusal as u64)
}

pub fn trampoline_of(address: u64) -> Option<(u64, bool)> {
    let count = ARENA_COUNT.load(Ordering::Acquire);
    for arena in 0..count {
        let base = ARENA_BASE[arena].load(Ordering::Acquire);
        let used = ARENA_USED[arena].load(Ordering::Acquire) as u64;
        if address < base || address >= base + used * SLOT as u64 {
            continue;
        }
        let entry = base + (address - base) / SLOT as u64 * SLOT as u64;
        let slow = unsafe { core::ptr::read((entry + SLOW_AT as u64) as *const u64) } & 0xFFFF;
        return Some((entry, address == entry + slow));
    }
    None
}

fn within_reach(site: u64, base: u64) -> bool {
    let low = base.min(site);
    let high = (base + ARENA_BYTES as u64).max(site + MAX_LEN as u64);
    high - low < REACH
}

fn slot_near(site: u64) -> Option<(usize, u64)> {
    let count = ARENA_COUNT.load(Ordering::Acquire);
    for arena in 0..count {
        let base = ARENA_BASE[arena].load(Ordering::Acquire);
        let used = ARENA_USED[arena].load(Ordering::Acquire);
        if used < ARENA_SLOTS && within_reach(site, base) {
            return Some((arena, base + used as u64 * SLOT as u64));
        }
    }
    let _ = WANTED.compare_exchange(0, site, Ordering::AcqRel, Ordering::Relaxed);
    None
}

pub fn service() {
    let site = WANTED.load(Ordering::Acquire);
    if site == 0 {
        return;
    }
    if LOCK.swap(true, Ordering::Acquire) {
        return;
    }
    let count = ARENA_COUNT.load(Ordering::Acquire);
    let allocator = ALLOCATOR.load(Ordering::Acquire);
    let mut served = false;
    for arena in 0..count {
        served |= ARENA_USED[arena].load(Ordering::Acquire) < ARENA_SLOTS && within_reach(site, ARENA_BASE[arena].load(Ordering::Acquire));
    }
    if !served && count < ARENAS && allocator != 0 {
        let allocator: NearAllocator = unsafe { core::mem::transmute(allocator) };
        let base = unsafe { allocator(site, ARENA_BYTES as u64) };
        if base != 0 && within_reach(site, base) {
            ARENA_BASE[count].store(base, Ordering::Release);
            ARENA_USED[count].store(0, Ordering::Release);
            ARENA_COUNT.store(count + 1, Ordering::Release);
            served = true;
        }
    }
    if !served {
        UNREACHABLE.store(site, Ordering::Release);
    }
    WANTED.store(0, Ordering::Release);
    LOCK.store(false, Ordering::Release);
}

unsafe fn poke(address: u64, byte: u8) {
    (*(address as *const AtomicU8)).store(byte, Ordering::SeqCst);
}

unsafe fn jumps_to(site: u64, entry: u64) -> bool {
    let code = site as *const u8;
    if core::ptr::read_volatile(code) != 0xE9 {
        return false;
    }
    let mut raw = [0u8; 4];
    for (offset, slot) in raw.iter_mut().enumerate() {
        *slot = core::ptr::read_volatile(code.add(1 + offset));
    }
    site.wrapping_add(5).wrapping_add(i32::from_le_bytes(raw) as i64 as u64) == entry
}

unsafe fn still_original(site: u64, entry: u64) -> bool {
    let word = core::ptr::read((entry + SLOW_AT as u64) as *const u64);
    let slow = entry + (word & 0xFFFF);
    let length = ((word >> 16) & 0xFF) as usize;
    (0..length).all(|offset| core::ptr::read_volatile((site as *const u8).add(offset)) == core::ptr::read((slow as *const u8).add(offset)))
}

unsafe fn known_site(site: u64) -> Option<(u32, u64)> {
    let known = site_get(site);
    if known & REFUSED != 0 {
        return Some((OUTCOME_REFUSED, known & !REFUSED));
    }
    if known & REDIRECT != 0 {
        let entry = known & !REDIRECT;
        return still_original(site, entry).then_some((OUTCOME_REDIRECT, entry));
    }
    if known != 0 {
        if jumps_to(site, known) {
            return Some((OUTCOME_PATCHED, known));
        }
        if PATCHING.load(Ordering::SeqCst) == site {
            return Some((OUTCOME_BUSY, 0));
        }
    }
    None
}

unsafe fn patch(site: u64) -> (u32, u64) {
    if let Some(found) = known_site(site) {
        return found;
    }
    let guest = GUEST.load(Ordering::Acquire);
    if guest != 0 {
        let guest: GuestCode = core::mem::transmute(guest);
        if !guest(site) {
            return refuse(site, Refusal::NotGuestCode);
        }
    }
    if !os::core_sync_ready() {
        return refuse(site, Refusal::NoCoreSync);
    }
    let store = match decode::decode_at(site as *const u8) {
        Ok(store) => store,
        Err(refusal) => return refuse(site, refusal),
    };
    let length = store.len as usize;
    let mut original = [0u8; MAX_LEN + 1];
    core::ptr::copy_nonoverlapping(site as *const u8, original.as_mut_ptr(), length);
    let tables = Tables { delta: cards::delta_base(), cards: cards::card_base(), pages: cards::pages() as u32 };
    if tables.pages == 0 {
        return (OUTCOME_NONE, 0);
    }
    let Some((arena, entry)) = slot_near(site) else {
        let unreachable = UNREACHABLE.load(Ordering::Acquire);
        if unreachable != 0 && site.abs_diff(unreachable) < REACH / 2 {
            return refuse(site, Refusal::OutOfReach);
        }
        return (OUTCOME_BUSY, 0);
    };
    let built = match emit::build(&store, &original[..length], site, entry, &tables) {
        Ok(built) => built,
        Err(refusal) => return refuse(site, refusal),
    };
    let jump = match emit::jump(site, entry) {
        Ok(jump) => jump,
        Err(refusal) => return refuse(site, refusal),
    };
    core::ptr::copy_nonoverlapping(built.bytes.as_ptr(), entry as *mut u8, SLOT);
    os::flush_code(entry, SLOT);
    ARENA_USED[arena].fetch_add(1, Ordering::Release);
    if length < decode::PATCH_LEN {
        if !site_put(site, REDIRECT | entry) {
            REFUSALS[Refusal::TableFull as usize].fetch_add(1, Ordering::Relaxed);
            return (OUTCOME_REFUSED, Refusal::TableFull as u64);
        }
        REDIRECT_SITES.fetch_add(1, Ordering::Relaxed);
        return (OUTCOME_REDIRECT, entry);
    }
    if !site_put(site, entry) {
        REFUSALS[Refusal::TableFull as usize].fetch_add(1, Ordering::Relaxed);
        return (OUTCOME_REFUSED, Refusal::TableFull as u64);
    }
    let Some(access) = os::open_code(site, length) else {
        return refuse(site, Refusal::CodeProtection);
    };
    PATCHING.store(site, Ordering::SeqCst);
    poke(site, 0xCC);
    os::sync_cores();
    for offset in 1..length {
        poke(site + offset as u64, jump[offset]);
    }
    os::sync_cores();
    poke(site, jump[0]);
    os::sync_cores();
    os::flush_code(site, length);
    os::close_code(site, length, access);
    PATCHING.store(0, Ordering::SeqCst);
    PATCHED.fetch_add(1, Ordering::Relaxed);
    (OUTCOME_PATCHED, entry)
}

pub unsafe fn lookup(address: u64) -> (u32, u64) {
    if let Some((entry, slow)) = trampoline_of(address) {
        if slow {
            SLOW_FAULTS.fetch_add(1, Ordering::Relaxed);
            return (OUTCOME_SLOW_STORE, entry);
        }
        return (OUTCOME_INSIDE, entry);
    }
    if let Some(found) = known_site(address) {
        if found.0 == OUTCOME_REDIRECT {
            REDIRECT_FAULTS.fetch_add(1, Ordering::Relaxed);
        }
        return found;
    }
    (OUTCOME_NONE, 0)
}

pub unsafe fn write_fault(address: u64) -> (u32, u64) {
    let found = lookup(address);
    if found.0 != OUTCOME_NONE || !enabled() {
        return found;
    }
    if LOCK.swap(true, Ordering::Acquire) {
        BUSY.fetch_add(1, Ordering::Relaxed);
        return (OUTCOME_BUSY, 0);
    }
    let outcome = patch(address);
    LOCK.store(false, Ordering::Release);
    outcome
}

pub fn breakpoint(address: u64) -> u64 {
    while PATCHING.load(Ordering::SeqCst) == address {
        os::relax();
    }
    let known = site_get(address);
    if known != 0 && known & REFUSED == 0 {
        BREAKS.fetch_add(1, Ordering::Relaxed);
        return address;
    }
    0
}

pub fn site_of(entry: u64) -> u64 {
    unsafe { core::ptr::read((entry + emit::SITE_AT as u64) as *const u64) }
}

pub unsafe fn target_of(entry: u64, registers: *const u64) -> u64 {
    let word = core::ptr::read((entry + SLOW_AT as u64) as *const u64);
    let Ok(store) = decode::decode_at((entry + (word & 0xFFFF)) as *const u8) else {
        return 0;
    };
    let mut address = (*registers.add(store.base as usize)).wrapping_add(store.disp as i64 as u64);
    if let Some(index) = store.index {
        address = address.wrapping_add((*registers.add(index as usize)).wrapping_mul(store.scale as u64));
    }
    address
}

pub fn width_of(entry: u64) -> u32 {
    (unsafe { core::ptr::read((entry + SLOW_AT as u64) as *const u64) } >> 24) as u32 & 0xFF
}
