#![feature(no_core)]
#![no_core]
#![no_main]

// fn items as values, calls through fn-pointer fields, five-argument
// calls, and a fn declared inside a nested block.
fn inc(x: u32) -> u32 {
    x + 1
}

struct Op {
    f: fn(u32) -> u32,
    tag: u32,
}

trait Show {
    fn show(&self, k: u32) -> u32;
}

impl Show for u32 {
    fn show(&self, k: u32) -> u32 {
        *self * k
    }
}

struct Shower {
    g: fn(&u32, u32) -> u32,
}

fn via_trait<T: Show>(x: &T, k: u32) -> u32 {
    let s = Shower { g: unsafe { transmute(<T as Show>::show as fn(&T, u32) -> u32) } };
    (s.g)(x, k)
}

#[rustc_intrinsic]
pub unsafe fn transmute<A, B>(a: A) -> B;

fn apply(op: &Op, v: u32) -> u32 {
    (op.f)(v) + op.tag
}

fn sum5(a: u32, b: u32, c: u32, d: u32, e: u32) -> u32 {
    a + b + c + d + e
}

#[no_mangle]
pub extern "C" fn fn_pointer(v: u32) -> u32 {
    let op = Op { f: inc, tag: 100 };
    let nested = unsafe {
        fn helper(x: u32) -> u32 {
            x * 2
        }
        helper(v)
    };
    apply(&op, v) + sum5(1, 2, 3, 4, 5) + nested + via_trait(&v, 3)
}
