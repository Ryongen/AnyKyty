use crate::cards;
use crate::decode::REFUSAL_COUNT;
use crate::sites::{self, GuestCode, NearAllocator};
use core::sync::atomic::Ordering;

#[repr(C)]
pub struct KytySelfMarkFault {
    pub outcome: u32,
    pub reason: u32,
    pub entry: u64,
    pub width: u32,
}

#[repr(C)]
pub struct KytySelfMarkStats {
    pub patched: u64,
    pub busy: u64,
    pub slow_faults: u64,
    pub breaks: u64,
    pub redirect_sites: u64,
    pub redirect_faults: u64,
    pub refusals: [u64; REFUSAL_COUNT],
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_init(guest_limit: u64, allocator: Option<NearAllocator>, guest: Option<GuestCode>) -> bool {
    if !cards::init(guest_limit) {
        return false;
    }
    sites::configure(allocator, guest);
    true
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_set_enabled(on: bool) {
    sites::set_enabled(on && cards::pages() != 0);
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_enabled() -> bool {
    sites::enabled()
}

fn fault(outcome: u32, value: u64) -> KytySelfMarkFault {
    match outcome {
        sites::OUTCOME_REFUSED => KytySelfMarkFault { outcome, reason: value as u32, entry: 0, width: 0 },
        sites::OUTCOME_PATCHED | sites::OUTCOME_SLOW_STORE | sites::OUTCOME_INSIDE | sites::OUTCOME_REDIRECT => KytySelfMarkFault { outcome, reason: 0, entry: value, width: sites::width_of(value) },
        _ => KytySelfMarkFault { outcome, reason: 0, entry: value, width: 0 },
    }
}

#[no_mangle]
pub unsafe extern "C" fn kyty_selfmark_write_fault(address: u64) -> KytySelfMarkFault {
    let (outcome, value) = sites::write_fault(address);
    fault(outcome, value)
}

#[no_mangle]
pub unsafe extern "C" fn kyty_selfmark_lookup(address: u64) -> KytySelfMarkFault {
    let (outcome, value) = sites::lookup(address);
    fault(outcome, value)
}

#[no_mangle]
pub unsafe extern "C" fn kyty_selfmark_target(entry: u64, registers: *const u64) -> u64 {
    sites::target_of(entry, registers)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_site(entry: u64) -> u64 {
    sites::site_of(entry)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_service() {
    sites::service()
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_breakpoint(address: u64) -> u64 {
    sites::breakpoint(address)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_table_fault(address: u64) -> bool {
    cards::table_fault(address)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_arm(address: u64, delta: i64) -> bool {
    cards::arm(address, delta)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_armed(address: u64) -> i64 {
    cards::armed(address)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_mark(address: u64) -> bool {
    cards::mark(address)
}

#[no_mangle]
pub unsafe extern "C" fn kyty_selfmark_take(begin: u64, end: u64, runs: *mut u64, capacity: usize, next: *mut u64) -> usize {
    cards::take(begin, end, runs, capacity, next)
}

#[no_mangle]
pub extern "C" fn kyty_selfmark_any(begin: u64, end: u64) -> bool {
    cards::any(begin, end)
}

#[no_mangle]
pub unsafe extern "C" fn kyty_selfmark_stats(stats: *mut KytySelfMarkStats) {
    let stats = &mut *stats;
    stats.patched = sites::PATCHED.load(Ordering::Relaxed);
    stats.busy = sites::BUSY.load(Ordering::Relaxed);
    stats.slow_faults = sites::SLOW_FAULTS.load(Ordering::Relaxed);
    stats.breaks = sites::BREAKS.load(Ordering::Relaxed);
    stats.redirect_sites = sites::REDIRECT_SITES.load(Ordering::Relaxed);
    stats.redirect_faults = sites::REDIRECT_FAULTS.load(Ordering::Relaxed);
    for (slot, counter) in stats.refusals.iter_mut().zip(sites::REFUSALS.iter()) {
        *slot = counter.load(Ordering::Relaxed);
    }
}
