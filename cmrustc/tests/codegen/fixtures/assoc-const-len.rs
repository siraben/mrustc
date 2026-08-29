#![no_core]
#![feature(no_core)]
struct Group(u64);
impl Group {
    const WIDTH: usize = 4;
    fn tags(&self) -> [u32; Self::WIDTH] {
        [1, 2, 3, 4]
    }
}
fn widen(tags: [u32; Group::WIDTH]) -> u32 {
    let mut total = 0;
    let mut index = 0;
    while index < Group::WIDTH {
        total = total + tags[index];
        index = index + 1;
    }
    total
}
#[no_mangle]
pub fn probe_assoc_const_len(a: u32) -> u32 {
    let g = Group(7);
    let tags: [u32; Group::WIDTH] = g.tags();
    widen(tags) + a
}
