#![cfg_attr(unix, no_core)]

mod inline {
    #![cfg_attr(unix, allow(dead_code))]
    struct Inline;
}

mod hidden;
struct hidden;
mod child;
mod conditional;
struct conditional;
