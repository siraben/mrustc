pub struct Wrapper<T = u32>(pub T);

pub enum Choice<T> {
    None,
    Some(T),
}

pub mod child {
    pub use bool;
}

pub type Alias = Wrapper<u32>;
