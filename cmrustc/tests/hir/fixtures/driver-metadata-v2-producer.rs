pub struct Wrapper(pub u32);

pub unsafe extern "C" fn widen(value: u32) -> u64 {
    value as u64
}

pub const LIMIT: u32 = 17;

pub static COUNTER: u64 = 0;
