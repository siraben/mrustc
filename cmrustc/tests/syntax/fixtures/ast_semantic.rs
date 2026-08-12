#![allow(dead_code)]

use crate::model::Point as P;
extern crate core as rust_core;

extern "C" {
    fn foreign(value: *const u8, length: usize) -> i32;
    static FOREIGN: u8;
}

trait Transform<T>: Sized
where
    T: Copy,
{
    type Output;
    const ZERO: usize = 0;
    fn apply(&self, value: T) -> Self::Output;
}

struct Point<T> {
    x: T,
    y: T,
}

impl<T> Transform<T> for Point<T>
where
    T: Copy,
{
    type Output = (T, T);

    fn apply(&self, value: T) -> (T, T) {
        let pair = (value, value);
        pair
    }
}

fn exercise(input: Option<(i32, i32)>, values: &[i32]) -> i32 {
    let mut total = 0;
    if let Some((left, right)) = input {
        total = left + right;
    }
    for &value in values {
        total += value;
    }
    let mapped = |x: i32| -> i32 { x * 2 };
    match total {
        0 => mapped(1),
        1..=9 => total,
        _ => {
            let array = [total, 2];
            array[0]
        }
    }
}

fn more_expressions(point: Point<i32>, values: &[i32]) -> i32 {
    let first = point.x as i32;
    let length = values.len();
    let range = 0..length;
    if first < 0 {
        return 0;
    }
    let mut count = 0;
    while count < 1 {
        count += 1;
        continue;
    }
    loop {
        break first + range.start as i32;
    }
}

mod model {
    pub struct Point;
}

macro_rules! semantic_type {
    () => { i32 };
}

union Bits {
    integer: u32,
    float: f32,
}

struct Pair(i32, i32);

fn rest_patterns(Pair(..): Pair, (_, ..): (i32, i32), [_, ..]: [i32; 2]) {}

trait Associated {
    type Value;
    const VALUE: i32;
}

impl Associated for i32 {
    type Value = i32;
    const VALUE: i32 = 1;
}

type Projection = <i32 as Associated>::Value;

fn returns_impl() -> impl Copy {
    1_i32
}

fn newer_syntax(
    result: Result<i32, i32>,
    tuple: (i32,),
    pointer: *const i32,
    object: &dyn Associated<Value = i32>,
) -> Result<i32, i32> {
    let qualified = <i32 as Associated>::VALUE;
    let projected = tuple.0;
    let raw = &raw const *pointer;
    let value = result?;
    let typed: semantic_type!() = value;
    let attempted = try { typed };
    if value > 0 && let 1 = value {
        return attempted;
    }
    let _ = (raw, object, returns_impl());
    Ok(qualified + projected + value)
}
