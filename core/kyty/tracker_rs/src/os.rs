#[cfg(unix)]
mod imp {
    use core::ffi::c_void;

    extern "C" {
        fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, offset: i64) -> *mut c_void;
        fn munmap(addr: *mut c_void, len: usize) -> i32;
        fn syscall(number: i64, ...) -> i64;
        fn sched_yield() -> i32;
    }

    const PROT_READ: i32 = 1;
    const PROT_WRITE: i32 = 2;
    const MAP_PRIVATE: i32 = 0x02;
    const MAP_ANONYMOUS: i32 = 0x20;
    const MAP_NORESERVE: i32 = 0x4000;
    const SYS_GETTID: i64 = 186;

    pub fn reserve(bytes: usize) -> *mut u8 {
        let memory = unsafe {
            mmap(core::ptr::null_mut(), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)
        };
        if memory as isize == -1 {
            core::ptr::null_mut()
        } else {
            memory as *mut u8
        }
    }

    pub fn commit(_memory: *mut u8, _bytes: usize) -> bool {
        true
    }

    pub fn release(memory: *mut u8, bytes: usize) {
        unsafe {
            munmap(memory as *mut c_void, bytes);
        }
    }

    pub fn thread_id() -> u32 {
        unsafe { syscall(SYS_GETTID) as u32 }
    }

    #[cfg(target_arch = "x86_64")]
    #[inline(always)]
    pub fn thread_token() -> u64 {
        let token: u64;
        unsafe { core::arch::asm!("mov {}, fs:[0]", out(reg) token, options(nostack, preserves_flags, readonly, pure)) };
        token
    }

    #[cfg(not(target_arch = "x86_64"))]
    pub fn thread_token() -> u64 {
        thread_id() as u64
    }

    pub fn sleep_while_held(_state: &core::sync::atomic::AtomicU32) {
        unsafe {
            sched_yield();
        }
    }

    pub fn wake_sleeper(_state: &core::sync::atomic::AtomicU32) {}
}

#[cfg(windows)]
mod imp {
    use core::ffi::c_void;

    #[link(name = "kernel32")]
    extern "system" {
        fn VirtualAlloc(address: *mut c_void, size: usize, kind: u32, protect: u32) -> *mut c_void;
        fn VirtualFree(address: *mut c_void, size: usize, kind: u32) -> i32;
        fn GetCurrentThreadId() -> u32;
    }

    #[link(name = "synchronization")]
    extern "system" {
        fn WaitOnAddress(address: *const c_void, compare: *const c_void, size: usize, milliseconds: u32) -> i32;
        fn WakeByAddressSingle(address: *const c_void);
    }

    const MEM_COMMIT: u32 = 0x1000;
    const MEM_RESERVE: u32 = 0x2000;
    const MEM_RELEASE: u32 = 0x8000;
    const PAGE_READWRITE: u32 = 0x04;

    pub fn reserve(bytes: usize) -> *mut u8 {
        unsafe { VirtualAlloc(core::ptr::null_mut(), bytes, MEM_RESERVE, PAGE_READWRITE) as *mut u8 }
    }

    pub fn commit(memory: *mut u8, bytes: usize) -> bool {
        unsafe { !VirtualAlloc(memory as *mut c_void, bytes, MEM_COMMIT, PAGE_READWRITE).is_null() }
    }

    pub fn release(memory: *mut u8, _bytes: usize) {
        unsafe {
            VirtualFree(memory as *mut c_void, 0, MEM_RELEASE);
        }
    }

    pub fn thread_id() -> u32 {
        unsafe { GetCurrentThreadId() }
    }

    #[inline(always)]
    pub fn thread_token() -> u64 {
        let token: u64;
        unsafe { core::arch::asm!("mov {}, gs:[0x30]", out(reg) token, options(nostack, preserves_flags, readonly, pure)) };
        token
    }

    pub fn sleep_while_held(state: &core::sync::atomic::AtomicU32) {
        let held: u32 = 1;
        unsafe {
            WaitOnAddress(state as *const _ as *const c_void, &held as *const u32 as *const c_void, 4, 1);
        }
    }

    pub fn wake_sleeper(state: &core::sync::atomic::AtomicU32) {
        unsafe { WakeByAddressSingle(state as *const _ as *const c_void) }
    }
}

pub use imp::*;
