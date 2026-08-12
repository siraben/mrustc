#![feature(no_core)]
#![no_core]
#![no_main]

struct Slot {
    value: usize,
}

fn read(slot: Slot) -> usize {
    slot.value
}

#[no_mangle]
pub extern "C" fn aggregate_usize(value: usize) -> usize {
    read(Slot { value: value })
}
