use crate::decode::{Form, Plain, Refusal, Store};

pub const SLOT: usize = 384;
pub const SITE_AT: usize = 368;
pub const SLOW_AT: usize = 376;
const CODE_ROOM: usize = SITE_AT;
pub const LEAST_DELTA: u32 = 0x1000;

const CANDIDATES: [u8; 11] = [6, 7, 3, 2, 1, 8, 9, 10, 11, 14, 15];

#[derive(Clone, Copy, Debug)]
pub struct Tables {
    pub delta: u64,
    pub cards: u64,
    pub pages: u32,
}

pub struct Built {
    pub bytes: [u8; SLOT],
    pub slow_at: usize,
}

struct Code {
    bytes: [u8; SLOT],
    len: usize,
    overflow: bool,
}

impl Code {
    fn put(&mut self, byte: u8) {
        if self.len < CODE_ROOM {
            self.bytes[self.len] = byte;
            self.len += 1;
        } else {
            self.overflow = true;
        }
    }

    fn all(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.put(byte);
        }
    }

    fn imm32(&mut self, value: u32) {
        self.all(&value.to_le_bytes());
    }

    fn imm64(&mut self, value: u64) {
        self.all(&value.to_le_bytes());
    }

    fn hole(&mut self) -> usize {
        let at = self.len;
        self.imm32(0);
        at
    }

    fn fill(&mut self, hole: usize, value: i32) {
        if hole + 4 <= CODE_ROOM {
            self.bytes[hole..hole + 4].copy_from_slice(&value.to_le_bytes());
        }
    }
}

fn rex(wide: bool, reg: u8, index: bool, base: u8) -> u8 {
    0x40 | if wide { 8 } else { 0 } | if reg >= 8 { 4 } else { 0 } | if index { 2 } else { 0 } | if base >= 8 { 1 } else { 0 }
}

fn direct(reg: u8, rm: u8) -> u8 {
    0xC0 | ((reg & 7) << 3) | (rm & 7)
}

fn push(code: &mut Code, register: u8) {
    if register >= 8 {
        code.put(0x41);
    }
    code.put(0x50 + (register & 7));
}

fn pop(code: &mut Code, register: u8) {
    if register >= 8 {
        code.put(0x41);
    }
    code.put(0x58 + (register & 7));
}

fn restore_flags(code: &mut Code) {
    code.all(&[0x04, 0x7F, 0x9E]);
}

pub fn scratch(store: &Store) -> Result<(u8, u8, u8), Refusal> {
    let mut picked = [0u8; 3];
    let mut count = 0;
    for &register in CANDIDATES.iter() {
        if store.used & (1 << register) != 0 {
            continue;
        }
        picked[count] = register;
        count += 1;
        if count == 3 {
            break;
        }
    }
    if count < 3 || (store.high_byte && picked[0] >= 8) {
        return Err(Refusal::NoScratch);
    }
    Ok((picked[0], picked[1], picked[2]))
}

fn address(code: &mut Code, store: &Store, original: &[u8], target: u8) {
    code.put(0x48 | if target >= 8 { 4 } else { 0 } | if store.ext_x { 2 } else { 0 } | if store.ext_b { 1 } else { 0 });
    code.put(0x8D);
    code.put((store.mode << 6) | ((target & 7) << 3) | store.rm);
    let tail = store.modrm_at as usize + 1;
    let extra = usize::from(store.has_sib) + store.disp_len as usize;
    code.all(&original[tail..tail + extra]);
}

fn write_through(code: &mut Code, store: &Store, original: &[u8], pointer: u8) {
    let modrm = ((store.reg & 7) << 3) | (pointer & 7);
    match store.form {
        Form::Legacy => {
            code.all(&original[..store.prefix_len as usize]);
            let prefix = (store.rex & 0x4C) | if pointer >= 8 { 1 } else { 0 };
            if store.has_rex {
                code.put(0x40 | prefix);
            } else if pointer >= 8 {
                code.put(0x41);
            }
            match store.plain {
                Plain::OneByte(opcode) => code.put(opcode),
                Plain::Opcode(opcode) => code.all(&[0x0F, opcode]),
                Plain::Same => {
                    if store.escape {
                        code.put(0x0F);
                    }
                    code.put(store.opcode);
                }
            }
        }
        Form::Vex2 | Form::Vex3 => {
            let opcode = match store.plain {
                Plain::Opcode(opcode) | Plain::OneByte(opcode) => opcode,
                Plain::Same => store.opcode,
            };
            if store.form == Form::Vex2 && pointer < 8 {
                code.all(&[0xC5, store.vex[1]]);
            } else {
                let (first, second) = if store.form == Form::Vex2 { ((store.vex[1] & 0x80) | 0x01, store.vex[1] & 0x7F) } else { (store.vex[1] & 0x9F, store.vex[2]) };
                code.all(&[0xC4, first | 0x40 | if pointer >= 8 { 0 } else { 0x20 }, second]);
            }
            code.put(opcode);
        }
    }
    code.put(modrm);
    let length = store.len as usize;
    code.all(&original[length - store.imm_len as usize..length]);
}

fn page_of(code: &mut Code, pointer: u8, page: u8, limit: u32, holes: &mut Holes) {
    code.all(&[rex(true, pointer, false, page), 0x89, direct(pointer, page)]);
    code.all(&[rex(true, 0, false, page), 0xC1, direct(5, page), 12]);
    code.all(&[rex(true, 0, false, page), 0x81, direct(7, page)]);
    code.imm32(limit);
    code.all(&[0x0F, 0x83]);
    holes.add(code.hole());
}

fn armed(code: &mut Code, table: u8, holes: &mut Holes) {
    code.all(&[rex(true, 0, false, table), 0x81, direct(7, table)]);
    code.imm32(LEAST_DELTA);
    code.all(&[0x0F, 0x82]);
    holes.add(code.hole());
}

fn finish(code: &mut Code, store: &Store, original: &[u8], pointer: u8, page: u8, table: u8, cards: u64, second: bool) -> usize {
    code.all(&[rex(true, table, false, pointer), 0x01, direct(table, pointer)]);
    code.all(&[rex(true, 0, false, table), 0xB8 + (table & 7)]);
    code.imm64(cards);
    code.all(&[rex(true, table, false, page), 0x01, direct(table, page)]);
    restore_flags(code);
    code.all(&[0x48, 0x8B, 0x44, 0x24, 0x18]);
    write_through(code, store, original, pointer);
    if page >= 8 {
        code.put(0x41);
    }
    code.all(&[0xC6, page & 7, 0x01]);
    if second {
        if page >= 8 {
            code.put(0x41);
        }
        code.all(&[0xC6, 0x40 | (page & 7), 0x01, 0x01]);
    }
    pop(code, table);
    pop(code, page);
    pop(code, pointer);
    code.all(&[0x48, 0x8D, 0xA4, 0x24, 0x88, 0x00, 0x00, 0x00]);
    code.put(0xE9);
    code.hole()
}

struct Holes {
    at: [usize; 8],
    count: usize,
}

impl Holes {
    fn add(&mut self, hole: usize) {
        self.at[self.count] = hole;
        self.count += 1;
    }
}

pub fn build(store: &Store, original: &[u8], site: u64, at: u64, tables: &Tables) -> Result<Built, Refusal> {
    let (pointer, page, table) = scratch(store)?;
    let length = store.len as usize;
    let back = site.wrapping_add(length as u64);
    let mut code = Code { bytes: [0xCC; SLOT], len: 0, overflow: false };
    let mut slow_holes = Holes { at: [0; 8], count: 0 };
    let mut back_holes = Holes { at: [0; 8], count: 0 };
    let mut cross_hole = None;

    code.all(&[0x48, 0x8D, 0x64, 0x24, 0x80]);
    push(&mut code, 0);
    push(&mut code, pointer);
    push(&mut code, page);
    push(&mut code, table);
    address(&mut code, store, original, pointer);
    code.all(&[0x9F, 0x0F, 0x90, 0xC0]);
    if store.width > 1 {
        code.all(&[rex(true, page, false, pointer), 0x8D, 0x40 | ((page & 7) << 3) | (pointer & 7), (store.width - 1) as u8]);
        code.all(&[rex(true, pointer, false, page), 0x31, direct(pointer, page)]);
        code.all(&[rex(true, 0, false, page), 0xC1, direct(5, page), 12]);
        code.all(&[0x0F, 0x85]);
        cross_hole = Some(code.hole());
    }
    page_of(&mut code, pointer, page, tables.pages, &mut slow_holes);
    code.all(&[rex(true, 0, false, table), 0xB8 + (table & 7)]);
    code.imm64(tables.delta);
    code.all(&[0x48 | if table >= 8 { 5 } else { 0 } | if page >= 8 { 2 } else { 0 }, 0x8B, ((table & 7) << 3) | 4, 0xC0 | ((page & 7) << 3) | (table & 7)]);
    armed(&mut code, table, &mut slow_holes);
    back_holes.add(finish(&mut code, store, original, pointer, page, table, tables.cards, false));

    if let Some(hole) = cross_hole {
        let cross = code.len;
        code.fill(hole, (cross - (hole + 4)) as i32);
        page_of(&mut code, pointer, page, tables.pages - 1, &mut slow_holes);
        code.all(&[rex(true, 0, false, table), 0xB8 + (table & 7)]);
        code.imm64(tables.delta);
        push(&mut code, 0);
        let indexed = 0x48 | if page >= 8 { 2 } else { 0 } | if table >= 8 { 1 } else { 0 };
        code.all(&[indexed, 0x8B, 0x44, 0xC0 | ((page & 7) << 3) | (table & 7), 0x08]);
        code.all(&[indexed | if table >= 8 { 4 } else { 0 }, 0x8B, ((table & 7) << 3) | 4, 0xC0 | ((page & 7) << 3) | (table & 7)]);
        code.all(&[rex(true, 0, false, table), 0x39, direct(0, table)]);
        pop(&mut code, 0);
        code.all(&[0x0F, 0x85]);
        slow_holes.add(code.hole());
        armed(&mut code, table, &mut slow_holes);
        back_holes.add(finish(&mut code, store, original, pointer, page, table, tables.cards, true));
    }

    let slow = code.len;
    restore_flags(&mut code);
    pop(&mut code, table);
    pop(&mut code, page);
    pop(&mut code, pointer);
    pop(&mut code, 0);
    code.all(&[0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00]);
    let slow_at = code.len;
    code.all(&original[..length]);
    code.put(0xE9);
    back_holes.add(code.hole());

    if code.overflow || tables.pages < 2 {
        return Err(Refusal::Encoding);
    }
    for &hole in slow_holes.at[..slow_holes.count].iter() {
        code.fill(hole, (slow - (hole + 4)) as i32);
    }
    for &hole in back_holes.at[..back_holes.count].iter() {
        let from = at.wrapping_add(hole as u64 + 4);
        let distance = back.wrapping_sub(from) as i64;
        if distance != distance as i32 as i64 {
            return Err(Refusal::OutOfReach);
        }
        code.fill(hole, distance as i32);
    }
    code.bytes[SITE_AT..SITE_AT + 8].copy_from_slice(&site.to_le_bytes());
    let word = slow_at as u64 | (length as u64) << 16 | (store.width as u64) << 24;
    code.bytes[SLOW_AT..SLOW_AT + 8].copy_from_slice(&word.to_le_bytes());
    Ok(Built { bytes: code.bytes, slow_at })
}
