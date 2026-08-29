#![feature(no_core)]
#![no_core]
#![no_main]

// `return v` stores v in the body's return slot, including inside
// nested blocks and closures.
enum Option<T> {
    None,
    Some(T),
}

fn pick(x: u32) -> Option<u32> {
    if x == 0 {
        return Option::None;
    }
    unsafe {
        if x > 100 {
            return Option::Some(100);
        }
    }
    Option::Some(x)
}

#[no_mangle]
pub extern "C" fn early_return(x: u32) -> u32 {
    let base = match pick(x) {
        Option::Some(v) => v,
        Option::None => 7,
    };
    let f = |y: u32| -> u32 {
        if y > 10 {
            return y - 10;
        }
        y
    };
    base + f(x)
}
