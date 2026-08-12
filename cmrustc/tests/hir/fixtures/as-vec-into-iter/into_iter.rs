#[cfg(not(no_global_oom_handling))]
use super::AsVecIntoIter;

pub struct IntoIter<T> {
    item: T,
}

#[cfg(not(no_global_oom_handling))]
unsafe impl<T> AsVecIntoIter for IntoIter<T> {
    type Item = T;

    fn as_into_iter(&mut self) -> &mut IntoIter<Self::Item> {
        self
    }
}
