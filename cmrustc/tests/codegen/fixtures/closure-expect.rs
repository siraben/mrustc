// A closure argument takes its expectation from the callee's Fn-family
// bound (`map_err<F, O: FnOnce(E) -> F>`): its body's `.into()` learns
// the target type `F` (core's `finish_grow`/`layout_array` in RawVec).
#![feature(no_core)]
#![no_core]
#![no_main]
enum Result<T, E> { Ok(T), Err(E) }
trait FnOnce<Args> { type Output; fn call_once(self, args: Args) -> Self::Output; }
impl<T, E> Result<T, E> {
    fn map_err<F, O: FnOnce(E) -> F>(self, op: O) -> Result<T, F> {
        match self { Result::Ok(t) => Result::Ok(t), Result::Err(e) => Result::Err(op(e)) }
    }
}
trait From<T> { fn from(v: T) -> Self; }
trait Into<U> { fn into(self) -> U; }
impl<T, U: From<T>> Into<U> for T { fn into(self) -> U { U::from(self) } }
struct Small;
struct Big { code: u32 }
impl From<Small> for Big { fn from(_v: Small) -> Big { Big { code: 77 } } }
fn convert(r: Result<u32, u8>) -> Result<u32, Big> {
    r.map_err(|_| Small.into())
}
#[no_mangle]
pub extern "C" fn into_probe(i: u32) -> u32 {
    let a = convert(if i > 5 { Result::Err(1) } else { Result::Ok(i) });
    match a { Result::Ok(v) => v, Result::Err(b) => b.code }
}
