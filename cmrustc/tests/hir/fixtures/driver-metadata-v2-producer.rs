// A named-field struct: the bounded v2 declaration format transports public
// types and values but deliberately rejects public tuple-struct constructor
// bindings instead of omitting them.
pub struct Wrapper {
    pub value: u32,
}

pub unsafe extern "C" fn widen(value: u32) -> u64 {
    value as u64
}

pub const LIMIT: u32 = 17;

pub static COUNTER: u64 = 0;
