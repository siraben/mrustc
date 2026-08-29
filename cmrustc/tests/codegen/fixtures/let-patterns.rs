#![feature(no_core)]
#![no_core]
#![no_main]

// `if let` / `while let` test the discriminant and bind the payload;
// `for` drives `Iterator::next` and binds the item pattern.
enum Option<T> {
    None,
    Some(T),
}

trait Iterator {
    type Item;
    fn next(&mut self) -> Option<Self::Item>;
}

struct Counter {
    left: u32,
}

impl Iterator for Counter {
    type Item = u32;
    fn next(&mut self) -> Option<u32> {
        if self.left == 0 {
            Option::None
        } else {
            self.left = self.left - 1;
            Option::Some(self.left + 1)
        }
    }
}

// Nested sub-patterns under a variant: `Some(&v)` (core's
// `Option<&u8>::copied`) and `Some((a, b))`.
fn copied(x: Option<&u32>) -> u32 {
    match x {
        Option::Some(&v) => v,
        Option::None => 0,
    }
}

fn pair_sum(x: Option<(u32, u32)>) -> u32 {
    match x {
        Option::Some((a, b)) => a + b,
        Option::None => 0,
    }
}

fn pick(x: Option<u32>) -> u32 {
    if let Option::Some(v) = x { v + 1 } else { 100 }
}

#[no_mangle]
pub extern "C" fn let_patterns(n: u32) -> u32 {
    let mut total = pick(Option::Some(n)) + pick(Option::None);
    let mut counter = Counter { left: n };
    while let Option::Some(v) = counter.next() {
        total = total + v;
    }
    let second = Counter { left: 2 };
    for item in second {
        total = total + item * 10;
    }
    let pair = (n, 5u32);
    let (a, b) = pair;
    let seven = 7u32;
    total + a + b + copied(Option::Some(&seven)) * 100 + copied(Option::None)
        + pair_sum(Option::Some((n, 1))) * 1000
}
