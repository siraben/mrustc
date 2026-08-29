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

// `return` as a block's last statement: the block's unit value must not
// overwrite the return slot afterwards (core's `count_ones`: `{ return
// intrinsics::ctpop(self); }`).
fn tail_return(x: u32) -> u32 {
    return x + 1;
}

fn nested_tail_return(x: u32) -> u32 {
    let _unused = {
        if x > 0 {
            return x * 2;
        }
        5
    };
    9
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
    base + f(x) + tail_return(x) * 100 + nested_tail_return(x) * 10000
}
