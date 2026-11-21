pub const PAD: u8 = 0x2E;
pub const JUMP: u8 = 0xE9;
pub const JUMP_LEN: usize = 5;
pub const MAX_PADS: usize = 3;
pub const LOWEST: u64 = 0x1_0000;
pub const HIGHEST: u64 = 1 << 47;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Window {
    pub pads: u8,
    pub free: u8,
    pub low: u64,
    pub count: u64,
}

pub fn good(byte: u8) -> bool {
    (0x08..0x80).contains(&byte)
}

pub fn borrowed(len: usize, pads: usize) -> usize {
    pads + JUMP_LEN - len
}

pub fn window(site: u64, len: usize, pads: usize, after: &[u8]) -> Option<Window> {
    if len < 2 || len >= JUMP_LEN || pads >= len {
        return None;
    }
    let free = len - pads - 1;
    let fixed = borrowed(len, pads);
    if after.len() < fixed {
        return None;
    }
    let mut raw = [0u8; 4];
    raw[free..].copy_from_slice(&after[..fixed]);
    let origin = site.checked_add(pads as u64 + JUMP_LEN as u64)?;
    let low = (origin as i64).checked_add(i32::from_le_bytes(raw) as i64)?;
    let count = 1u64 << (8 * free);
    if low < LOWEST as i64 || low as u64 + count + JUMP_LEN as u64 > HIGHEST {
        return None;
    }
    Some(Window { pads: pads as u8, free: free as u8, low: low as u64, count })
}

pub fn encode(site: u64, len: usize, pads: usize, after: &[u8], target: u64) -> Option<[u8; 4]> {
    let window = window(site, len, pads, after)?;
    if target < window.low || target - window.low >= window.count {
        return None;
    }
    let free = window.free as usize;
    let distance = target.wrapping_sub(site + pads as u64 + JUMP_LEN as u64) as i64;
    if distance != distance as i32 as i64 {
        return None;
    }
    let raw = (distance as i32).to_le_bytes();
    if raw[free..] != after[..borrowed(len, pads)] {
        return None;
    }
    let mut bytes = [0xCCu8; 4];
    for slot in bytes[..pads].iter_mut() {
        *slot = PAD;
    }
    bytes[pads] = JUMP;
    bytes[pads + 1..len].copy_from_slice(&raw[..free]);
    Some(bytes)
}

pub fn long(site: u64, len: usize, pads: usize, target: u64) -> Option<[u8; 16]> {
    if len > 15 || pads > MAX_PADS || pads + JUMP_LEN > len {
        return None;
    }
    let distance = target.wrapping_sub(site + pads as u64 + JUMP_LEN as u64) as i64;
    if distance != distance as i32 as i64 {
        return None;
    }
    let mut bytes = [0xCCu8; 16];
    for slot in bytes[..pads].iter_mut() {
        *slot = PAD;
    }
    bytes[pads] = JUMP;
    bytes[pads + 1..pads + JUMP_LEN].copy_from_slice(&(distance as i32).to_le_bytes());
    Some(bytes)
}

pub fn hop(at: u64, target: u64) -> Option<[u8; JUMP_LEN]> {
    let distance = target.wrapping_sub(at + JUMP_LEN as u64) as i64;
    if distance != distance as i32 as i64 {
        return None;
    }
    let mut bytes = [JUMP; JUMP_LEN];
    bytes[1..].copy_from_slice(&(distance as i32).to_le_bytes());
    Some(bytes)
}

pub fn follow(code: &[u8], at: u64) -> Option<(usize, u64)> {
    let mut pads = 0;
    while pads < code.len() && pads <= MAX_PADS && code[pads] == PAD {
        pads += 1;
    }
    if pads > MAX_PADS || code.len() < pads + JUMP_LEN || code[pads] != JUMP {
        return None;
    }
    let mut raw = [0u8; 4];
    raw.copy_from_slice(&code[pads + 1..pads + JUMP_LEN]);
    Some((pads, at.wrapping_add((pads + JUMP_LEN) as u64).wrapping_add(i32::from_le_bytes(raw) as i64 as u64)))
}

pub fn pads_for(len: usize) -> usize {
    (len - JUMP_LEN).min(MAX_PADS)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Random(u64);

    impl Random {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }
    }

    #[test]
    fn a_window_is_fixed_by_the_bytes_that_follow() {
        let after = [0x48, 0x8D, 0x8F, 0x30];
        assert_eq!(window(0x105_9B37, 2, 0, &after), None);
        assert_eq!(window(0x105_9B37, 2, 1, &after), Some(Window { pads: 1, free: 0, low: 0x105_9B37 + 6 + 0x308F_8D48, count: 1 }));
        let after = [0x5D, 0xC3, 0x48, 0x8B];
        assert_eq!(window(0x7000_0000_4218, 3, 1, &after), Some(Window { pads: 1, free: 1, low: 0x7000_0000_4218 + 6 + 0x48C3_5D00, count: 256 }));
        assert_eq!(window(0x7000_0000_4218, 3, 0, &after), Some(Window { pads: 0, free: 2, low: 0x7000_0000_4218 + 5 - 0x3CA3_0000, count: 65536 }));
        assert_eq!(window(0x40_0000, 4, 0, &[0x48]), Some(Window { pads: 0, free: 3, low: 0x40_0005 + 0x4800_0000, count: 1 << 24 }));
        assert_eq!(window(0x40_0000, 4, 0, &[0xC5]), None);
        assert_eq!(window(0x40_0000, 4, 4, &after), None);
        assert_eq!(window(0x40_0000, 5, 0, &after), None);
        assert_eq!(window(0x40_0000, 1, 0, &after), None);
        assert_eq!(window(0x40_0000, 2, 1, &after[..3]), None);
        assert_eq!(window(HIGHEST - 0x1000, 4, 0, &[0x7F]), None);
    }

    #[test]
    fn every_encoding_decodes_to_its_target_and_leaves_the_next_bytes_alone() {
        let mut random = Random(0x9E37_79B9_7F4A_7C15);
        let mut encoded = 0u64;
        for round in 0..400_000u64 {
            let len = 2 + (random.next() % 3) as usize;
            let pads = (random.next() % len as u64) as usize;
            let site = match round % 3 {
                0 => 0x40_0000 + random.next() % 0x180_0000,
                1 => 0x7000_0000_0000 + random.next() % 0x1000_0000,
                _ => LOWEST + random.next() % (HIGHEST - LOWEST),
            };
            let mut after = [0u8; 8];
            for slot in after.iter_mut() {
                *slot = random.next() as u8;
            }
            let Some(window) = window(site, len, pads, &after) else {
                assert_eq!(encode(site, len, pads, &after, site), None);
                continue;
            };
            assert_eq!(window.free as usize + borrowed(len, pads), 4);
            assert_eq!(window.count, 1 << (8 * window.free));
            for target in [window.low, window.low + window.count - 1, window.low + random.next() % window.count] {
                let bytes = encode(site, len, pads, &after, target).expect("a target of the window encodes");
                let mut code = [0u8; 12];
                code[..len].copy_from_slice(&bytes[..len]);
                code[len..len + 8].copy_from_slice(&after);
                assert_eq!(follow(&code, site), Some((pads, target)), "site {site:x} len {len} pads {pads} after {after:02x?}");
                assert!(bytes[len..].iter().all(|&byte| byte == 0xCC));
                encoded += 1;
            }
            assert_eq!(encode(site, len, pads, &after, window.low + window.count), None);
            assert_eq!(encode(site, len, pads, &after, window.low.wrapping_sub(1)), None);
        }
        assert!(encoded > 300_000);
    }

    #[test]
    fn long_jumps_and_hops() {
        let bytes = long(0x40_0000, 8, 3, 0x50_0000).unwrap();
        assert_eq!(&bytes[..8], &[0x2E, 0x2E, 0x2E, 0xE9, 0xF8, 0xFF, 0x0F, 0x00]);
        assert_eq!(follow(&bytes, 0x40_0000), Some((3, 0x50_0000)));
        let bytes = long(0x40_0000, 5, 0, 0x3F_0000).unwrap();
        assert_eq!(follow(&bytes, 0x40_0000), Some((0, 0x3F_0000)));
        assert_eq!(long(0x40_0000, 5, 1, 0x50_0000), None);
        assert_eq!(long(0x40_0000, 7, 0, 0x1_0000_0000_0000), None);
        assert_eq!((pads_for(5), pads_for(6), pads_for(7), pads_for(8), pads_for(15)), (0, 1, 2, 3, 3));
        let bytes = hop(0x7000_0000_1234, 0x7000_0800_0000).unwrap();
        assert_eq!(follow(&bytes, 0x7000_0000_1234), Some((0, 0x7000_0800_0000)));
        assert_eq!(hop(0x1_0000, 0x1_0000_0000), None);
        assert_eq!(follow(&[0x2E, 0x2E, 0x2E, 0x2E, 0xE9, 0, 0, 0, 0], 0), None);
        assert_eq!(follow(&[0x2E, 0xE8, 0, 0, 0, 0], 0), None);
        assert_eq!(follow(&[0xE9, 0, 0, 0], 0), None);
    }

    #[test]
    fn good_bytes_point_forward_and_away() {
        assert!(good(0x2E) && good(0x08) && good(0x7F));
        assert!(!good(0x00) && !good(0x07) && !good(0x80) && !good(0xE9) && !good(0xFF));
    }
}
