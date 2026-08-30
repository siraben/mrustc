#![no_std]

fn below_four(value: &u32) -> bool {
    *value < 4
}

#[no_mangle]
pub extern "C" fn core_iterator_all_probe() -> u32 {
    let yes = [0u32, 1, 2];
    let no = [0u32, 4, 2];
    if yes.iter().all(below_four) && !no.iter().all(below_four) {
        1
    } else {
        0
    }
}
