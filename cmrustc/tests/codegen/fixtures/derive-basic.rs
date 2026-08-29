// Builtin derives synthesize impls (Clone/PartialEq/Eq here; Debug, Copy,
// Default share the expander): a local `core` alias supplies the traits.
#![feature(no_core)]
#![no_core]
#![no_main]
extern crate self as core;
pub mod clone { pub trait Clone { fn clone(&self) -> Self; } impl Clone for u32 { fn clone(&self) -> u32 { *self } } }
pub mod cmp { pub trait PartialEq { fn eq(&self, o: &Self) -> bool; } pub trait Eq {} impl PartialEq for u32 { fn eq(&self, o: &u32) -> bool { *self == *o } } }
pub mod marker { pub trait Copy {} }
#[derive(Clone, PartialEq, Eq)]
pub struct Pt<T> { x: T, y: u32 }
#[derive(Clone, PartialEq)]
pub enum Sh { Dot, Pair(u32, u32), Named { a: u32 } }
#[no_mangle]
pub extern "C" fn derive_probe(i: u32) -> u32 {
    let p = Pt { x: i, y: 5 };
    let q = core::clone::Clone::clone(&p);
    let s = Sh::Pair(i, 2);
    let t = core::clone::Clone::clone(&s);
    let same = core::cmp::PartialEq::eq(&p, &q);
    let diff = core::cmp::PartialEq::eq(&s, &Sh::Dot);
    let same2 = core::cmp::PartialEq::eq(&s, &t);
    q.x * 100 + q.y * 10 + (same as u32) * 4 + (diff as u32) * 2 + (same2 as u32)
}
