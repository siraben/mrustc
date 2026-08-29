#![no_core]
#![feature(no_core)]
pub const MAX_ADDR_LEN: usize = 7;
mod inner {
    pub struct Addr {
        pub bytes: [u8; crate::MAX_ADDR_LEN],
    }
    pub fn total(addr: &Addr) -> u32 {
        let mut sum = 0;
        let mut index = 0;
        while index < crate::MAX_ADDR_LEN {
            sum = sum + addr.bytes[index] as u32;
            index = index + 1;
        }
        sum
    }
}
#[no_mangle]
pub fn probe_crate_const_len(a: u8) -> u32 {
    let addr = inner::Addr { bytes: [a; crate::MAX_ADDR_LEN] };
    inner::total(&addr)
}
