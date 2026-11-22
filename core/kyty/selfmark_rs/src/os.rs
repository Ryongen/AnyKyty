pub const PAGE: u64 = 4096;

#[cfg(unix)]
mod imp {
    use core::ffi::c_void;
    use core::sync::atomic::{AtomicU8, Ordering};

    extern "C" {
        fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, offset: i64) -> *mut c_void;
        fn munmap(addr: *mut c_void, len: usize) -> i32;
        fn mprotect(addr: *mut c_void, len: usize, prot: i32) -> i32;
        fn syscall(number: i64, ...) -> i64;
        fn sched_yield() -> i32;
    }

    const PROT_READ: i32 = 1;
    const PROT_WRITE: i32 = 2;
    const PROT_EXEC: i32 = 4;
    const MAP_PRIVATE: i32 = 0x02;
    const MAP_ANONYMOUS: i32 = 0x20;
    const MAP_NORESERVE: i32 = 0x4000;
    const SYS_MEMBARRIER: i64 = 324;
    const SYS_GETTID: i64 = 186;
    const MEMBARRIER_QUERY: i64 = 0;
    const MEMBARRIER_SYNC_CORE: i64 = 1 << 5;
    const MEMBARRIER_REGISTER_SYNC_CORE: i64 = 1 << 6;

    static SYNC: AtomicU8 = AtomicU8::new(0);

    pub struct CodeAccess;

    pub fn reserve(bytes: usize) -> *mut u8 {
        let memory = unsafe { mmap(core::ptr::null_mut(), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) };
        if memory as isize == -1 {
            core::ptr::null_mut()
        } else {
            memory as *mut u8
        }
    }

    pub fn commit(_memory: *mut u8, _bytes: usize) -> bool {
        true
    }

    pub fn lazy() -> bool {
        false
    }

    pub fn release(memory: *mut u8, bytes: usize) {
        unsafe {
            munmap(memory as *mut c_void, bytes);
        }
    }

    pub fn executable(bytes: usize) -> *mut u8 {
        let memory = unsafe { mmap(core::ptr::null_mut(), bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) };
        if memory as isize == -1 {
            core::ptr::null_mut()
        } else {
            memory as *mut u8
        }
    }

    fn span(address: u64, bytes: usize) -> (u64, usize) {
        let first = address & !(super::PAGE - 1);
        let last = (address + bytes as u64 + super::PAGE - 1) & !(super::PAGE - 1);
        (first, (last - first) as usize)
    }

    pub fn open_code(address: u64, bytes: usize) -> Option<CodeAccess> {
        let (first, length) = span(address, bytes);
        if unsafe { mprotect(first as *mut c_void, length, PROT_READ | PROT_WRITE | PROT_EXEC) } != 0 {
            return None;
        }
        Some(CodeAccess)
    }

    pub fn close_code(address: u64, bytes: usize, _access: CodeAccess) {
        let (first, length) = span(address, bytes);
        unsafe {
            mprotect(first as *mut c_void, length, PROT_READ | PROT_EXEC);
        }
    }

    pub fn core_sync_ready() -> bool {
        match SYNC.load(Ordering::Acquire) {
            1 => true,
            2 => false,
            _ => {
                let supported = unsafe { syscall(SYS_MEMBARRIER, MEMBARRIER_QUERY, 0i64, 0i64) };
                let ready = supported > 0 && supported & MEMBARRIER_SYNC_CORE != 0 && unsafe { syscall(SYS_MEMBARRIER, MEMBARRIER_REGISTER_SYNC_CORE, 0i64, 0i64) } == 0;
                SYNC.store(if ready { 1 } else { 2 }, Ordering::Release);
                ready
            }
        }
    }

    pub fn sync_cores() {
        unsafe {
            syscall(SYS_MEMBARRIER, MEMBARRIER_SYNC_CORE, 0i64, 0i64);
        }
    }

    pub fn flush_code(_address: u64, _bytes: usize) {}

    pub fn relax() {
        unsafe {
            sched_yield();
        }
    }

    pub fn thread_id() -> u64 {
        unsafe { syscall(SYS_GETTID) as u64 }
    }
}

#[cfg(windows)]
mod imp {
    use core::ffi::c_void;

    extern "system" {
        fn VirtualAlloc(address: *mut c_void, size: usize, kind: u32, protect: u32) -> *mut c_void;
        fn VirtualFree(address: *mut c_void, size: usize, kind: u32) -> i32;
        fn VirtualProtect(address: *mut c_void, size: usize, protect: u32, old: *mut u32) -> i32;
        fn FlushInstructionCache(process: *mut c_void, address: *const c_void, size: usize) -> i32;
        fn FlushProcessWriteBuffers();
        fn GetCurrentProcess() -> *mut c_void;
        fn SwitchToThread() -> i32;
        fn GetCurrentThreadId() -> u32;
    }

    const MEM_COMMIT: u32 = 0x1000;
    const MEM_RESERVE: u32 = 0x2000;
    const MEM_RELEASE: u32 = 0x8000;
    const PAGE_READWRITE: u32 = 0x04;
    const PAGE_EXECUTE_READWRITE: u32 = 0x40;

    pub struct CodeAccess {
        old: u32,
    }

    pub fn reserve(bytes: usize) -> *mut u8 {
        unsafe { VirtualAlloc(core::ptr::null_mut(), bytes, MEM_RESERVE, PAGE_READWRITE) as *mut u8 }
    }

    pub fn commit(memory: *mut u8, bytes: usize) -> bool {
        !unsafe { VirtualAlloc(memory as *mut c_void, bytes, MEM_COMMIT, PAGE_READWRITE) }.is_null()
    }

    pub fn lazy() -> bool {
        true
    }

    pub fn release(memory: *mut u8, _bytes: usize) {
        unsafe {
            VirtualFree(memory as *mut c_void, 0, MEM_RELEASE);
        }
    }

    pub fn executable(bytes: usize) -> *mut u8 {
        unsafe { VirtualAlloc(core::ptr::null_mut(), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) as *mut u8 }
    }

    pub fn open_code(address: u64, bytes: usize) -> Option<CodeAccess> {
        let mut old = 0u32;
        if unsafe { VirtualProtect(address as *mut c_void, bytes, PAGE_EXECUTE_READWRITE, &mut old) } == 0 {
            return None;
        }
        Some(CodeAccess { old })
    }

    pub fn close_code(address: u64, bytes: usize, access: CodeAccess) {
        let mut old = 0u32;
        unsafe {
            VirtualProtect(address as *mut c_void, bytes, access.old, &mut old);
        }
    }

    pub fn core_sync_ready() -> bool {
        true
    }

    pub fn sync_cores() {
        unsafe {
            FlushProcessWriteBuffers();
        }
    }

    pub fn flush_code(address: u64, bytes: usize) {
        unsafe {
            FlushInstructionCache(GetCurrentProcess(), address as *const c_void, bytes);
        }
    }

    pub fn relax() {
        unsafe {
            SwitchToThread();
        }
    }

    pub fn thread_id() -> u64 {
        unsafe { GetCurrentThreadId() as u64 }
    }
}

pub use imp::*;
