#![feature(no_core)]
#![no_core]
#![no_main]

// `&str` values are references to a [data, len] pair; the intrinsics
// core's str::len / as_ptr bottom out in read it.
#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;
#[rustc_intrinsic]
pub unsafe fn offset<Ptr, Delta>(dst: Ptr, offset: Delta) -> Ptr;

fn text(sel: u32) -> &'static str {
    if sel == 0 { "hello" } else { "a\tb\n\u{e9}" }
}

#[no_mangle]
pub extern "C" fn str_len(sel: u32) -> usize {
    let s = text(sel);
    unsafe { ptr_metadata(s as *const str) }
}

#[no_mangle]
pub extern "C" fn str_byte(sel: u32, i: usize) -> u8 {
    let s = text(sel);
    let p = s as *const str as *const u8;
    unsafe { *offset(p, i as isize) }
}

const LUT: &[u8; 4] = b"ab\x01d";

fn lut(i: usize) -> u32 {
    LUT[i] as u32
}

fn bytes_len() -> usize {
    let s: &[u8] = b"xyz";
    unsafe { ptr_metadata(s) }
}

struct Wrap(u32, u32);

#[no_mangle]
pub extern "C" fn ctor_call(v: u32) -> u32 {
    let w = Wrap(v, 7);
    w.0 + w.1 + lut(2) * 1000 + lut(3) + bytes_len() as u32 * 10000
}
