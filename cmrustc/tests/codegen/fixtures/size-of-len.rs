#![no_core]
#![feature(no_core)]
struct NonNull<T>(*const T);
const fn size_of<T>() -> usize { 8 }
const TAG_MASK: usize = 0b11;
const TAG_OS: usize = 0b01;
const _: [(); size_of::<NonNull<()>>()] = [(); 8];
const _: [(); size_of::<usize>()] = [(); 8];
const _: [(); TAG_MASK & TAG_OS] = [(); 1];
struct Arr { slots: [u8; size_of::<u64>()] }
#[no_mangle]
pub fn probe_size_of_len(a: u8) -> u32 {
    let x = Arr { slots: [a; size_of::<u64>()] };
    x.slots[7] as u32 + 34
}
