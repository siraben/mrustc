#[cfg(not(no_global_oom_handling))]
pub(crate) use self::in_place_collect::AsVecIntoIter;
#[stable(feature = "rust1", since = "1.0.0")]
pub use self::into_iter::IntoIter;

mod into_iter;

#[cfg(not(no_global_oom_handling))]
mod in_place_collect;
