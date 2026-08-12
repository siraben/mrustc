#![no_core]

#[repr(C)]
pub struct Pair<T> {
    pub left: T,
    right: T,
}

pub struct Tuple(pub u32, u8);
struct Unit;

struct Buffer<'a, const N: usize> {
    bytes: &'a [u8; N],
}

type Projection<T> = Associated<T, Item = u8, 4>;

enum Choice<T> {
    None,
    One(T),
    Record { value: T },
    Number = 3,
}

type Nested<T> = Option<Result<T, u8>>;
type Defaulted<T = u8> = T;
type BoundedDefault<T: Into<Vec<u8>> = u8> = T;
type Callable<F: FnMut(u8, Item) -> Result<u16, Error> + Send> = F;
type WhereCallable<F> where F: FnMut(u8) -> u16 = F;
type DeepDefault<T = Option<Result<u8, u16>>> = T;
type IteratorItem<T> = <T as Iterator>::Item;
type ExplicitProjection<'a, T> =
    <Option<T> as ::core::convert::Trait<'a, Result<T, u8>>>::Assoc<&'a T, 4>;
struct ConstDefault<const N: usize = { Pair { left: 1, right: 2 }.left }>;
const LIMIT: usize = 4;
pub static mut FLAG: bool = false;

mod nested {
    pub const fn identity<T>(value: T) -> T {
        value
    }

    pub const unsafe fn unchecked(value: usize) -> usize {
        value
    }
}

mod external;
use crate::nested::identity as call_identity;
extern crate core as rust_core;

extern "C" {
    fn foreign(value: i32) -> i32;
    static EXTERNAL: u8;
}

trait Show<T>: Sized + nested::Marker + ~const ConstMarker
where
    T: Copy,
{
    type Output;
    const VALUE: usize;
    fn show(&self, value: T) -> Self::Output;
}

trait Iterator {
    type Item;
}

trait PointeeSized {}

trait IntoIterator {
    type Item;
    type IntoIter: Iterator<Item = Self::Item>;
}

trait Deref: PointeeSized {
    type Target: ?Sized;
}

unsafe impl<T> Show<T> for Pair<T> where T: Copy {
    type Output = T;
    const VALUE: usize = 1;
    fn show(&self, value: T) -> T {
        value
    }
}

impl ! {}
impl !PointeeSized for Pair<u8> {}
