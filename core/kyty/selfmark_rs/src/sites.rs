use crate::cards;
use crate::decode::{self, Refusal, MAX_LEN, REFUSAL_COUNT};
use crate::emit::{self, Tables, PATCHED_AT, SLOT, SLOW_AT};
use crate::os;
use crate::pun::{self, Window, JUMP_LEN};
use core::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, AtomicUsize, Ordering};

pub const OUTCOME_NONE: u32 = 0;
pub const OUTCOME_PATCHED: u32 = 1;
pub const OUTCOME_SLOW_STORE: u32 = 2;
pub const OUTCOME_REFUSED: u32 = 3;
pub const OUTCOME_BUSY: u32 = 4;
pub const OUTCOME_INSIDE: u32 = 5;

pub type NearAllocator = unsafe extern "C" fn(near: u64, bytes: u64) -> u64;
pub type FixedAllocator = unsafe extern "C" fn(address: u64, bytes: *mut u64) -> u64;
pub type GuestCode = unsafe extern "C" fn(address: u64) -> bool;

pub const ARENA_BYTES: usize = 256 * 1024;
pub const NOTES: usize = 256;
const OWNER_NOTES: usize = 64;
const OWNER_KINDS: usize = 32;
const OWNERS: usize = 6;
const WRITERS: usize = 1024;
const ARENA_SLOTS: usize = ARENA_BYTES / SLOT;
const ARENA_WORDS: usize = ARENA_SLOTS.div_ceil(64);
const ARENAS: usize = 64;
const SITE_SLOTS: usize = 1 << 15;
const PROBES: usize = 96;
const REFUSED: u64 = 1 << 63;
const REACH: u64 = 0x7FF0_0000;
const REGIONS: usize = 4096;
const PUNS: usize = 1 << 14;
const CHAIN: usize = 12;
const SPOT_TRIES: usize = 2048;
const SPOT_STEP: u64 = 8;
const ADDRESS_BITS: u64 = 48;

static ENABLED: AtomicBool = AtomicBool::new(false);
static SHORT: AtomicBool = AtomicBool::new(true);
static LOCK: AtomicBool = AtomicBool::new(false);
static PATCHING: AtomicU64 = AtomicU64::new(0);
static ALLOCATOR: AtomicUsize = AtomicUsize::new(0);
static FIXED: AtomicUsize = AtomicUsize::new(0);
static GUEST: AtomicUsize = AtomicUsize::new(0);
static SITE_KEYS: [AtomicU64; SITE_SLOTS] = [const { AtomicU64::new(0) }; SITE_SLOTS];
static SITE_VALUES: [AtomicU64; SITE_SLOTS] = [const { AtomicU64::new(0) }; SITE_SLOTS];
static ARENA_BASE: [AtomicU64; ARENAS] = [const { AtomicU64::new(0) }; ARENAS];
static ARENA_MAP: [[AtomicU64; ARENA_WORDS]; ARENAS] = [const { [const { AtomicU64::new(0) }; ARENA_WORDS] }; ARENAS];
static ARENA_COUNT: AtomicUsize = AtomicUsize::new(0);
static REGION_BASE: [AtomicU64; REGIONS] = [const { AtomicU64::new(0) }; REGIONS];
static REGION_END: [AtomicU64; REGIONS] = [const { AtomicU64::new(0) }; REGIONS];
static REGION_NEXT: [AtomicU64; REGIONS] = [const { AtomicU64::new(0) }; REGIONS];
static REGION_COUNT: AtomicUsize = AtomicUsize::new(0);
static PUN_SITE: [AtomicU64; PUNS] = [const { AtomicU64::new(0) }; PUNS];
static PUN_HOP: [AtomicU64; PUNS] = [const { AtomicU64::new(0) }; PUNS];
static PUN_COUNT: AtomicUsize = AtomicUsize::new(0);
static WANTED: AtomicU64 = AtomicU64::new(0);

pub static PATCHED: AtomicU64 = AtomicU64::new(0);
pub static PUNNED: AtomicU64 = AtomicU64::new(0);
pub static AHEAD: AtomicU64 = AtomicU64::new(0);
pub static SHORT_OFF: AtomicU64 = AtomicU64::new(0);
pub static REFUSALS: [AtomicU64; REFUSAL_COUNT] = [const { AtomicU64::new(0) }; REFUSAL_COUNT];
pub static BUSY: AtomicU64 = AtomicU64::new(0);
pub static SLOW_FAULTS: AtomicU64 = AtomicU64::new(0);
pub static BREAKS: AtomicU64 = AtomicU64::new(0);
pub static NOTED: [AtomicU64; NOTES] = [const { AtomicU64::new(0) }; NOTES];
static WRITER: [AtomicU64; WRITERS] = [const { AtomicU64::new(0) }; WRITERS];

pub fn configure(allocator: Option<NearAllocator>, fixed: Option<FixedAllocator>, guest: Option<GuestCode>) {
    ALLOCATOR.store(allocator.map_or(0, |function| function as usize), Ordering::Release);
    FIXED.store(fixed.map_or(0, |function| function as usize), Ordering::Release);
    GUEST.store(guest.map_or(0, |function| function as usize), Ordering::Release);
}

pub fn set_enabled(on: bool) {
    ENABLED.store(on, Ordering::Release);
}

pub fn enabled() -> bool {
    ENABLED.load(Ordering::Acquire)
}

pub fn short_on() -> bool {
    SHORT.load(Ordering::Acquire)
}

pub fn hop_regions() -> u64 {
    REGION_COUNT.load(Ordering::Acquire) as u64
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

fn slot_used(arena: usize, index: usize) -> bool {
    ARENA_MAP[arena][index / 64].load(Ordering::Acquire) & (1 << (index % 64)) != 0
}

pub fn trampoline_of(address: u64) -> Option<(u64, bool)> {
    let count = ARENA_COUNT.load(Ordering::Acquire);
    for arena in 0..count {
        let base = ARENA_BASE[arena].load(Ordering::Acquire);
        if address < base || address >= base + (ARENA_SLOTS * SLOT) as u64 {
            continue;
        }
        let index = ((address - base) / SLOT as u64) as usize;
        if !slot_used(arena, index) {
            return None;
        }
        let entry = base + (index * SLOT) as u64;
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

fn slot_for(site: u64, also: u64, pads: usize, plain: bool) -> Option<(usize, usize, u64)> {
    let count = ARENA_COUNT.load(Ordering::Acquire);
    for pass in 0..3 {
        for arena in 0..count {
            let base = ARENA_BASE[arena].load(Ordering::Acquire);
            if !within_reach(site, base) || (also != 0 && !within_reach(also, base)) {
                continue;
            }
            for word in 0..ARENA_WORDS {
                let mut free = !ARENA_MAP[arena][word].load(Ordering::Acquire);
                while free != 0 {
                    let index = word * 64 + free.trailing_zeros() as usize;
                    free &= free - 1;
                    if index >= ARENA_SLOTS {
                        break;
                    }
                    let entry = base + (index * SLOT) as u64;
                    if plain || pass == 2 {
                        return Some((arena, index, entry));
                    }
                    let distance = entry.wrapping_sub(site + (pads + JUMP_LEN) as u64);
                    let second = pads != 0 || pun::good((distance >> 8) as u8);
                    if pun::good(distance as u8) && (second || pass == 1) {
                        return Some((arena, index, entry));
                    }
                }
            }
        }
        if plain {
            break;
        }
    }
    None
}

unsafe fn arena_near(site: u64) -> bool {
    let count = ARENA_COUNT.load(Ordering::Acquire);
    let allocator = ALLOCATOR.load(Ordering::Acquire);
    if count == ARENAS || allocator == 0 {
        return false;
    }
    let allocator: NearAllocator = core::mem::transmute(allocator);
    let base = allocator(site, ARENA_BYTES as u64);
    if base == 0 || !within_reach(site, base) {
        return false;
    }
    for word in ARENA_MAP[count].iter() {
        word.store(0, Ordering::Release);
    }
    ARENA_BASE[count].store(base, Ordering::Release);
    ARENA_COUNT.store(count + 1, Ordering::Release);
    true
}

fn mapped(address: u64) -> bool {
    let count = REGION_COUNT.load(Ordering::Acquire);
    (0..count).any(|region| address >= REGION_BASE[region].load(Ordering::Acquire) && address < REGION_END[region].load(Ordering::Acquire))
}

fn hop_clear(target: u64) -> bool {
    let count = PUN_COUNT.load(Ordering::Acquire);
    (0..count).all(|pun| {
        let hop = PUN_HOP[pun].load(Ordering::Acquire);
        hop >= target + JUMP_LEN as u64 || target >= hop + JUMP_LEN as u64
    })
}

fn usable(target: u64) -> bool {
    mapped(target) && mapped(target + JUMP_LEN as u64 - 1) && hop_clear(target)
}

unsafe fn map_for(target: u64) -> bool {
    for address in [target, target + JUMP_LEN as u64 - 1] {
        if mapped(address) {
            continue;
        }
        let fixed = FIXED.load(Ordering::Acquire);
        let count = REGION_COUNT.load(Ordering::Acquire);
        if fixed == 0 || count == REGIONS {
            return false;
        }
        let fixed: FixedAllocator = core::mem::transmute(fixed);
        let mut bytes = 0u64;
        let base = fixed(address & !(os::PAGE - 1), &mut bytes);
        if base == 0 || address < base || address - base >= bytes {
            return false;
        }
        REGION_BASE[count].store(base, Ordering::Release);
        REGION_END[count].store(base + bytes, Ordering::Release);
        REGION_NEXT[count].store(base, Ordering::Release);
        REGION_COUNT.store(count + 1, Ordering::Release);
    }
    true
}

fn lows(bytes: u64) -> u64 {
    (0..bytes).fold(0, |value, index| value | 0x08 << (8 * index))
}

fn offset_good(offset: u64, free: u8) -> bool {
    (0..free as u64).all(|index| pun::good((offset >> (8 * index)) as u8))
}

fn next_good(mut offset: u64, free: u8) -> u64 {
    loop {
        let mut moved = false;
        for index in (0..free as u64).rev() {
            let shift = 8 * index;
            let byte = (offset >> shift) as u8;
            if byte < 0x08 {
                offset = (offset >> (shift + 8) << (shift + 8)) | lows(index + 1);
                moved = true;
                break;
            }
            if byte >= 0x80 {
                offset = (((offset >> (shift + 8)) + 1) << (shift + 8)) | lows(index + 1);
                moved = true;
                break;
            }
        }
        if !moved {
            return offset;
        }
    }
}

enum Spot {
    At(u64),
    Wanted,
    Nowhere,
}

unsafe fn spot(window: &Window, alloc: bool) -> Spot {
    if window.free == 0 {
        if usable(window.low) {
            return Spot::At(window.low);
        }
        if !hop_clear(window.low) {
            return Spot::Nowhere;
        }
        if !alloc {
            return Spot::Wanted;
        }
        return if map_for(window.low) && usable(window.low) { Spot::At(window.low) } else { Spot::Nowhere };
    }
    let end = window.low + window.count;
    let regions = REGION_COUNT.load(Ordering::Acquire);
    let mut tries = 0;
    for region in 0..regions {
        let from = REGION_BASE[region].load(Ordering::Acquire).max(window.low);
        let to = REGION_END[region].load(Ordering::Acquire).min(end);
        if from >= to {
            continue;
        }
        let mut target = from.max(REGION_NEXT[region].load(Ordering::Acquire));
        let mut packed = target == REGION_NEXT[region].load(Ordering::Acquire);
        while target < to && tries < SPOT_TRIES {
            let offset = target - window.low;
            if !offset_good(offset, window.free) {
                target = window.low + next_good(offset, window.free);
                packed = false;
                continue;
            }
            tries += 1;
            if usable(target) {
                if packed {
                    REGION_NEXT[region].store(target + SPOT_STEP, Ordering::Release);
                }
                return Spot::At(target);
            }
            target += SPOT_STEP;
        }
    }
    if !alloc {
        return Spot::Wanted;
    }
    for top in [0x10u64, 0x30, 0x50, 0x70, 0x20, 0x40, 0x60] {
        let free = window.free as u64;
        let offset = (lows(free - 1) << 1) | top << (8 * (free - 1));
        let target = window.low + offset;
        if hop_clear(target) && map_for(target) && usable(target) {
            return Spot::At(target);
        }
    }
    Spot::Nowhere
}

fn pun_fields(packed: u64) -> (u64, usize, usize) {
    (packed & ((1 << ADDRESS_BITS) - 1), ((packed >> ADDRESS_BITS) & 0xF) as usize, ((packed >> (ADDRESS_BITS + 4)) & 0xF) as usize)
}

fn frozen(begin: u64, bytes: u64) -> bool {
    let count = PUN_COUNT.load(Ordering::Acquire);
    (0..count).any(|pun| {
        let (site, length, pads) = pun_fields(PUN_SITE[pun].load(Ordering::Acquire));
        begin < site + (pads + JUMP_LEN) as u64 && site + (length as u64) < begin + bytes
    })
}

unsafe fn poke(address: u64, byte: u8) {
    (*(address as *const AtomicU8)).store(byte, Ordering::SeqCst);
}

unsafe fn peek(address: u64) -> u8 {
    core::ptr::read_volatile(address as *const u8)
}

unsafe fn jump_at(address: u64, pads_allowed: usize) -> Option<u64> {
    let mut pads = 0;
    let mut byte = peek(address);
    while byte == pun::PAD && pads < pads_allowed {
        pads += 1;
        byte = peek(address + pads as u64);
    }
    if byte != pun::JUMP {
        return None;
    }
    let mut raw = [0u8; 4];
    for (offset, slot) in raw.iter_mut().enumerate() {
        *slot = peek(address + (pads + 1 + offset) as u64);
    }
    Some(address.wrapping_add((pads + JUMP_LEN) as u64).wrapping_add(i32::from_le_bytes(raw) as i64 as u64))
}

unsafe fn jumps_to(site: u64, entry: u64) -> bool {
    let Some(target) = jump_at(site, pun::MAX_PADS) else {
        return false;
    };
    if target == entry {
        return true;
    }
    mapped(target) && mapped(target + JUMP_LEN as u64 - 1) && jump_at(target, 0) == Some(entry)
}

unsafe fn rewrite(site: u64, length: usize, bytes: &[u8]) -> bool {
    let Some(access) = os::open_code(site, length) else {
        return false;
    };
    PATCHING.store(site, Ordering::SeqCst);
    poke(site, 0xCC);
    os::sync_cores();
    for offset in 1..length {
        poke(site + offset as u64, bytes[offset]);
    }
    os::sync_cores();
    poke(site, bytes[0]);
    os::sync_cores();
    os::flush_code(site, length);
    os::close_code(site, length, access);
    PATCHING.store(0, Ordering::SeqCst);
    true
}

unsafe fn is_guest(address: u64) -> bool {
    let guest = GUEST.load(Ordering::Acquire);
    if guest == 0 {
        return true;
    }
    let guest: GuestCode = core::mem::transmute(guest);
    guest(address)
}

unsafe fn patch_one(site: u64, alloc: bool) -> (u32, u64) {
    let known = site_get(site);
    if known & REFUSED != 0 {
        return (OUTCOME_REFUSED, known & !REFUSED);
    }
    if known != 0 && jumps_to(site, known) {
        return (OUTCOME_PATCHED, known);
    }
    if !is_guest(site) {
        return refuse(site, Refusal::NotGuestCode);
    }
    if !os::core_sync_ready() {
        return refuse(site, Refusal::NoCoreSync);
    }
    let store = match decode::decode_at(site as *const u8) {
        Ok(store) => store,
        Err(refusal) => return refuse(site, refusal),
    };
    let length = store.len as usize;
    let short = store.short();
    if short && !short_on() {
        return refuse(site, Refusal::Short);
    }
    if frozen(site, length as u64) {
        return refuse(site, Refusal::Frozen);
    }
    let mut original = [0u8; MAX_LEN + 1];
    core::ptr::copy_nonoverlapping(site as *const u8, original.as_mut_ptr(), length);
    let tables = Tables { delta: cards::delta_base(), cards: cards::card_base(), pages: cards::pages() as u32 };
    if tables.pages == 0 {
        return (OUTCOME_NONE, 0);
    }
    let mut after = [0u8; 4];
    let mut borrowable = 0;
    let mut pads = pun::pads_for(length.max(JUMP_LEN));
    let mut hop = 0u64;
    let mut slot = slot_for(site, 0, pads, short);
    if slot.is_none() {
        if !alloc {
            return (OUTCOME_BUSY, 0);
        }
        if arena_near(site) {
            slot = slot_for(site, 0, pads, short);
        }
        if slot.is_none() {
            return refuse(site, Refusal::OutOfReach);
        }
    }
    if short {
        let next = site + length as u64;
        while borrowable < after.len() && (next + borrowable as u64) >> 12 == next >> 12 {
            after[borrowable] = peek(next + borrowable as u64);
            borrowable += 1;
        }
        if PUN_COUNT.load(Ordering::Acquire) == PUNS {
            return refuse(site, Refusal::TableFull);
        }
        slot = None;
        for candidate in 0..length {
            let Some(window) = pun::window(site, length, candidate, &after[..borrowable]) else {
                continue;
            };
            if slot_for(site, window.low, 0, true).is_none() && slot_for(site, window.low + window.count - 1, 0, true).is_none() {
                continue;
            }
            match spot(&window, alloc) {
                Spot::At(target) => {
                    slot = slot_for(site, target, 0, true);
                    if slot.is_some() {
                        pads = candidate;
                        hop = target;
                        break;
                    }
                }
                Spot::Wanted => return (OUTCOME_BUSY, 0),
                Spot::Nowhere => {}
            }
        }
    }
    let Some((arena, index, entry)) = slot else {
        return refuse(site, Refusal::NoWindow);
    };
    let mut built = match emit::build(&store, &original[..length], site, entry, &tables) {
        Ok(built) => built,
        Err(refusal) => return refuse(site, refusal),
    };
    let mut bytes = [0xCCu8; 16];
    let mut relay = [0u8; JUMP_LEN];
    if short {
        let (Some(punned), Some(jump)) = (pun::encode(site, length, pads, &after[..borrowable], hop), pun::hop(hop, entry)) else {
            return refuse(site, Refusal::OutOfReach);
        };
        bytes[..4].copy_from_slice(&punned);
        relay = jump;
        built.bytes[PATCHED_AT..PATCHED_AT + 4].copy_from_slice(&punned);
    } else {
        let Some(jump) = pun::long(site, length, pads, entry) else {
            return refuse(site, Refusal::OutOfReach);
        };
        bytes = jump;
    }
    core::ptr::copy_nonoverlapping(built.bytes.as_ptr(), entry as *mut u8, SLOT);
    os::flush_code(entry, SLOT);
    ARENA_MAP[arena][index / 64].fetch_or(1 << (index % 64), Ordering::AcqRel);
    if !site_put(site, entry) {
        REFUSALS[Refusal::TableFull as usize].fetch_add(1, Ordering::Relaxed);
        return (OUTCOME_REFUSED, Refusal::TableFull as u64);
    }
    if short {
        core::ptr::copy_nonoverlapping(relay.as_ptr(), hop as *mut u8, JUMP_LEN);
        os::flush_code(hop, JUMP_LEN);
        let pun = PUN_COUNT.load(Ordering::Acquire);
        PUN_HOP[pun].store(hop, Ordering::Release);
        PUN_SITE[pun].store(site | (length as u64) << ADDRESS_BITS | (pads as u64) << (ADDRESS_BITS + 4), Ordering::Release);
        PUN_COUNT.store(pun + 1, Ordering::Release);
    }
    if !rewrite(site, length, &bytes) {
        return refuse(site, Refusal::CodeProtection);
    }
    PATCHED.fetch_add(1, Ordering::Relaxed);
    if short {
        PUNNED.fetch_add(1, Ordering::Relaxed);
    }
    (OUTCOME_PATCHED, entry)
}

unsafe fn settled(site: u64) -> bool {
    let known = site_get(site);
    known & REFUSED != 0 || (known != 0 && jumps_to(site, known))
}

unsafe fn patch(site: u64, alloc: bool) -> (u32, u64) {
    let mut chain = [0u64; CHAIN];
    let mut count = 1;
    chain[0] = site;
    if short_on() && !settled(site) && is_guest(site) {
        let mut cursor = site;
        while count < CHAIN {
            let Ok(store) = decode::decode_at(cursor as *const u8) else {
                break;
            };
            if !store.short() {
                break;
            }
            let next = cursor + store.len as u64;
            if settled(next) || !is_guest(next) || decode::decode_at(next as *const u8).is_err() {
                break;
            }
            chain[count] = next;
            count += 1;
            cursor = next;
        }
    }
    let mut outcome = (OUTCOME_NONE, 0);
    for index in (0..count).rev() {
        outcome = patch_one(chain[index], alloc);
        if outcome.0 == OUTCOME_BUSY {
            return outcome;
        }
        if index != 0 && outcome.0 == OUTCOME_PATCHED {
            AHEAD.fetch_add(1, Ordering::Relaxed);
        }
    }
    outcome
}

pub unsafe fn service() {
    let site = WANTED.load(Ordering::Acquire);
    if site == 0 {
        return;
    }
    if LOCK.swap(true, Ordering::Acquire) {
        return;
    }
    if patch(site, true).0 == OUTCOME_BUSY {
        refuse(site, Refusal::OutOfReach);
    }
    WANTED.store(0, Ordering::Release);
    LOCK.store(false, Ordering::Release);
}

pub unsafe fn set_short(on: bool) {
    while LOCK.swap(true, Ordering::Acquire) {
        os::relax();
    }
    if SHORT.swap(on, Ordering::AcqRel) != on {
        if on {
            for value in SITE_VALUES.iter() {
                let _ = value.compare_exchange(REFUSED | Refusal::Short as u64, 0, Ordering::AcqRel, Ordering::Relaxed);
            }
        }
        let count = PUN_COUNT.load(Ordering::Acquire);
        for step in 0..count {
            let pun = if on { step } else { count - 1 - step };
            let (site, length, _) = pun_fields(PUN_SITE[pun].load(Ordering::Acquire));
            let entry = site_get(site);
            if entry == 0 || entry & REFUSED != 0 {
                continue;
            }
            let mut bytes = [0u8; 4];
            let from = if on { entry + PATCHED_AT as u64 } else { entry + (core::ptr::read((entry + SLOW_AT as u64) as *const u64) & 0xFFFF) };
            core::ptr::copy_nonoverlapping(from as *const u8, bytes.as_mut_ptr(), length);
            rewrite(site, length, &bytes);
        }
    }
    LOCK.store(false, Ordering::Release);
}

pub unsafe fn lookup(address: u64) -> (u32, u64) {
    if let Some((entry, slow)) = trampoline_of(address) {
        if slow {
            SLOW_FAULTS.fetch_add(1, Ordering::Relaxed);
            return (OUTCOME_SLOW_STORE, entry);
        }
        return (OUTCOME_INSIDE, entry);
    }
    let known = site_get(address);
    if known & REFUSED != 0 {
        return (OUTCOME_REFUSED, known & !REFUSED);
    }
    if known != 0 {
        if jumps_to(address, known) {
            return (OUTCOME_PATCHED, known);
        }
        if PATCHING.load(Ordering::SeqCst) == address {
            return (OUTCOME_BUSY, 0);
        }
        if !short_on() && (length_of(known) as usize) < JUMP_LEN {
            SHORT_OFF.fetch_add(1, Ordering::Relaxed);
            return (OUTCOME_REFUSED, Refusal::Short as u64);
        }
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
    let outcome = patch(address, false);
    LOCK.store(false, Ordering::Release);
    if outcome.0 == OUTCOME_BUSY {
        let _ = WANTED.compare_exchange(0, address, Ordering::AcqRel, Ordering::Relaxed);
    }
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

pub fn length_of(entry: u64) -> u32 {
    (unsafe { core::ptr::read((entry + SLOW_AT as u64) as *const u64) } >> 16) as u32 & 0xFF
}

pub fn note(kind: usize) {
    if kind < NOTES {
        NOTED[kind].fetch_add(1, Ordering::Relaxed);
    }
}

pub fn set_writer(kind: u32) {
    let thread = os::thread_id();
    WRITER[thread as usize % WRITERS].store(thread << 8 | (kind as u64 & 0xFF), Ordering::Relaxed);
}

pub fn note_owner(owner: usize) {
    let thread = os::thread_id();
    let slot = WRITER[thread as usize % WRITERS].load(Ordering::Relaxed);
    let kind = if slot >> 8 == thread { (slot & 0xFF) as usize } else { 0 };
    if owner < OWNERS && kind < OWNER_KINDS {
        NOTED[OWNER_NOTES + owner * OWNER_KINDS + kind].fetch_add(1, Ordering::Relaxed);
    }
}
