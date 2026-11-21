use crate::cards;
use crate::decode::{self, Refusal, Store};
use crate::sites;
use core::ffi::c_void;
use core::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock};

extern "C" {
    fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, offset: i64) -> *mut c_void;
    fn mprotect(addr: *mut c_void, len: usize, prot: i32) -> i32;
    fn memfd_create(name: *const u8, flags: u32) -> i32;
    fn ftruncate(fd: i32, length: i64) -> i32;
    fn sigaction(signal: i32, action: *const SigAction, old: *mut SigAction) -> i32;
    fn sigaltstack(stack: *const AltStack, old: *mut AltStack) -> i32;
    fn CallStub(stub: *const u8, regs: *mut Regs);
}

core::arch::global_asm!(
    r#"
.text
.globl CallStub
.type CallStub,@function
CallStub:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rsi
    push %rdi
    mov %rsi, %rax
    vmovdqu 144(%rax), %ymm0
    vmovdqu 176(%rax), %ymm1
    vmovdqu 208(%rax), %ymm2
    vmovdqu 240(%rax), %ymm3
    vmovdqu 272(%rax), %ymm4
    vmovdqu 304(%rax), %ymm5
    vmovdqu 336(%rax), %ymm6
    vmovdqu 368(%rax), %ymm7
    vmovdqu 400(%rax), %ymm8
    vmovdqu 432(%rax), %ymm9
    vmovdqu 464(%rax), %ymm10
    vmovdqu 496(%rax), %ymm11
    vmovdqu 528(%rax), %ymm12
    vmovdqu 560(%rax), %ymm13
    vmovdqu 592(%rax), %ymm14
    vmovdqu 624(%rax), %ymm15
    mov 8(%rax), %rcx
    mov 16(%rax), %rdx
    mov 24(%rax), %rbx
    mov 40(%rax), %rbp
    mov 48(%rax), %rsi
    mov 56(%rax), %rdi
    mov 64(%rax), %r8
    mov 72(%rax), %r9
    mov 80(%rax), %r10
    mov 88(%rax), %r11
    mov 96(%rax), %r12
    mov 104(%rax), %r13
    mov 112(%rax), %r14
    mov 120(%rax), %r15
    push 128(%rax)
    popfq
    mov 0(%rax), %rax
    call *(%rsp)
    push %rax
    mov 16(%rsp), %rax
    pop 0(%rax)
    pushfq
    pop 128(%rax)
    mov %rcx, 8(%rax)
    mov %rdx, 16(%rax)
    mov %rbx, 24(%rax)
    mov %rbp, 40(%rax)
    mov %rsi, 48(%rax)
    mov %rdi, 56(%rax)
    mov %r8, 64(%rax)
    mov %r9, 72(%rax)
    mov %r10, 80(%rax)
    mov %r11, 88(%rax)
    mov %r12, 96(%rax)
    mov %r13, 104(%rax)
    mov %r14, 112(%rax)
    mov %r15, 120(%rax)
    vmovdqu %ymm0, 144(%rax)
    vmovdqu %ymm1, 176(%rax)
    vmovdqu %ymm2, 208(%rax)
    vmovdqu %ymm3, 240(%rax)
    vmovdqu %ymm4, 272(%rax)
    vmovdqu %ymm5, 304(%rax)
    vmovdqu %ymm6, 336(%rax)
    vmovdqu %ymm7, 368(%rax)
    vmovdqu %ymm8, 400(%rax)
    vmovdqu %ymm9, 432(%rax)
    vmovdqu %ymm10, 464(%rax)
    vmovdqu %ymm11, 496(%rax)
    vmovdqu %ymm12, 528(%rax)
    vmovdqu %ymm13, 560(%rax)
    vmovdqu %ymm14, 592(%rax)
    vmovdqu %ymm15, 624(%rax)
    vzeroupper
    pop %rdi
    pop %rsi
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
"#,
    options(att_syntax)
);

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Regs {
    pub gpr: [u64; 16],
    pub rflags: u64,
    pub pad: u64,
    pub ymm: [[u8; 32]; 16],
}

#[repr(C)]
struct SigAction {
    handler: usize,
    flags: u64,
    restorer: usize,
    mask: [u64; 16],
}

#[repr(C)]
struct AltStack {
    sp: *mut c_void,
    flags: i32,
    size: usize,
}

const PROT_READ: i32 = 1;
const PROT_WRITE: i32 = 2;
const PROT_EXEC: i32 = 4;
const MAP_SHARED: i32 = 0x01;
const MAP_PRIVATE: i32 = 0x02;
const MAP_ANONYMOUS: i32 = 0x20;
const MAP_FIXED_NOREPLACE: i32 = 0x100000;
const SIGTRAP: i32 = 5;
const SIGSEGV: i32 = 11;
const SA_SIGINFO: u64 = 4;
const SA_ONSTACK: u64 = 0x0800_0000;
const SA_NODEFER: u64 = 0x4000_0000;
const SA_RESTORER: u64 = 0x0400_0000;
const REG_RIP: usize = 16;
const GREGS_AT: usize = 40;

pub const GUEST_BYTES: u64 = 256 << 20;
pub const PAGE: u64 = 4096;
const CODE_BYTES: usize = 64 << 20;
const STUB_BYTES: usize = 32 << 20;

pub struct World {
    pub primary: u64,
    pub alias: u64,
    pub code: u64,
}

static WORLD: OnceLock<World> = OnceLock::new();
static SERIAL: Mutex<()> = Mutex::new(());
static STUB_NEXT: AtomicUsize = AtomicUsize::new(0);
static ARENA_NEXT: AtomicUsize = AtomicUsize::new(STUB_BYTES);
pub static FAULTS: AtomicU64 = AtomicU64::new(0);
pub static OPENED: AtomicU64 = AtomicU64::new(0);
pub static LAST_REFUSAL: AtomicU64 = AtomicU64::new(0);
pub static ARM_ON_FAULT: AtomicU64 = AtomicU64::new(1);
static REPEAT_RIP: AtomicU64 = AtomicU64::new(0);
static REPEAT_ADDRESS: AtomicU64 = AtomicU64::new(0);

core::arch::global_asm!(
    r#"
.text
.globl selfmark_test_restorer
.type selfmark_test_restorer,@function
selfmark_test_restorer:
    mov $15, %eax
    syscall
"#,
    options(att_syntax)
);

extern "C" {
    fn selfmark_test_restorer();
}

unsafe extern "C" fn near(_site: u64, bytes: u64) -> u64 {
    let world = WORLD.get().unwrap();
    let at = ARENA_NEXT.fetch_add(bytes as usize, Ordering::SeqCst);
    if at + bytes as usize > CODE_BYTES {
        return 0;
    }
    world.code + at as u64
}

unsafe extern "C" fn guest(address: u64) -> bool {
    let world = WORLD.get().unwrap();
    address >= world.code && address < world.code + STUB_BYTES as u64
}

fn in_guest(world: &World, address: u64) -> bool {
    address >= world.primary && address < world.primary + GUEST_BYTES
}

fn open_page(address: u64) {
    unsafe {
        mprotect((address & !(PAGE - 1)) as *mut c_void, PAGE as usize, PROT_READ | PROT_WRITE);
    }
    OPENED.fetch_add(1, Ordering::SeqCst);
}

unsafe extern "C" fn on_fault(_signal: i32, info: *const u64, context: *mut u8) {
    let world = WORLD.get().unwrap();
    let address = *info.add(2);
    let gregs = context.add(GREGS_AT) as *mut u64;
    let rip = *gregs.add(REG_RIP);
    FAULTS.fetch_add(1, Ordering::SeqCst);
    if !in_guest(world, address) {
        extern "C" {
            #[link_name = "write"]
            fn write_out(fd: i32, data: *const u8, bytes: usize) -> isize;
            fn _exit(code: i32) -> !;
        }
        let mut text = *b"selfmark harness: fault outside the guest range, address 0000000000000000 rip 0000000000000000\n";
        for (value, at) in [(address, 57usize), (rip, 78usize)] {
            for digit in 0..16 {
                text[at + digit] = b"0123456789abcdef"[((value >> (60 - digit * 4)) & 15) as usize];
            }
        }
        write_out(2, text.as_ptr(), text.len());
        _exit(70);
    }
    let (mut outcome, mut value) = sites::write_fault(rip);
    if outcome == sites::OUTCOME_BUSY {
        sites::service();
        (outcome, value) = sites::write_fault(rip);
    }
    let delta = world.alias.wrapping_sub(world.primary) as i64;
    match outcome {
        sites::OUTCOME_PATCHED | sites::OUTCOME_SLOW_STORE => {
            let repeated = REPEAT_RIP.swap(rip, Ordering::SeqCst) == rip && REPEAT_ADDRESS.swap(address, Ordering::SeqCst) == address;
            REPEAT_ADDRESS.store(address, Ordering::SeqCst);
            if ARM_ON_FAULT.load(Ordering::SeqCst) != 0 && !repeated {
                cards::arm(address & !(PAGE - 1), delta);
                let width = sites::width_of(value) as u64;
                if (address & (PAGE - 1)) + width > PAGE {
                    cards::arm((address & !(PAGE - 1)) + PAGE, delta);
                }
            } else {
                cards::arm(address & !(PAGE - 1), 0);
                open_page(address);
            }
            if outcome == sites::OUTCOME_SLOW_STORE {
                *gregs.add(REG_RIP) = value;
            }
        }
        sites::OUTCOME_REFUSED => {
            LAST_REFUSAL.store(value, Ordering::SeqCst);
            open_page(address);
        }
        sites::OUTCOME_BUSY => {}
        _ => open_page(address),
    }
}

unsafe extern "C" fn on_trap(_signal: i32, _info: *const u64, context: *mut u8) {
    let gregs = context.add(GREGS_AT) as *mut u64;
    let at = *gregs.add(REG_RIP) - 1;
    let resume = sites::breakpoint(at);
    if resume == 0 {
        extern "C" {
            fn _exit(code: i32) -> !;
        }
        _exit(71);
    }
    *gregs.add(REG_RIP) = resume;
}

fn install(signal: i32, handler: usize) {
    let action = SigAction { handler, flags: SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESTORER, restorer: selfmark_test_restorer as *const () as usize, mask: [0; 16] };
    extern "C" {
        fn syscall(number: i64, ...) -> i64;
    }
    let done = unsafe { syscall(13, signal as i64, &action as *const SigAction, core::ptr::null_mut::<SigAction>(), 8i64) };
    assert_eq!(done, 0);
    let _ = sigaction;
}

pub fn alt_stack() {
    let size = 256 * 1024;
    let memory = unsafe { mmap(core::ptr::null_mut(), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) };
    let stack = AltStack { sp: memory, flags: 0, size };
    assert_eq!(unsafe { sigaltstack(&stack, core::ptr::null_mut()) }, 0);
}

pub fn world() -> (&'static World, MutexGuard<'static, ()>) {
    let guard = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let world = WORLD.get_or_init(|| unsafe {
        let fd = memfd_create(b"selfmark\0".as_ptr(), 0);
        assert!(fd >= 0);
        assert_eq!(ftruncate(fd, GUEST_BYTES as i64), 0);
        let mut primary = usize::MAX as *mut c_void;
        for step in 0..64u64 {
            let hint = (0x1_0000_0000u64 + step * 0x1000_0000) as *mut c_void;
            let mapped = mmap(hint, GUEST_BYTES as usize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
            if mapped == hint {
                primary = mapped;
                break;
            }
        }
        assert_ne!(primary as usize, usize::MAX);
        let alias = mmap(core::ptr::null_mut(), GUEST_BYTES as usize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert_ne!(alias as isize, -1);
        let code = mmap(core::ptr::null_mut(), CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert_ne!(code as isize, -1);
        assert!(cards::init(primary as u64 + GUEST_BYTES));
        sites::configure(Some(near), Some(guest));
        sites::set_enabled(true);
        install(SIGSEGV, on_fault as *const () as usize);
        install(SIGTRAP, on_trap as *const () as usize);
        World { primary: primary as u64, alias: alias as u64, code: code as u64 }
    });
    alt_stack();
    (world, guard)
}

pub fn stub(world: &World, bytes: &[u8]) -> u64 {
    let at = STUB_NEXT.fetch_add(64, Ordering::SeqCst);
    assert!(at + 64 <= STUB_BYTES);
    let address = world.code + at as u64;
    unsafe {
        mprotect((address & !(PAGE - 1)) as *mut c_void, 2 * PAGE as usize, PROT_READ | PROT_WRITE | PROT_EXEC);
        core::ptr::write_bytes(address as *mut u8, 0x90, 64);
        core::ptr::copy_nonoverlapping(bytes.as_ptr(), address as *mut u8, bytes.len());
        *((address + bytes.len() as u64 + 8) as *mut u8) = 0xC3;
    }
    address
}

pub fn call(stub: u64, regs: &mut Regs) {
    unsafe { CallStub(stub as *const u8, regs) }
}

pub fn protect(address: u64, writable: bool) {
    unsafe {
        mprotect((address & !(PAGE - 1)) as *mut c_void, PAGE as usize, if writable { PROT_READ | PROT_WRITE } else { PROT_READ });
    }
}

pub fn seed(flags: u64) -> Regs {
    let mut regs = Regs { gpr: [0; 16], rflags: 0x202 | flags, pad: 0, ymm: [[0; 32]; 16] };
    for (index, slot) in regs.gpr.iter_mut().enumerate() {
        *slot = 0x1111_1111_1111_1111u64.wrapping_mul(index as u64 + 1).wrapping_add(0x0123_4567_89AB_CDEF);
    }
    for (index, lane) in regs.ymm.iter_mut().enumerate() {
        for (byte, slot) in lane.iter_mut().enumerate() {
            *slot = (index * 37 + byte * 11 + 5) as u8;
        }
    }
    regs
}

pub fn aim(regs: &mut Regs, store: &Store, target: u64) -> bool {
    let index = 0x40u64;
    let Some(register) = store.index else {
        regs.gpr[store.base as usize] = target.wrapping_sub(store.disp as i64 as u64);
        return true;
    };
    if register == store.base {
        return false;
    }
    regs.gpr[register as usize] = index;
    regs.gpr[store.base as usize] = target.wrapping_sub(store.disp as i64 as u64).wrapping_sub(index * store.scale as u64);
    true
}

pub fn window(address: u64) -> [u8; 192] {
    let mut bytes = [0u8; 192];
    unsafe { core::ptr::copy_nonoverlapping((address - 64) as *const u8, bytes.as_mut_ptr(), 192) };
    bytes
}

pub fn fill(world: &World, address: u64) {
    let through = if in_guest(world, address) { world.alias + (address - world.primary) } else { address };
    unsafe { core::ptr::write_bytes((through - 64) as *mut u8, 0xEE, 192) };
}

pub fn taken(begin: u64, end: u64) -> Vec<(u64, u64)> {
    let mut runs = [0u64; 64];
    let mut next = 0u64;
    let count = unsafe { cards::take(begin, end, runs.as_mut_ptr(), 32, &mut next) };
    assert_eq!(next, end);
    (0..count).map(|run| (runs[run * 2], runs[run * 2 + 1])).collect()
}

const FLAG_SETS: [u64; 4] = [0, 0x8D5, 0x001 | 0x040 | 0x800, 0x004 | 0x010 | 0x080];

#[repr(align(64))]
struct Aligned([u8; 512]);

pub struct Checked {
    pub refusal: Option<Refusal>,
}

pub fn check(world: &World, bytes: &[u8], slot: u64) -> Checked {
    let store = match decode::decode(bytes) {
        Ok(store) => store,
        Err(refusal) => return Checked { refusal: Some(refusal) },
    };
    let length = store.len as usize;
    let page = world.primary + (0x100 + slot * 4) * PAGE;
    let target = page + 0x200;
    let plain = stub(world, &bytes[..length]);
    let patched = stub(world, &bytes[..length]);
    let mut expected = Vec::new();
    for &flags in FLAG_SETS.iter() {
        let mut regs = seed(flags);
        if !aim(&mut regs, &store, target) {
            return Checked { refusal: None };
        }
        fill(world, target);
        protect(target, true);
        call(plain, &mut regs);
        expected.push((regs, window(target)));
    }
    let untouched = expected[0].1;

    fill(world, target);
    cards::arm(page, 0);
    protect(target, false);
    LAST_REFUSAL.store(0, Ordering::SeqCst);
    let faults = FAULTS.load(Ordering::SeqCst);
    let opened = OPENED.load(Ordering::SeqCst);
    let mut regs = seed(FLAG_SETS[0]);
    aim(&mut regs, &store, target);
    call(patched, &mut regs);
    let refused = LAST_REFUSAL.load(Ordering::SeqCst);
    if refused != 0 {
        protect(target, true);
        panic!("{bytes:02x?} decoded but the site was refused with {refused}");
    }
    assert_eq!(FAULTS.load(Ordering::SeqCst), faults + 1, "{bytes:02x?} first write faults once");
    assert_eq!(OPENED.load(Ordering::SeqCst), opened, "{bytes:02x?} page stays protected");
    assert!(regs == expected[0].0, "{bytes:02x?} registers after the first corrected write");
    assert_eq!(window(world.alias + (target - world.primary)), untouched, "{bytes:02x?} memory after the first corrected write");
    assert_eq!(taken(page, page + PAGE), vec![(page, page + PAGE)], "{bytes:02x?} card after the first corrected write");
    assert_eq!(unsafe { *(patched as *const u8) }, 0xE9, "{bytes:02x?} site holds a jump");

    for (round, &flags) in FLAG_SETS.iter().enumerate() {
        fill(world, target);
        let before = FAULTS.load(Ordering::SeqCst);
        let mut regs = seed(flags);
        aim(&mut regs, &store, target);
        call(patched, &mut regs);
        assert_eq!(FAULTS.load(Ordering::SeqCst), before, "{bytes:02x?} corrected write takes no fault");
        assert!(regs == expected[round].0, "{bytes:02x?} registers and flags, flag set {round}");
        assert_eq!(window(world.alias + (target - world.primary)), expected[round].1, "{bytes:02x?} memory, flag set {round}");
        assert_eq!(taken(page, page + PAGE), vec![(page, page + PAGE)], "{bytes:02x?} card, flag set {round}");
    }

    cards::arm(page, 0);
    protect(target, true);
    fill(world, target);
    let before = FAULTS.load(Ordering::SeqCst);
    let mut regs = seed(FLAG_SETS[1]);
    aim(&mut regs, &store, target);
    call(patched, &mut regs);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before, "{bytes:02x?} open page takes no fault");
    assert!(regs == expected[1].0, "{bytes:02x?} registers on the slow path");
    assert_eq!(window(target), expected[1].1, "{bytes:02x?} memory on the slow path");
    assert!(taken(page, page + PAGE).is_empty(), "{bytes:02x?} no card on the slow path");

    protect(target, false);
    fill(world, target);
    let before = FAULTS.load(Ordering::SeqCst);
    let mut regs = seed(FLAG_SETS[2]);
    aim(&mut regs, &store, target);
    call(patched, &mut regs);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before + 1, "{bytes:02x?} slow store faults once on a protected page");
    assert!(regs == expected[2].0, "{bytes:02x?} registers after the slow store was resumed at the entry");
    assert_eq!(window(world.alias + (target - world.primary)), expected[2].1, "{bytes:02x?} memory after the resumed write");
    assert_eq!(taken(page, page + PAGE), vec![(page, page + PAGE)], "{bytes:02x?} card after the resumed write");

    let mut outside = Box::new(Aligned([0xEEu8; 512]));
    let far = outside.0.as_mut_ptr() as u64 + 256;
    let mut want = seed(FLAG_SETS[3]);
    aim(&mut want, &store, far);
    call(plain, &mut want);
    let wanted = window(far);
    unsafe { core::ptr::write_bytes(outside.0.as_mut_ptr(), 0xEE, 512) };
    let mut regs = seed(FLAG_SETS[3]);
    aim(&mut regs, &store, far);
    let before = FAULTS.load(Ordering::SeqCst);
    call(patched, &mut regs);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before, "{bytes:02x?} write outside the guest range takes no fault");
    assert!(regs == want, "{bytes:02x?} registers outside the guest range");
    assert_eq!(window(far), wanted, "{bytes:02x?} memory outside the guest range");

    cards::arm(page, 0);
    protect(target, true);
    Checked { refusal: None }
}

#[test]
fn corrected_writes_match_the_original_instruction() {
    let (world, _guard) = world();
    let forms: &[&[u8]] = &[
        &[0xC5, 0xFC, 0x11, 0x81, 0xA0, 0x00, 0x00, 0x00],
        &[0xC4, 0xC1, 0x78, 0x11, 0x2C, 0x31],
        &[0xC5, 0xF9, 0xE7, 0x4F, 0x10],
        &[0xC5, 0xFD, 0xE7, 0x47, 0x20],
        &[0xC5, 0xF9, 0x7F, 0x4F, 0x10],
        &[0xC5, 0xFE, 0x7F, 0x47, 0x20],
        &[0xC5, 0xF8, 0x29, 0x47, 0x40],
        &[0xC5, 0xFC, 0x2B, 0x47, 0x40],
        &[0xC5, 0xFA, 0x11, 0x47, 0x08],
        &[0xC5, 0xFB, 0x11, 0x47, 0x08],
        &[0xC5, 0xF9, 0xD6, 0x47, 0x08],
        &[0xC5, 0xF9, 0x7E, 0x47, 0x08],
        &[0xC4, 0xE1, 0xF9, 0x7E, 0x47, 0x08],
        &[0xC5, 0xF8, 0x13, 0x47, 0x08],
        &[0xC5, 0xF8, 0x17, 0x47, 0x08],
        &[0xC4, 0x41, 0x7C, 0x11, 0x44, 0x05, 0x00],
        &[0xC4, 0x21, 0x7D, 0x7F, 0x3C, 0x38],
        &[0x48, 0x89, 0x84, 0xCB, 0x00, 0x01, 0x00, 0x00],
        &[0x4C, 0x89, 0x7C, 0xC8, 0x10],
        &[0x66, 0x41, 0x89, 0x47, 0x10],
        &[0x41, 0x89, 0x44, 0x24, 0x10],
        &[0x45, 0x89, 0x6C, 0x9D, 0x00],
        &[0xC7, 0x47, 0x10, 0x78, 0x56, 0x34, 0x12],
        &[0x48, 0xC7, 0x00, 0x01, 0x00, 0x00, 0x80],
        &[0x66, 0xC7, 0x47, 0x10, 0x34, 0x12],
        &[0xC6, 0x84, 0x08, 0x00, 0x10, 0x00, 0x00, 0x7F],
        &[0x88, 0xA7, 0x00, 0x01, 0x00, 0x00],
        &[0x40, 0x88, 0xB7, 0x00, 0x01, 0x00, 0x00],
        &[0x88, 0x84, 0x0B, 0x00, 0x01, 0x00, 0x00],
        &[0x0F, 0x11, 0x44, 0x08, 0x10],
        &[0x66, 0x0F, 0x7F, 0x47, 0x10],
        &[0xF3, 0x0F, 0x7F, 0x47, 0x11],
        &[0x66, 0x0F, 0xE7, 0x47, 0x10],
        &[0x0F, 0x2B, 0x44, 0x08, 0x10],
        &[0x66, 0x0F, 0x2B, 0x47, 0x10],
        &[0x66, 0x0F, 0xD6, 0x47, 0x10],
        &[0x66, 0x48, 0x0F, 0x7E, 0x47, 0x10],
        &[0xF3, 0x0F, 0x11, 0x47, 0x10],
        &[0xF2, 0x0F, 0x11, 0x47, 0x10],
        &[0x48, 0x0F, 0xC3, 0x44, 0x08, 0x10],
        &[0x0F, 0xC3, 0x84, 0x08, 0x10, 0x00, 0x00, 0x00],
        &[0x44, 0x0F, 0x11, 0x7C, 0x0E, 0x10],
        &[0x48, 0x89, 0x8C, 0x56, 0x00, 0x01, 0x00, 0x00],
        &[0x4C, 0x89, 0x94, 0x7A, 0x00, 0x01, 0x00, 0x00],
        &[0x4D, 0x89, 0x9C, 0xDF, 0x00, 0x01, 0x00, 0x00],
    ];
    for (slot, form) in forms.iter().enumerate() {
        let checked = check(world, form, slot as u64);
        assert_eq!(checked.refusal, None, "{form:02x?}");
    }
}

#[test]
fn writes_across_two_pages() {
    let (world, _guard) = world();
    let delta = world.alias.wrapping_sub(world.primary) as i64;
    let bytes = [0xC5, 0xFE, 0x7F, 0x47, 0x20];
    let store = decode::decode(&bytes).unwrap();
    let first = world.primary + 0x4000 * PAGE;
    let second = first + PAGE;
    let target = second - 8;
    let plain = stub(world, &bytes);
    let patched = stub(world, &bytes);
    let mut want = seed(0x8D5);
    aim(&mut want, &store, target);
    fill(world, target);
    call(plain, &mut want);
    let wanted = window(target);

    fill(world, target);
    protect(first, false);
    protect(second, false);
    let before = FAULTS.load(Ordering::SeqCst);
    let mut regs = seed(0x8D5);
    aim(&mut regs, &store, target);
    call(patched, &mut regs);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before + 1);
    assert!(regs == want);
    assert_eq!(window(world.alias + (target - world.primary)), wanted);
    assert_eq!(taken(first, second + PAGE), vec![(first, second + PAGE)]);

    fill(world, target);
    let before = FAULTS.load(Ordering::SeqCst);
    let mut regs = seed(0x8D5);
    aim(&mut regs, &store, target);
    call(patched, &mut regs);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before);
    assert!(regs == want);
    assert_eq!(window(world.alias + (target - world.primary)), wanted);
    assert_eq!(taken(first, second + PAGE), vec![(first, second + PAGE)]);

    cards::arm(second, 0);
    protect(second, true);
    fill(world, target);
    let before = FAULTS.load(Ordering::SeqCst);
    let opened = OPENED.load(Ordering::SeqCst);
    let mut regs = seed(0x8D5);
    aim(&mut regs, &store, target);
    ARM_ON_FAULT.store(0, Ordering::SeqCst);
    call(patched, &mut regs);
    ARM_ON_FAULT.store(1, Ordering::SeqCst);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before + 1);
    assert_eq!(OPENED.load(Ordering::SeqCst), opened + 1);
    assert!(regs == want);
    assert_eq!(window(target), wanted);
    assert!(taken(first, second + PAGE).is_empty());
    assert_eq!(cards::armed(first), 0);
    let _ = delta;
    protect(first, true);
}

#[test]
fn unsupported_sites_keep_the_fault() {
    let (world, _guard) = world();
    let cases: &[(&[u8], Refusal)] = &[
        (&[0x48, 0x89, 0x17], Refusal::Short),
        (&[0xC5, 0xF9, 0xE7, 0x07], Refusal::Short),
        (&[0x48, 0x01, 0x87, 0x00, 0x01, 0x00, 0x00], Refusal::NotStore),
        (&[0xF0, 0x48, 0x01, 0x87, 0x00, 0x01, 0x00, 0x00], Refusal::Prefix),
    ];
    for (slot, (bytes, refusal)) in cases.iter().enumerate() {
        let page = world.primary + (0x6000 + slot as u64) * PAGE;
        let target = page + 0x80;
        let code = stub(world, bytes);
        let mut regs = seed(0);
        regs.gpr[7] = target;
        if bytes[1] == 0x01 || bytes[2] == 0x01 {
            regs.gpr[7] = target - 0x100;
        }
        fill(world, target);
        protect(target, false);
        LAST_REFUSAL.store(0, Ordering::SeqCst);
        let faults = FAULTS.load(Ordering::SeqCst);
        let opened = OPENED.load(Ordering::SeqCst);
        call(code, &mut regs);
        assert_eq!(FAULTS.load(Ordering::SeqCst), faults + 1, "{bytes:02x?}");
        assert_eq!(OPENED.load(Ordering::SeqCst), opened + 1, "{bytes:02x?}");
        assert_eq!(LAST_REFUSAL.load(Ordering::SeqCst), *refusal as u64, "{bytes:02x?}");
        assert_eq!(unsafe { *(code as *const u8) }, bytes[0], "{bytes:02x?} site left as it was");
        assert!(taken(page, page + PAGE).is_empty(), "{bytes:02x?}");
        protect(target, false);
        let faults = FAULTS.load(Ordering::SeqCst);
        call(code, &mut regs);
        assert_eq!(FAULTS.load(Ordering::SeqCst), faults + 1, "{bytes:02x?} refused site faults again");
        protect(target, true);
    }
}

#[test]
fn refused_pages_take_the_slow_path() {
    let (world, _guard) = world();
    let bytes = [0x48, 0x89, 0x87, 0x00, 0x01, 0x00, 0x00];
    let store = decode::decode(&bytes).unwrap();
    let page = world.primary + 0x7000 * PAGE;
    let target = page + 0x40;
    let patched = stub(world, &bytes);
    protect(page, false);
    let mut regs = seed(0);
    aim(&mut regs, &store, target);
    call(patched, &mut regs);
    assert_eq!(unsafe { *(patched as *const u8) }, 0xE9);
    assert_eq!(taken(page, page + PAGE), vec![(page, page + PAGE)]);
    assert!(cards::arm(page, cards::REFUSED));
    protect(page, true);
    fill(world, target);
    let before = FAULTS.load(Ordering::SeqCst);
    let mut regs = seed(0x8D5);
    aim(&mut regs, &store, target);
    regs.gpr[0] = 0x5A5A_1234;
    call(patched, &mut regs);
    assert_eq!(FAULTS.load(Ordering::SeqCst), before);
    assert_eq!(unsafe { core::ptr::read_volatile(target as *const u64) }, 0x5A5A_1234);
    assert!(taken(page, page + PAGE).is_empty());
    assert_eq!(cards::armed(page), cards::REFUSED);
    cards::arm(page, 0);
}

#[test]
fn card_runs() {
    let (world, _guard) = world();
    let base = world.primary + 0x2000 * PAGE;
    for page in [0u64, 1, 2, 7, 8, 9, 40, 100] {
        assert!(cards::mark(base + page * PAGE));
    }
    assert!(cards::any(base, base + 128 * PAGE));
    assert!(!cards::any(base + 3 * PAGE, base + 7 * PAGE));
    assert_eq!(taken(base + PAGE, base + 9 * PAGE), vec![(base + PAGE, base + 3 * PAGE), (base + 7 * PAGE, base + 9 * PAGE)]);
    let mut runs = [0u64; 4];
    let mut next = 0u64;
    let count = unsafe { cards::take(base, base + 128 * PAGE, runs.as_mut_ptr(), 2, &mut next) };
    assert_eq!(count, 2);
    assert_eq!(&runs, &[base, base + PAGE, base + 9 * PAGE, base + 10 * PAGE]);
    assert!(next < base + 128 * PAGE);
    assert_eq!(taken(next, base + 128 * PAGE), vec![(base + 40 * PAGE, base + 41 * PAGE), (base + 100 * PAGE, base + 101 * PAGE)]);
    assert!(!cards::any(base, base + 128 * PAGE));
    assert!(!cards::mark(world.primary + GUEST_BYTES + PAGE));
    assert!(!cards::arm(world.primary + GUEST_BYTES + PAGE, 4096));
    assert!(!cards::arm(base, 100));
}

#[test]
fn sites_are_corrected_under_running_threads() {
    let (world, _guard) = world();
    let delta = world.alias.wrapping_sub(world.primary) as i64;
    let bytes = [0x48, 0x89, 0x87, 0x00, 0x01, 0x00, 0x00];
    for round in 0..24u64 {
        let code = stub(world, &bytes);
        let page = world.primary + (0x8000 + round) * PAGE;
        cards::arm(page, 0);
        protect(page, true);
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let started = std::sync::Arc::new(std::sync::Barrier::new(5));
        let mut threads = Vec::new();
        for thread in 0..4u64 {
            let stop = stop.clone();
            let started = started.clone();
            threads.push(std::thread::spawn(move || {
                alt_stack();
                started.wait();
                let mut count = 0u64;
                while !stop.load(Ordering::Relaxed) {
                    let mut regs = seed(0);
                    regs.gpr[7] = page + thread * 64 - 0x100;
                    regs.gpr[0] = count;
                    call(code, &mut regs);
                    assert_eq!(regs.gpr[0], count);
                    count += 1;
                }
                (thread, count)
            }));
        }
        started.wait();
        std::thread::sleep(std::time::Duration::from_millis(2));
        protect(page, false);
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
        while unsafe { core::ptr::read_volatile(code as *const u8) } != 0xE9 || cards::armed(page) == 0 {
            assert!(std::time::Instant::now() < deadline, "the site was never corrected");
            std::thread::yield_now();
        }
        std::thread::sleep(std::time::Duration::from_millis(2));
        stop.store(true, Ordering::Relaxed);
        for thread in threads {
            let (thread, count) = thread.join().unwrap();
            assert!(count > 0);
            let stored = unsafe { core::ptr::read_volatile((world.alias + (page - world.primary) + thread * 64) as *const u64) };
            assert_eq!(stored, count - 1, "the last write of thread {thread} is in memory");
        }
        assert_eq!(cards::armed(page), delta);
        assert_eq!(taken(page, page + PAGE), vec![(page, page + PAGE)]);
        cards::arm(page, 0);
        protect(page, true);
    }
}

#[test]
fn recorded_sites() {
    let Ok(path) = std::env::var("KYTY_SELFMARK_SITES") else {
        return;
    };
    let (world, _guard) = world();
    let text = std::fs::read_to_string(&path).expect("KYTY_SELFMARK_SITES names a readable file");
    let mut total = 0u64;
    let mut accepted = 0u64;
    let mut executed = 0u64;
    let mut sites_executed = 0u64;
    let mut reasons = std::collections::BTreeMap::<String, u64>::new();
    for (slot, line) in text.lines().enumerate() {
        let mut fields = line.split_whitespace();
        let (Some(_), Some(weight), Some(hex)) = (fields.next(), fields.next(), fields.next()) else {
            continue;
        };
        let weight: u64 = weight.parse().unwrap();
        let bytes: Vec<u8> = (0..hex.len() / 2).map(|at| u8::from_str_radix(&hex[at * 2..at * 2 + 2], 16).unwrap()).collect();
        total += weight;
        match decode::decode(&bytes) {
            Err(refusal) => *reasons.entry(format!("{refusal:?}")).or_default() += weight,
            Ok(store) => {
                accepted += weight;
                if store.index == Some(store.base) {
                    *reasons.entry("harness: base is the index".into()).or_default() += weight;
                    continue;
                }
                let checked = check(world, &bytes[..store.len as usize], 0x400 + slot as u64);
                assert_eq!(checked.refusal, None);
                executed += weight;
                sites_executed += 1;
            }
        }
    }
    println!("recorded sites: {total} faults, {:.1} % from sites the decoder accepts, {:.1} % executed and checked ({sites_executed} sites)", accepted as f64 * 100.0 / total as f64, executed as f64 * 100.0 / total as f64);
    for (reason, weight) in reasons {
        println!("   {reason}: {:.1} %", weight as f64 * 100.0 / total as f64);
    }
}
