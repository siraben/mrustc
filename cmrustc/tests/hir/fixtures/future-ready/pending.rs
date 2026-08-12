use crate::future::Future;
use crate::marker;
use crate::pin::Pin;
use crate::task::{Context, Poll};

#[stable(feature = "future_readiness_fns", since = "1.48.0")]
#[must_use = "futures do nothing unless you `.await` or poll them"]
pub struct Pending<T> {
    _data: marker::PhantomData<fn() -> T>,
}

#[stable(feature = "future_readiness_fns", since = "1.48.0")]
impl<T> Future for Pending<T> {
    type Output = T;

    fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<T> {
        Poll::Pending
    }
}
