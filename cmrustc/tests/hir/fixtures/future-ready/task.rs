pub struct Context<'a> {
    marker: &'a (),
}

pub enum Poll<T> {
    Ready(T),
    Pending,
}
