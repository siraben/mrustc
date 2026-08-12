#[cfg(unix)]
macro_rules! cfg_parent {
    () => { struct Enabled; }
}

mod child;
