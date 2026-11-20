use crate::host::{self, Fatal};
use crate::os;
use core::marker::PhantomData;
use core::sync::atomic::{AtomicU64, AtomicU8, Ordering};

pub unsafe trait Zeroable {}

const EMPTY: u8 = 0;
const BUILDING: u8 = 1;
const READY: u8 = 2;

pub struct Arena<T: Zeroable> {
    base: *mut u8,
    stride: usize,
    count: usize,
    states: Box<[AtomicU8]>,
    created: Box<[AtomicU64]>,
    _marker: PhantomData<T>,
}

unsafe impl<T: Zeroable + Send + Sync> Sync for Arena<T> {}
unsafe impl<T: Zeroable + Send + Sync> Send for Arena<T> {}

fn zeroed_slice<A>(count: usize) -> Box<[A]> {
    let layout = std::alloc::Layout::array::<A>(count).unwrap();
    unsafe {
        let memory = std::alloc::alloc_zeroed(layout) as *mut A;
        if memory.is_null() {
            std::alloc::handle_alloc_error(layout);
        }
        Box::from_raw(core::ptr::slice_from_raw_parts_mut(memory, count))
    }
}

impl<T: Zeroable> Arena<T> {
    pub fn new(count: usize) -> Self {
        let stride = core::mem::size_of::<T>().next_multiple_of(core::mem::align_of::<T>().max(64));
        let base = os::reserve(stride * count);
        if base.is_null() {
            host::fatal(Fatal::ReservationFailed, (stride * count) as u64, 0);
        }
        Arena {
            base,
            stride,
            count,
            states: zeroed_slice(count),
            created: zeroed_slice(count.div_ceil(64)),
            _marker: PhantomData,
        }
    }

    #[inline]
    fn slot(&self, index: usize) -> &T {
        unsafe { &*(self.base.add(index * self.stride) as *const T) }
    }

    #[inline]
    pub fn find(&self, index: usize) -> Option<&T> {
        if self.states[index].load(Ordering::Acquire) == READY {
            Some(self.slot(index))
        } else {
            None
        }
    }

    pub fn get_or_create(&self, index: usize, init: impl FnOnce(&T)) -> &T {
        let state = &self.states[index];
        if state.load(Ordering::Acquire) == READY {
            return self.slot(index);
        }
        if state.compare_exchange(EMPTY, BUILDING, Ordering::AcqRel, Ordering::Acquire).is_ok() {
            let memory = unsafe { self.base.add(index * self.stride) };
            if !os::commit(memory, self.stride) {
                host::fatal(Fatal::ReservationFailed, self.stride as u64, index as u64);
            }
            init(self.slot(index));
            self.created[index / 64].fetch_or(1u64 << (index % 64), Ordering::Release);
            state.store(READY, Ordering::Release);
        } else {
            while state.load(Ordering::Acquire) != READY {
                core::hint::spin_loop();
            }
        }
        self.slot(index)
    }

    pub fn for_each(&self, mut f: impl FnMut(usize, &T)) {
        for (word_index, word) in self.created.iter().enumerate() {
            let mut bits = word.load(Ordering::Acquire);
            while bits != 0 {
                let index = word_index * 64 + bits.trailing_zeros() as usize;
                bits &= bits - 1;
                if self.states[index].load(Ordering::Acquire) == READY {
                    f(index, self.slot(index));
                }
            }
        }
    }
}

impl<T: Zeroable> Drop for Arena<T> {
    fn drop(&mut self) {
        let mut ready = Vec::new();
        self.for_each(|index, _| ready.push(index));
        for index in ready {
            unsafe { core::ptr::drop_in_place(self.base.add(index * self.stride) as *mut T) };
        }
        os::release(self.base, self.stride * self.count);
    }
}
