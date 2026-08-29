#![feature(no_core)]
#![no_core]
#![no_main]

// A method found through an overloaded `Deref` (`Vec<T>` -> `[T]`,
// `String` -> `str`) must be called on `Deref::deref(&receiver)`, not on
// the wrapper itself.
#[lang = "deref"]
trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

#[lang = "deref_mut"]
trait DerefMut: Deref {
    fn deref_mut(&mut self) -> &mut Self::Target;
}

struct Inner {
    a: u32,
    b: u32,
}

impl Inner {
    fn total(&self) -> u32 {
        self.a + self.b
    }
    fn bump(&mut self) {
        self.a = self.a + 1;
    }
}

struct Wrapper {
    tag: u32,
    inner: Inner,
}

impl Deref for Wrapper {
    type Target = Inner;
    fn deref(&self) -> &Inner {
        &self.inner
    }
}

impl DerefMut for Wrapper {
    fn deref_mut(&mut self) -> &mut Inner {
        &mut self.inner
    }
}

#[no_mangle]
pub extern "C" fn deref_method(x: u32) -> u32 {
    let mut w = Wrapper { tag: 7, inner: Inner { a: x, b: 2 } };
    w.bump();
    let t = w.total();
    let r = &w;
    t * 100 + r.total() + w.tag * 10000
}
