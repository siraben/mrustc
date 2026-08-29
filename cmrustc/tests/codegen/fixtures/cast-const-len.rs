#![no_core]
#![feature(no_core)]
const FD_SETSIZE: u32 = 1024;
const ULONG_SIZE: usize = 64;
struct FdSet {
    bits: [u64; FD_SETSIZE as usize / ULONG_SIZE],
}
#[no_mangle]
pub fn probe_cast_const_len(a: u64) -> u64 {
    let set = FdSet { bits: [a; FD_SETSIZE as usize / ULONG_SIZE] };
    let mut total = 0;
    let mut index = 0;
    while index < FD_SETSIZE as usize / ULONG_SIZE {
        total = total + set.bits[index];
        index = index + 1;
    }
    total
}
