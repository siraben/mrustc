#![feature(no_core)]
#![no_core]
#![no_main]

// Rust 2024 let chains (std's `signal_handler`: `if let Some(info) =
// info && info.guard_page_range.contains(..)`): the scrutinee stops
// before `&&`, and the chain is `LET && cond` with the binding scoped
// over the then-branch.
#[lang = "sized"]
trait Sized {}

enum Opt<T> {
    Some(T),
    None,
}

struct Info {
    name: Opt<u32>,
}

fn f(info: Opt<&Info>, x: u32) -> u32 {
    if let Opt::Some(info) = info
        && x > 0
        && let Opt::Some(n) = info.name
    {
        n + x
    } else {
        100
    }
}

fn count(mut v: Opt<u32>) -> u32 {
    let mut total = 0;
    while let Opt::Some(n) = v
        && n > 0
    {
        total = total + n;
        v = Opt::Some(n - 1);
    }
    total
}

#[no_mangle]
pub extern "C" fn let_chain(x: u32) -> u32 {
    let i = Info { name: Opt::Some(7) };
    f(Opt::Some(&i), x) + f(Opt::None, x) + f(Opt::Some(&i), 0) + count(Opt::Some(3))
}
