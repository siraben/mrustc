#![no_core]

fn compute(input: Option<i32>, xs: [i32; 3]) -> i32 {
    let mut total: i32 = 1 + 2 * 3;
    let pair = (total, xs[0]);
    let choose = |x: i32| -> i32 { x + 1 };
    let numbers = 1..=4;
    if let Some(value) = input {
        total = choose(value);
    } else {
        total = xs.iter().next(0).field as i32;
    }
    match pair {
        (0 | 1, _) => return total,
        (ref left, right) if *left < right => total += right,
        _ => {},
    }
    loop { break; }
    while total < 10 {
        total = total + 1;
        if total == 8 { continue; }
    }
    for item in xs { total += item; }
    total
}

fn repeat(value: i32) {
    let repeated = [value; 4];
}

fn patterns(value: Thing, slice: [i32; 4]) {
    let Thing { left: ref x, right, .. } = value;
    let [first, .., last] = slice;
    let result = match first {
        0..=9 => true,
        _ => false,
    };
    let combine = move |mut x, y| x + y;
}

fn named_structs(iter: Iter, start: i32, end: i32, base: Range) {
    let pending = Pending { _data: marker::PhantomData };
    let from_iter = FromIter { iter };
    let range = Range { start, end };
    let updated = Range { start: 0, ..base };
    unsafe { consume(updated); };
}
