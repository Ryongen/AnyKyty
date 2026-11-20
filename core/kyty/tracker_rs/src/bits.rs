use core::sync::atomic::{AtomicU64, Ordering::Relaxed};

pub const PAGES: usize = 1024;
pub const WORDS: usize = PAGES / 64;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Mask(pub [u64; WORDS]);

impl Default for Mask {
    #[inline]
    fn default() -> Self {
        Mask::EMPTY
    }
}

#[inline]
const fn start_mask(start: usize) -> u64 {
    !0u64 << (start % 64)
}

#[inline]
const fn end_mask(end: usize) -> u64 {
    let bit = (end - 1) % 64;
    if bit == 63 {
        !0u64
    } else {
        (1u64 << (bit + 1)) - 1
    }
}

#[inline]
fn for_each_word(start: usize, end: usize, mut f: impl FnMut(usize, u64) -> bool) -> bool {
    if start >= end || end > PAGES {
        return false;
    }
    let first = start / 64;
    let last = (end - 1) / 64;
    for word in first..=last {
        let mut mask = !0u64;
        if word == first {
            mask &= start_mask(start);
        }
        if word == last {
            mask &= end_mask(end);
        }
        if f(word, mask) {
            return true;
        }
    }
    false
}

impl Mask {
    pub const EMPTY: Mask = Mask([0; WORDS]);
    pub const FULL: Mask = Mask([!0; WORDS]);

    #[inline]
    pub fn masked(&self, start: usize, end: usize) -> Mask {
        let mut out = Mask::EMPTY;
        for_each_word(start, end, |word, mask| {
            out.0[word] = self.0[word] & mask;
            false
        });
        out
    }

    #[inline]
    pub fn not(&self) -> Mask {
        let mut out = *self;
        for word in out.0.iter_mut() {
            *word = !*word;
        }
        out
    }

    #[inline]
    pub fn or_assign(&mut self, other: &Mask) {
        for (word, value) in self.0.iter_mut().zip(other.0.iter()) {
            *word |= *value;
        }
    }

    #[inline]
    pub fn and_assign(&mut self, other: &Mask) {
        for (word, value) in self.0.iter_mut().zip(other.0.iter()) {
            *word &= *value;
        }
    }

    #[inline]
    pub fn get(&self, index: usize) -> bool {
        (self.0[index / 64] >> (index % 64)) & 1 != 0
    }

    #[inline]
    pub fn set(&mut self, index: usize) {
        self.0[index / 64] |= 1u64 << (index % 64);
    }

    #[inline]
    pub fn unset(&mut self, index: usize) {
        self.0[index / 64] &= !(1u64 << (index % 64));
    }

    #[inline]
    pub fn set_range(&mut self, start: usize, end: usize) {
        let words = &mut self.0;
        for_each_word(start, end, |word, mask| {
            words[word] |= mask;
            false
        });
    }

    #[inline]
    pub fn unset_range(&mut self, start: usize, end: usize) {
        let words = &mut self.0;
        for_each_word(start, end, |word, mask| {
            words[word] &= !mask;
            false
        });
    }

    #[inline]
    pub fn none(&self) -> bool {
        self.0.iter().fold(0u64, |acc, word| acc | *word) == 0
    }

    #[inline]
    pub fn any(&self, start: usize, end: usize) -> bool {
        for_each_word(start, end, |word, mask| self.0[word] & mask != 0)
    }

    #[inline]
    pub fn count(&self) -> u64 {
        self.0.iter().map(|word| word.count_ones() as u64).sum()
    }

    #[inline]
    pub fn first_range_from(&self, start: usize) -> (usize, usize) {
        first_range_from(start, |word| self.0[word])
    }

    #[inline]
    pub fn last_range_from(&self, end: usize) -> (usize, usize) {
        if end == 0 {
            return (0, 0);
        }
        let end = end.min(PAGES);
        let mut index = (end - 1) / 64;
        let mut word = self.0[index] & end_mask(end);
        while word == 0 {
            if index == 0 {
                return (0, 0);
            }
            index -= 1;
            word = self.0[index];
        }
        let empty = word.leading_zeros() as usize;
        let ones = (word << empty).leading_ones() as usize;
        let last = (index + 1) * 64 - empty;
        if empty + ones < 64 {
            return (last - ones, last);
        }
        while index != 0 {
            index -= 1;
            let word = self.0[index];
            if word != !0u64 {
                return ((index + 1) * 64 - word.leading_ones() as usize, last);
            }
        }
        (0, last)
    }

    #[inline]
    pub fn first_range(&self) -> (usize, usize) {
        self.first_range_from(0)
    }

    #[inline]
    pub fn last_range(&self) -> (usize, usize) {
        self.last_range_from(PAGES)
    }

    #[inline]
    pub fn ranges(&self) -> Ranges<'_> {
        Ranges { mask: self, next: self.first_range_from(0) }
    }
}

#[inline]
fn first_range_from(start: usize, load: impl Fn(usize) -> u64) -> (usize, usize) {
    if start >= PAGES {
        return (PAGES, PAGES);
    }
    let mut index = start / 64;
    let mut word = load(index) & start_mask(start);
    while word == 0 {
        index += 1;
        if index == WORDS {
            return (PAGES, PAGES);
        }
        word = load(index);
    }
    let first = index * 64 + word.trailing_zeros() as usize;
    let bit = first % 64;
    let ones = (word >> bit).trailing_ones() as usize;
    if bit + ones < 64 {
        return (first, first + ones);
    }
    for next in index + 1..WORDS {
        let word = load(next);
        if word != !0u64 {
            return (first, next * 64 + word.trailing_ones() as usize);
        }
    }
    (first, PAGES)
}

pub struct Ranges<'a> {
    mask: &'a Mask,
    next: (usize, usize),
}

impl Iterator for Ranges<'_> {
    type Item = (usize, usize);

    #[inline]
    fn next(&mut self) -> Option<(usize, usize)> {
        if self.next.0 >= PAGES {
            return None;
        }
        let current = self.next;
        self.next = self.mask.first_range_from(current.1);
        Some(current)
    }
}

#[repr(transparent)]
pub struct AtomicBits([AtomicU64; WORDS]);

impl AtomicBits {
    #[inline]
    fn load_word(&self, word: usize) -> u64 {
        self.0[word].load(Relaxed)
    }

    #[inline]
    fn store_word(&self, word: usize, value: u64) {
        self.0[word].store(value, Relaxed)
    }

    #[inline]
    pub fn load(&self) -> Mask {
        let mut out = Mask::EMPTY;
        for (word, value) in out.0.iter_mut().enumerate() {
            *value = self.load_word(word);
        }
        out
    }

    #[inline]
    pub fn masked(&self, start: usize, end: usize) -> Mask {
        let mut out = Mask::EMPTY;
        for_each_word(start, end, |word, mask| {
            out.0[word] = self.load_word(word) & mask;
            false
        });
        out
    }

    #[inline]
    pub fn not_masked(&self, start: usize, end: usize) -> Mask {
        let mut out = Mask::EMPTY;
        for_each_word(start, end, |word, mask| {
            out.0[word] = !self.load_word(word) & mask;
            false
        });
        out
    }

    #[inline]
    pub fn get(&self, index: usize) -> bool {
        (self.load_word(index / 64) >> (index % 64)) & 1 != 0
    }

    #[inline]
    pub fn unset(&self, index: usize) {
        let word = index / 64;
        self.store_word(word, self.load_word(word) & !(1u64 << (index % 64)));
    }

    #[inline]
    pub fn set_range(&self, start: usize, end: usize) {
        for_each_word(start, end, |word, mask| {
            self.store_word(word, self.load_word(word) | mask);
            false
        });
    }

    #[inline]
    pub fn unset_range(&self, start: usize, end: usize) {
        for_each_word(start, end, |word, mask| {
            self.store_word(word, self.load_word(word) & !mask);
            false
        });
    }

    #[inline]
    pub fn fill(&self) {
        for word in 0..WORDS {
            self.store_word(word, !0u64);
        }
    }

    #[inline]
    pub fn none(&self) -> bool {
        (0..WORDS).fold(0u64, |acc, word| acc | self.load_word(word)) == 0
    }

    #[inline]
    pub fn any(&self, start: usize, end: usize) -> bool {
        for_each_word(start, end, |word, mask| self.load_word(word) & mask != 0)
    }

    #[inline]
    pub fn first_range_from(&self, start: usize) -> (usize, usize) {
        first_range_from(start, |word| self.load_word(word))
    }
}
