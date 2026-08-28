#![feature(no_core)]
#![no_core]
#![no_main]

struct Counter {
    value: u32,
}

fn bump(counter: &mut Counter, by: u32) {
    counter.value = counter.value + by;
}

fn read(counter: &Counter) -> u32 {
    counter.value
}

#[no_mangle]
pub extern "C" fn probe_ref(start: u32, by: u32) -> u32 {
    let mut counter = Counter { value: start };
    bump(&mut counter, by);
    bump(&mut counter, by);
    read(&counter)
}
