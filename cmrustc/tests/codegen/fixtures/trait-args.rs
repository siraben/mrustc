#![feature(no_core)]
#![no_core]
#![no_main]

// Two impls of one trait for the same Self, differing only in the trait
// argument: the call must select by the trait argument (core's
// `impl SliceIndex<ByteStr> for usize` precedes `impl SliceIndex<[T]>`).
struct Bytes {
    data: [u8; 4],
}

trait Index<S: ?Sized> {
    type Out;
    fn get(self, s: &S) -> Self::Out;
}

impl Index<Bytes> for usize {
    type Out = u32;
    fn get(self, s: &Bytes) -> u32 {
        s.data[self] as u32 + 1000
    }
}

impl<T: Copy> Index<[T]> for usize {
    type Out = T;
    fn get(self, s: &[T]) -> T {
        s[self]
    }
}

trait Copy {}
impl Copy for u32 {}
impl Copy for u8 {}

fn lookup<T: Copy, I: Index<[T]>>(s: &[T], i: I) -> I::Out {
    i.get(s)
}

fn lookup_bytes<I: Index<Bytes>>(b: &Bytes, i: I) -> I::Out {
    i.get(b)
}

// A method bound through a supertrait on a parameter (`T: Step`, with
// `Step: Dup`) may be recorded against whichever impl tyck saw first
// (an array impl); the emitter must re-resolve it for the concrete
// receiver (core's `self.start.clone()` in `Range::spec_next`).
trait Dup {
    fn dup(&self) -> Self;
}

impl<T: Dup + Copy, const N: usize> Dup for [T; N] {
    fn dup(&self) -> Self {
        *self
    }
}

impl Dup for u32 {
    fn dup(&self) -> u32 {
        *self + 1
    }
}

trait Step: Dup {
    fn forward(self) -> Self;
}

impl Step for u32 {
    fn forward(self) -> u32 {
        self + 10
    }
}

trait Trusted: Step + Copy {}
impl Trusted for u32 {}

// A blanket impl over a bare `T` with a same-named method, not in scope
// at the call (core's `impl<T: Clone> SpecArrayClone for T { fn clone }`):
// a receiver typed by a parameter must resolve `dup` through its bounds.
mod hidden {
    pub trait Other {
        fn dup(&self) -> u32;
    }
    impl<T: super::Dup> Other for T {
        fn dup(&self) -> u32 {
            7777
        }
    }
}

fn stepper<T: Trusted>(start: T) -> T {
    let old = start.dup();
    old.forward()
}

// The receiver as a field of a generic struct inside an impl whose bound
// carries the supertrait chain (core's `Range<T>::spec_next`).
struct Span<A> {
    start: A,
    end: A,
}

trait SpanImpl {
    fn bump(&mut self) -> u32;
}

impl<T: Trusted + Into32> SpanImpl for Span<T> {
    fn bump(&mut self) -> u32 {
        let old = self.start.dup();
        self.start = old.forward();
        self.start.to32() + self.end.to32()
    }
}

trait Into32 {
    fn to32(&self) -> u32;
}

impl Into32 for u32 {
    fn to32(&self) -> u32 {
        *self
    }
}

// A receiver typed by a projection that normalizes to a concrete type
// (`<usize as Index<[T]>>::Out` = `Arg`) reaches the inherent method,
// not a blanket `impl<T> Show for &T` (core's `value.fmt(fmt)` in
// `fmt::run`, with `value: &<usize as SliceIndex<[Argument]>>::Output`).
struct Arg {
    n: u32,
}

impl Arg {
    fn show(&self) -> u32 {
        self.n * 2
    }
}

trait Show {
    fn show(&self) -> u32;
}

impl<T> Show for &T {
    fn show(&self) -> u32 {
        4242
    }
}

trait Pick<S: ?Sized> {
    type Out;
    fn pick(self, s: &S) -> &Self::Out;
}

impl Pick<[Arg]> for usize {
    type Out = Arg;
    fn pick(self, s: &[Arg]) -> &Arg {
        &s[self]
    }
}

fn run_show<I: Pick<[Arg], Out = Arg>>(args: &[Arg], i: I) -> u32 {
    let value = i.pick(args);
    value.show()
}

// A blanket impl over a bare parameter with an unsatisfied bound
// (core's `impl<F: FnPtr> Debug for F`) precedes the concrete impl in
// item order; the concrete impl must win for `bool`.
trait FnLike {}

trait Dbg2 {
    fn d(&self) -> u32;
}

impl<F: FnLike> Dbg2 for F {
    fn d(&self) -> u32 {
        9999
    }
}

impl Dbg2 for bool {
    fn d(&self) -> u32 {
        if *self { 7 } else { 8 }
    }
}

fn via2<T: Dbg2>(x: &T) -> u32 {
    let f: fn(&T) -> u32 = <T as Dbg2>::d;
    f(x) + x.d()
}

#[no_mangle]
pub extern "C" fn trait_args(i: u32) -> u32 {
    let table: [u32; 4] = [10, 20, 30, 40];
    let b = Bytes { data: [1, 2, 3, 4] };
    let idx = i as usize;
    // An index that is only a literal: rustc infers `usize` from the
    // `Index<[T]>` bound; the call must still select the `[T]` impl.
    let mut sp = Span { start: i, end: 2 };
    let bumped = sp.bump();
    let mut plain = 0;
    if i > 100 {
        plain = 1;
    }
    lookup(&table, idx) + lookup_bytes(&b, idx) + lookup(&table, plain)
        + stepper(i) * 10000 + bumped * 1000000
        + run_show(&[Arg { n: 3 }, Arg { n: i + 1 }], 1usize) * 100000000
        + via2(&(i > 1)) * 10
}
