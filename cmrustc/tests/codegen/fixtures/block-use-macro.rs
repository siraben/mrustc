#![feature(no_core)]
#![feature(decl_macro)]
#![no_core]
#![no_main]

// std's stack_overflow.rs: `use crate::sys::weak::dlsym;` inside a closure
// body, then `dlsym!(..)` — a block-local `use` of a macro.
mod w {
    pub(crate) macro dlsym {
        (fn $name:ident() -> $ret:ty;) => {
            fn $name() -> $ret { 9 }
        }
    }
}

mod sys {
    pub(crate) use crate::w::dlsym;
}

fn go() -> u32 {
    let v = {
        use crate::sys::dlsym;
        dlsym!(fn nine() -> u32;);
        nine()
    };
    v + 1
}

#[no_mangle]
pub extern "C" fn block_use_macro() -> u32 {
    go()
}
