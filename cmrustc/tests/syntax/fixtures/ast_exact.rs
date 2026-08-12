#![allow(dead_code)]

pub struct Pair {
    pub left: i32,
    right: bool,
}

struct Tuple(pub u8, usize);
struct Unit;

enum Choice {
    None,
    One(i32),
    Named { flag: bool },
}

type Bytes = [u8; 4];
const LIMIT: u32 = 4;
static mut FLAG: bool = true;

fn add((left, right): (i32, i32)) -> i32 {
    left + right
}

fn types(
    shared: &u8,
    unique: &mut u8,
    raw: *const u8,
    slice: &[u8],
    callback: fn(i32, bool) -> i32,
) -> ! {
    loop {}
}
