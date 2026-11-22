pub const MAX_LEN: usize = 15;
pub const PATCH_LEN: usize = 5;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Refusal {
    NotStore = 2,
    RipRelative = 3,
    StackBase = 4,
    NoBase = 5,
    Prefix = 6,
    Encoding = 7,
    RegisterForm = 8,
    StackOperand = 9,
    NoScratch = 10,
    NotGuestCode = 11,
    OutOfReach = 12,
    CodeProtection = 13,
    TableFull = 14,
    NoCoreSync = 15,
    NoWindow = 16,
    Frozen = 17,
}

pub const REFUSAL_COUNT: usize = 18;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Form {
    Legacy,
    Vex2,
    Vex3,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Plain {
    Same,
    Opcode(u8),
    OneByte(u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Store {
    pub len: u8,
    pub form: Form,
    pub prefix_len: u8,
    pub rex: u8,
    pub has_rex: bool,
    pub vex: [u8; 3],
    pub escape: bool,
    pub opcode: u8,
    pub modrm_at: u8,
    pub mode: u8,
    pub reg: u8,
    pub rm: u8,
    pub has_sib: bool,
    pub sib: u8,
    pub disp_len: u8,
    pub disp: i32,
    pub imm_len: u8,
    pub base: u8,
    pub index: Option<u8>,
    pub scale: u8,
    pub width: u16,
    pub used: u16,
    pub high_byte: bool,
    pub plain: Plain,
    pub ext_x: bool,
    pub ext_b: bool,
}

impl Store {
    pub fn short(&self) -> bool {
        (self.len as usize) < PATCH_LEN
    }
}

struct Reader {
    at: *const u8,
    read: usize,
}

impl Reader {
    #[inline]
    unsafe fn next(&mut self) -> Option<u8> {
        if self.read >= MAX_LEN {
            return None;
        }
        let byte = core::ptr::read_volatile(self.at.add(self.read));
        self.read += 1;
        Some(byte)
    }
}

struct Class {
    width: u16,
    reg_is_gpr: bool,
    imm_len: u8,
    plain: Plain,
}

fn legacy_one(opcode: u8, reg: u8, wide: bool, word: bool, repeat: bool) -> Result<Class, Refusal> {
    if repeat {
        return Err(Refusal::Prefix);
    }
    let size = if wide {
        8
    } else if word {
        2
    } else {
        4
    };
    match opcode {
        0x88 => Ok(Class { width: 1, reg_is_gpr: true, imm_len: 0, plain: Plain::Same }),
        0x89 => Ok(Class { width: size, reg_is_gpr: true, imm_len: 0, plain: Plain::Same }),
        0xC6 if reg == 0 => Ok(Class { width: 1, reg_is_gpr: false, imm_len: 1, plain: Plain::Same }),
        0xC7 if reg == 0 => Ok(Class { width: size, reg_is_gpr: false, imm_len: if size == 2 { 2 } else { 4 }, plain: Plain::Same }),
        _ => Err(Refusal::NotStore),
    }
}

fn vector(opcode: u8, pp: u8, wide: bool, long: bool, vex: bool) -> Result<Class, Refusal> {
    let full: u16 = if long { 32 } else { 16 };
    let class = |width: u16, plain: Plain| Ok(Class { width, reg_is_gpr: false, imm_len: 0, plain });
    match (opcode, pp) {
        (0x11, 0) | (0x11, 1) => class(full, Plain::Same),
        (0x11, 2) => class(4, Plain::Same),
        (0x11, 3) => class(8, Plain::Same),
        (0x13, 0) | (0x13, 1) | (0x17, 0) | (0x17, 1) if !long => class(8, Plain::Same),
        (0x29, 0) | (0x29, 1) => class(full, Plain::Same),
        (0x2B, 0) | (0x2B, 1) => class(full, Plain::Opcode(0x29)),
        (0x7F, 1) | (0x7F, 2) => class(full, Plain::Same),
        (0xE7, 1) => class(full, Plain::Opcode(0x7F)),
        (0xD6, 1) if !long => class(8, Plain::Same),
        (0x7E, 1) if !long => class(if wide { 8 } else { 4 }, Plain::Same),
        (0xC3, 0) if !vex => Ok(Class { width: if wide { 8 } else { 4 }, reg_is_gpr: true, imm_len: 0, plain: Plain::OneByte(0x89) }),
        _ => Err(Refusal::NotStore),
    }
}

fn candidate(opcode: u8, escape: bool) -> bool {
    if escape {
        matches!(opcode, 0x11 | 0x13 | 0x17 | 0x29 | 0x2B | 0x7F | 0xE7 | 0xD6 | 0x7E | 0xC3)
    } else {
        matches!(opcode, 0x88 | 0x89 | 0xC6 | 0xC7)
    }
}

pub unsafe fn decode_at(code: *const u8) -> Result<Store, Refusal> {
    let mut reader = Reader { at: code, read: 0 };
    let mut word = false;
    let mut pp = 0u8;
    let mut mandatory = 0u8;
    let mut byte = reader.next().ok_or(Refusal::Encoding)?;
    loop {
        match byte {
            0x66 => {
                word = true;
                mandatory += 1;
                pp = 1;
            }
            0xF3 => {
                mandatory += 1;
                pp = 2;
            }
            0xF2 => {
                mandatory += 1;
                pp = 3;
            }
            0xF0 | 0x67 | 0x26 | 0x2E | 0x36 | 0x3E | 0x64 | 0x65 => return Err(Refusal::Prefix),
            _ => break,
        }
        byte = reader.next().ok_or(Refusal::Encoding)?;
    }
    if mandatory > 1 {
        return Err(Refusal::Prefix);
    }
    let prefix_len = (reader.read - 1) as u8;
    let mut rex = 0u8;
    let mut has_rex = false;
    let mut vex = [0u8; 3];
    let mut form = Form::Legacy;
    let mut escape = false;
    let mut long = false;
    let ext_r;
    let ext_x;
    let ext_b;
    let wide;
    let opcode;
    if byte == 0xC5 || byte == 0xC4 {
        if prefix_len != 0 {
            return Err(Refusal::Prefix);
        }
        vex[0] = byte;
        let second = reader.next().ok_or(Refusal::Encoding)?;
        vex[1] = second;
        let last;
        if byte == 0xC5 {
            form = Form::Vex2;
            ext_r = second & 0x80 == 0;
            ext_x = false;
            ext_b = false;
            wide = false;
            last = second;
        } else {
            form = Form::Vex3;
            if second & 0x1F != 1 {
                return Err(Refusal::NotStore);
            }
            ext_r = second & 0x80 == 0;
            ext_x = second & 0x40 == 0;
            ext_b = second & 0x20 == 0;
            let third = reader.next().ok_or(Refusal::Encoding)?;
            vex[2] = third;
            wide = third & 0x80 != 0;
            last = third;
        }
        if last & 0x78 != 0x78 {
            return Err(Refusal::NotStore);
        }
        long = last & 0x04 != 0;
        pp = last & 0x03;
        escape = true;
        opcode = reader.next().ok_or(Refusal::Encoding)?;
    } else {
        if byte & 0xF0 == 0x40 {
            rex = byte;
            has_rex = true;
            byte = reader.next().ok_or(Refusal::Encoding)?;
        }
        ext_r = rex & 0x04 != 0;
        ext_x = rex & 0x02 != 0;
        ext_b = rex & 0x01 != 0;
        wide = rex & 0x08 != 0;
        if byte == 0x0F {
            escape = true;
            opcode = reader.next().ok_or(Refusal::Encoding)?;
        } else {
            opcode = byte;
        }
    }
    if !candidate(opcode, escape) {
        return Err(Refusal::NotStore);
    }
    let modrm_at = reader.read as u8;
    let modrm = reader.next().ok_or(Refusal::Encoding)?;
    let mode = modrm >> 6;
    let reg_low = (modrm >> 3) & 7;
    let rm_low = modrm & 7;
    let class = if escape { vector(opcode, pp, wide, long, form != Form::Legacy)? } else { legacy_one(opcode, reg_low, wide, word, pp >= 2)? };
    if mode == 3 {
        return Err(Refusal::RegisterForm);
    }
    let mut has_sib = false;
    let mut sib = 0u8;
    let base;
    let mut index = None;
    let mut scale = 1u8;
    if rm_low == 4 {
        has_sib = true;
        sib = reader.next().ok_or(Refusal::Encoding)?;
        let index_low = (sib >> 3) & 7;
        let base_low = sib & 7;
        if base_low == 5 && mode == 0 {
            return Err(Refusal::NoBase);
        }
        base = base_low | if ext_b { 8 } else { 0 };
        let candidate = index_low | if ext_x { 8 } else { 0 };
        if candidate != 4 {
            index = Some(candidate);
            scale = 1 << (sib >> 6);
        }
    } else {
        if rm_low == 5 && mode == 0 {
            return Err(Refusal::RipRelative);
        }
        base = rm_low | if ext_b { 8 } else { 0 };
    }
    if base == 4 {
        return Err(Refusal::StackBase);
    }
    let disp_len: u8 = match mode {
        1 => 1,
        2 => 4,
        _ => 0,
    };
    let mut disp = 0i32;
    if disp_len == 1 {
        disp = reader.next().ok_or(Refusal::Encoding)? as i8 as i32;
    } else if disp_len == 4 {
        let mut raw = [0u8; 4];
        for slot in raw.iter_mut() {
            *slot = reader.next().ok_or(Refusal::Encoding)?;
        }
        disp = i32::from_le_bytes(raw);
    }
    for _ in 0..class.imm_len {
        reader.next().ok_or(Refusal::Encoding)?;
    }
    let reg = reg_low | if ext_r { 8 } else { 0 };
    let mut used = 1u16 << base;
    if let Some(register) = index {
        used |= 1 << register;
    }
    let mut high_byte = false;
    if class.reg_is_gpr {
        if class.width == 1 && !has_rex && reg_low >= 4 {
            high_byte = true;
            used |= 1 << (reg_low & 3);
        } else {
            used |= 1 << reg;
        }
    }
    if used & (1 << 4) != 0 {
        return Err(Refusal::StackOperand);
    }
    let len = reader.read as u8;
    Ok(Store {
        len,
        form,
        prefix_len,
        rex,
        has_rex,
        vex,
        escape,
        opcode,
        modrm_at,
        mode,
        reg,
        rm: rm_low,
        has_sib,
        sib,
        disp_len,
        disp,
        imm_len: class.imm_len,
        base,
        index,
        scale,
        width: class.width,
        used,
        high_byte,
        plain: class.plain,
        ext_x,
        ext_b,
    })
}

pub fn decode(bytes: &[u8]) -> Result<Store, Refusal> {
    let mut padded = [0u8; MAX_LEN + 1];
    let count = bytes.len().min(MAX_LEN);
    padded[..count].copy_from_slice(&bytes[..count]);
    let store = unsafe { decode_at(padded.as_ptr()) }?;
    if store.len as usize > bytes.len() {
        return Err(Refusal::Encoding);
    }
    Ok(store)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ok(bytes: &[u8]) -> Store {
        decode(bytes).unwrap_or_else(|refusal| panic!("{bytes:02x?} refused: {refusal:?}"))
    }

    fn no(bytes: &[u8]) -> Refusal {
        match decode(bytes) {
            Ok(store) => panic!("{bytes:02x?} accepted: {store:?}"),
            Err(refusal) => refusal,
        }
    }

    #[test]
    fn avx_store_with_long_displacement() {
        let store = ok(&[0xC5, 0xFC, 0x11, 0x81, 0xA0, 0x00, 0x00, 0x00]);
        assert_eq!(store.len, 8);
        assert_eq!(store.form, Form::Vex2);
        assert_eq!(store.base, 1);
        assert_eq!(store.index, None);
        assert_eq!(store.disp, 0xA0);
        assert_eq!(store.width, 32);
        assert_eq!(store.used, 1 << 1);
        assert_eq!(store.plain, Plain::Same);
    }

    #[test]
    fn three_byte_vex_with_index() {
        let store = ok(&[0xC4, 0xC1, 0x78, 0x11, 0x2C, 0x31]);
        assert_eq!(store.len, 6);
        assert_eq!(store.form, Form::Vex3);
        assert_eq!(store.base, 9);
        assert_eq!(store.index, Some(6));
        assert_eq!(store.scale, 1);
        assert_eq!(store.width, 16);
        assert_eq!(store.used, (1 << 9) | (1 << 6));
    }

    #[test]
    fn short_stores_are_decoded() {
        let cases: &[(&[u8], u8, u16, u8)] = &[
            (&[0xC5, 0xF9, 0xE7, 0x07], 4, 16, 7),
            (&[0x48, 0x89, 0x17], 3, 8, 7),
            (&[0x88, 0x0A], 2, 1, 2),
            (&[0x42, 0x89, 0x14, 0x33], 4, 4, 3),
            (&[0x89, 0x45, 0x00], 3, 4, 5),
            (&[0xC6, 0x00, 0x7F], 3, 1, 0),
            (&[0x66, 0x89, 0x01], 3, 2, 1),
            (&[0x0F, 0x11, 0x07], 3, 16, 7),
            (&[0x41, 0x88, 0x04, 0x24], 4, 1, 12),
        ];
        for (bytes, len, width, base) in cases {
            let store = ok(bytes);
            assert_eq!((store.len, store.width, store.base), (*len, *width, *base), "{bytes:02x?}");
            assert!(store.short(), "{bytes:02x?}");
        }
        assert!(!ok(&[0xC5, 0xF9, 0xE7, 0x4F, 0x10]).short());
        assert_eq!(no(&[0x89, 0x04, 0x24]), Refusal::StackBase);
        assert_eq!(no(&[0x89, 0x05]), Refusal::RipRelative);
        assert_eq!(no(&[0x01, 0x07]), Refusal::NotStore);
    }

    #[test]
    fn non_temporal_stores_have_a_plain_twin() {
        assert_eq!(ok(&[0xC5, 0xF9, 0xE7, 0x4F, 0x10]).plain, Plain::Opcode(0x7F));
        assert_eq!(ok(&[0x66, 0x0F, 0xE7, 0x47, 0x10]).plain, Plain::Opcode(0x7F));
        assert_eq!(ok(&[0x0F, 0x2B, 0x44, 0x08, 0x10]).plain, Plain::Opcode(0x29));
        let movnti = ok(&[0x48, 0x0F, 0xC3, 0x44, 0x08, 0x10]);
        assert_eq!(movnti.plain, Plain::OneByte(0x89));
        assert_eq!(movnti.width, 8);
        assert_eq!(movnti.used, (1 << 0) | (1 << 1));
    }

    #[test]
    fn general_stores() {
        let wide = ok(&[0x48, 0x89, 0x84, 0xCB, 0x00, 0x01, 0x00, 0x00]);
        assert_eq!((wide.len, wide.width, wide.base, wide.index, wide.scale, wide.disp), (8, 8, 3, Some(1), 8, 0x100));
        assert_eq!(wide.used, (1 << 0) | (1 << 3) | (1 << 1));
        let word = ok(&[0x66, 0x41, 0x89, 0x47, 0x10]);
        assert_eq!((word.len, word.width, word.base), (5, 2, 15));
        let immediate = ok(&[0xC7, 0x47, 0x10, 0x78, 0x56, 0x34, 0x12]);
        assert_eq!((immediate.len, immediate.width, immediate.imm_len), (7, 4, 4));
        let wide_immediate = ok(&[0x48, 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00]);
        assert_eq!((wide_immediate.len, wide_immediate.width), (7, 8));
        let word_immediate = ok(&[0x66, 0xC7, 0x47, 0x10, 0x34, 0x12]);
        assert_eq!((word_immediate.len, word_immediate.width, word_immediate.imm_len), (6, 2, 2));
        let byte_immediate = ok(&[0xC6, 0x84, 0x08, 0x00, 0x10, 0x00, 0x00, 0x7F]);
        assert_eq!((byte_immediate.len, byte_immediate.width), (8, 1));
    }

    #[test]
    fn high_byte_registers_are_noted() {
        let store = ok(&[0x88, 0xA7, 0x00, 0x01, 0x00, 0x00]);
        assert!(store.high_byte);
        assert_eq!(store.used, (1 << 7) | (1 << 0));
        let low = ok(&[0x40, 0x88, 0xB7, 0x00, 0x01, 0x00, 0x00]);
        assert!(!low.high_byte);
        assert_eq!(low.used, (1 << 7) | (1 << 6));
    }

    #[test]
    fn refusals() {
        assert_eq!(no(&[0x48, 0x89, 0x05, 0x00, 0x01, 0x00, 0x00]), Refusal::RipRelative);
        assert_eq!(no(&[0x48, 0x89, 0x44, 0x24, 0x10]), Refusal::StackBase);
        assert_eq!(no(&[0x48, 0x89, 0x04, 0x25, 0x00, 0x10, 0x00, 0x00]), Refusal::NoBase);
        assert_eq!(no(&[0xF0, 0x48, 0x01, 0x47, 0x10]), Refusal::Prefix);
        assert_eq!(no(&[0x64, 0x48, 0x89, 0x47, 0x10]), Refusal::Prefix);
        assert_eq!(no(&[0x67, 0x48, 0x89, 0x47, 0x10]), Refusal::Prefix);
        assert_eq!(no(&[0x48, 0x01, 0x47, 0x10, 0x90]), Refusal::NotStore);
        assert_eq!(no(&[0x48, 0x8B, 0x87, 0x00, 0x01, 0x00, 0x00]), Refusal::NotStore);
        assert_eq!(no(&[0x48, 0x89, 0xC8, 0x90, 0x90]), Refusal::RegisterForm);
        assert_eq!(no(&[0x48, 0x89, 0xA7, 0x00, 0x01, 0x00, 0x00]), Refusal::StackOperand);
        assert_eq!(no(&[0xF3, 0x48, 0x89, 0x47, 0x10]), Refusal::Prefix);
        assert_eq!(no(&[0x62, 0xF1, 0x7C, 0x48, 0x11, 0x47, 0x01]), Refusal::NotStore);
        assert_eq!(no(&[0xC5, 0xFA, 0x7E, 0x47, 0x10]), Refusal::NotStore);
        assert_eq!(no(&[0x0F, 0x7F, 0x47, 0x10, 0x90]), Refusal::NotStore);
        assert_eq!(no(&[0xC5, 0xE8, 0x11, 0x47, 0x10]), Refusal::NotStore);
        assert_eq!(no(&[0xC7, 0x4F, 0x10, 0x78, 0x56, 0x34, 0x12]), Refusal::NotStore);
    }

    #[test]
    fn truncated_bytes_are_refused() {
        assert_eq!(no(&[0xC5, 0xFC, 0x11, 0x81, 0xA0, 0x00]), Refusal::Encoding);
    }
}
