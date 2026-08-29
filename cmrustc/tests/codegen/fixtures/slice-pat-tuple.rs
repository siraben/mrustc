#![feature(no_core)]
#![no_core]
#![no_main]

// `match (pieces, args) { ([], []) => .., ([s], []) => .., _ => .. }` —
// core's `Arguments::as_str` shape: empty and one-element slice patterns
// inside a tuple.
fn classify(p: &[u32], a: &[u32]) -> u32 {
    match (p, a) {
        ([], []) => 1,
        ([s], []) => 2 + *s,
        _ => 3,
    }
}

#[no_mangle]
pub extern "C" fn slice_pat_tuple(x: u32) -> u32 {
    let none: [u32; 0] = [];
    let one = [x];
    let two = [x, x];
    let e: &[u32] = &none;
    let o: &[u32] = &one;
    let t: &[u32] = &two;
    classify(e, e) + classify(o, e) * 10 + classify(t, e) * 100 + classify(o, o) * 1000
}
