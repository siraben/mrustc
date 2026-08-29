#![feature(no_core)]
#![no_core]
#![no_main]

// core's memrchr: `type Chunk = usize;` inside the fn body, used in a
// method turbofish, a cast and a let annotation.
#[lang = "sized"]
trait Sized {}

struct Pair<T> {
    a: T,
    b: T,
}

impl<T: Copy> Pair<T> {
    fn first<U>(&self) -> T { self.a }
}

#[lang = "copy"]
trait Copy {}
impl Copy for u32 {}

trait Tag {
    fn tag(&self) -> u32;
}
impl Tag for u32 {
    fn tag(&self) -> u32 { *self + 100 }
}

#[no_mangle]
pub extern "C" fn local_type_alias(x: u32) -> u32 {
    type Chunk = u32;
    let pair = Pair { a: x, b: x + 1 };
    let first: Chunk = pair.first::<(Chunk, Chunk)>();
    let p = &pair.b as *const Chunk;
    let second = unsafe { *p };
    // A method on a cast to the alias: without the alias the cast's
    // type is an inference variable and the call never resolves.
    let tagged = (x as Chunk).tag();
    first + second + tagged
}
