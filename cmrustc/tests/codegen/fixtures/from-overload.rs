#![feature(no_core)]
#![no_core]
#![no_main]

// Two `From` impls on one type whose source types differ only by a
// reference: `Self::from(Inner::from(r))` inside the `&mut T` impl must
// pick the `Inner<T>` impl, not recurse (core's `Unique<T>: From<&mut T>`).
trait From<T> {
    fn from(value: T) -> Self;
}

struct Inner<T> {
    ptr: *mut T,
}

impl<T> Inner<T> {
    fn cast<U>(self) -> Inner<U> {
        Inner { ptr: self.ptr as *mut U }
    }
}

impl<T> From<&mut T> for Inner<T> {
    fn from(r: &mut T) -> Inner<T> {
        Inner { ptr: r as *mut T }
    }
}

struct Outer<T> {
    inner: Inner<T>,
    tag: u32,
}

impl<T> From<Inner<T>> for Outer<T> {
    fn from(inner: Inner<T>) -> Outer<T> {
        Outer { inner: inner, tag: 1 }
    }
}

impl<T> From<&mut T> for Outer<T> {
    fn from(r: &mut T) -> Outer<T> {
        let mut o = Self::from(Inner::from(r));
        o.tag = o.tag + 10;
        o
    }
}

// The argument's shape selects the impl and binds the callee's own
// generic: `Outer::from(i.cast())` is `From<Inner<T>>`, so `cast::<U>`
// gets `U = T` (raw_vec's `self.ptr = Unique::from(ptr.cast())`).
struct Holder<T> {
    o: Outer<T>,
}

impl<T> Holder<T> {
    fn set(&mut self, i: Inner<u8>) {
        self.o = Outer::from(i.cast());
    }
}

#[no_mangle]
pub extern "C" fn from_overload(x: u32) -> u32 {
    let mut v = x;
    let o = Outer::from(&mut v);
    let p = o.inner.ptr;
    unsafe { *p = *p + 1; }
    let mut bytes = x as u8;
    let mut h = Holder { o: Outer::from(&mut v) };
    h.set(Inner::from(&mut bytes));
    unsafe { *h.o.inner.ptr = 3; }
    v + o.tag + h.o.tag * 1000 + bytes as u32 * 100
}
