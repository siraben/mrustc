#![no_core]

mod source {
    pub struct Clash { pub value: u8 }
}

struct Clash { value: u8 }
use source::Clash;
