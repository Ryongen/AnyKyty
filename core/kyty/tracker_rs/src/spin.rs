use crate::host::{self, Fatal};
use crate::os;
use core::cell::{Cell, UnsafeCell};
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicU32, AtomicU64, AtomicU8, Ordering};
use std::time::Instant;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum LockClass {
    Other = 0,
    Tracker = 1,
    Pages = 2,
    Textures = 3,
}

pub struct Contention {
    waits: AtomicU64,
    ns: AtomicU64,
}

static CONTENTION: [Contention; 4] = [
    Contention { waits: AtomicU64::new(0), ns: AtomicU64::new(0) },
    Contention { waits: AtomicU64::new(0), ns: AtomicU64::new(0) },
    Contention { waits: AtomicU64::new(0), ns: AtomicU64::new(0) },
    Contention { waits: AtomicU64::new(0), ns: AtomicU64::new(0) },
];

pub fn take_contention(class: usize) -> (u64, u64) {
    let counters = &CONTENTION[class.min(3)];
    (counters.waits.swap(0, Ordering::Relaxed), counters.ns.swap(0, Ordering::Relaxed))
}

thread_local! {
    static THREAD: Cell<u32> = const { Cell::new(0) };
}

#[inline]
pub fn current_thread() -> u32 {
    THREAD.with(|cell| {
        let id = cell.get();
        if id != 0 {
            id
        } else {
            let id = os::thread_id();
            cell.set(id);
            id
        }
    })
}

const SPIN_ROUNDS: u32 = 4096;

pub struct SpinLock<T> {
    state: AtomicU32,
    owner: AtomicU32,
    sleepers: AtomicU32,
    class: AtomicU8,
    value: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for SpinLock<T> {}
unsafe impl<T: Send> Send for SpinLock<T> {}

pub struct Guard<'a, T> {
    lock: &'a SpinLock<T>,
    _not_send: PhantomData<*const ()>,
}

impl<T> SpinLock<T> {
    pub fn set_class(&self, class: LockClass) {
        self.class.store(class as u8, Ordering::Relaxed);
    }

    #[inline]
    pub fn lock(&self) -> Guard<'_, T> {
        let thread = current_thread();
        if self.state.swap(1, Ordering::Acquire) != 0 {
            self.lock_slow(thread);
        }
        self.owner.store(thread, Ordering::Relaxed);
        Guard { lock: self, _not_send: PhantomData }
    }

    #[cold]
    #[inline(never)]
    fn lock_slow(&self, thread: u32) {
        let own_core = (host::get().has_reserved_core)();
        let started = Instant::now();
        let mut round: u32 = 0;
        loop {
            if self.owner.load(Ordering::Relaxed) == thread {
                host::fatal(Fatal::LockReentered, thread as u64, 0);
            }
            if round < SPIN_ROUNDS || own_core {
                core::hint::spin_loop();
            } else {
                self.sleepers.fetch_add(1, Ordering::SeqCst);
                os::sleep_while_held(&self.state);
                self.sleepers.fetch_sub(1, Ordering::Relaxed);
            }
            if self.state.load(Ordering::Relaxed) == 0 && self.state.swap(1, Ordering::Acquire) == 0 {
                let counters = &CONTENTION[(self.class.load(Ordering::Relaxed) as usize).min(3)];
                counters.waits.fetch_add(1, Ordering::Relaxed);
                counters.ns.fetch_add(started.elapsed().as_nanos() as u64, Ordering::Relaxed);
                return;
            }
            round = round.wrapping_add(1);
        }
    }

    #[inline]
    fn unlock(&self) {
        if self.owner.load(Ordering::Relaxed) != current_thread() {
            host::fatal(Fatal::UnlockByOtherThread, 0, 0);
        }
        self.owner.store(0, Ordering::Relaxed);
        self.state.store(0, Ordering::SeqCst);
        if self.sleepers.load(Ordering::SeqCst) != 0 {
            os::wake_sleeper(&self.state);
        }
    }

    pub unsafe fn value_unlocked(&self) -> &mut T {
        unsafe { &mut *self.value.get() }
    }
}

impl<T> Deref for Guard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for Guard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for Guard<'_, T> {
    fn drop(&mut self) {
        self.lock.unlock();
    }
}
