#![feature(no_core)]
#![no_core]
#![no_main]

// `assert_eq!` expands to `match (&left, &right) { (left_val, right_val)
// => .. }`: an irrefutable tuple pattern as a match arm must bind its
// elements (std's `get_stack_start` asserts on `pthread_attr_getstack`).
#[lang = "sized"]
trait Sized {}

fn by_value(x: u32) -> u32 {
    match (x, 10u32) {
        (a, b) => a + b,
    }
}

fn by_ref(x: u32) -> u32 {
    let v: i32 = 100;
    match (&x, &v) {
        (left_val, right_val) => {
            if *left_val == 1 { *right_val as u32 } else { 0 }
        }
    }
}

#[no_mangle]
pub extern "C" fn tuple_arm(x: u32) -> u32 {
    by_value(x) + by_ref(x)
}
