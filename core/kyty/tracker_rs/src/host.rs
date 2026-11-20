use core::ffi::c_void;
use core::sync::atomic::{AtomicPtr, Ordering};

pub type RangeFn = extern "C" fn(*mut c_void, u64, u64);
pub type IndexedRangeFn = extern "C" fn(*mut c_void, usize, u64, u64);
pub type VoidFn = extern "C" fn(*mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Host {
    pub protect: extern "C" fn(u64, u64, u32) -> bool,
    pub bda_mark: extern "C" fn(u64),
    pub defer_protect: extern "C" fn(*const c_void, u64, u64),
    pub has_reserved_core: extern "C" fn() -> bool,
    pub steady_ns: extern "C" fn() -> u64,
    pub fatal: extern "C" fn(u32, u64, u64),
    pub hot_watch: extern "C" fn(u64, u64) -> bool,
    pub hot_unwatch: extern "C" fn(u64, u64),
    pub hot_collect: extern "C" fn(u64, u64, RangeFn, *mut c_void, bool) -> bool,
    pub hot_log: extern "C" fn(*const u64, usize),
}

static HOST: AtomicPtr<Host> = AtomicPtr::new(core::ptr::null_mut());

pub fn set(host: &Host) {
    let boxed = Box::into_raw(Box::new(*host));
    HOST.store(boxed, Ordering::Release);
}

#[inline]
pub fn get() -> &'static Host {
    let host = HOST.load(Ordering::Acquire);
    if host.is_null() {
        std::process::abort();
    }
    unsafe { &*host }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum Fatal {
    TrackerInvalidRange = 1,
    TrackerInvalidRegion = 2,
    CpuDirtyConflictsGpuDirty = 3,
    LockReentered = 4,
    UnlockByOtherThread = 5,
    PagesInvalidRange = 6,
    ReadWatcherOverflow = 7,
    ReadWatcherUnderflow = 8,
    WriteWatcherOverflow = 9,
    WriteWatcherUnderflow = 10,
    UntrackUnknownPage = 11,
    UntrackUnknownRegion = 12,
    ProtectFailed = 13,
    DestroyedWithLivePages = 14,
    ReservationFailed = 15,
}

#[cold]
#[inline(never)]
pub fn fatal(code: Fatal, a: u64, b: u64) -> ! {
    (get().fatal)(code as u32, a, b);
    std::process::abort()
}
