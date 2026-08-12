#![allow(dead_code)]

struct Counter;

trait Tick {
    fn shared(&self);
    fn unique(&mut self);
}

impl Tick for Counter {
    fn shared(&self) {}
    fn unique(&mut self) {}
}
