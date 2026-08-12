pub struct Direct;
pub struct Public;

macro_rules! make {
    () => { pub struct Generated; }
}
make!();

#[cfg(off)]
pub struct Hidden;
