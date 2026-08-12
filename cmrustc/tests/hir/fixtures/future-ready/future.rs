use crate::pin::Pin;
use crate::task::{Context, Poll};

#[doc(notable_trait)]
#[doc(search_unbox)]
#[must_use = "futures do nothing unless you `.await` or poll them"]
#[stable(feature = "futures_api", since = "1.36.0")]
#[lang = "future_trait"]
#[diagnostic::on_unimplemented(
    label = "`{Self}` is not a future",
    message = "`{Self}` is not a future"
)]
pub trait Future {
    #[stable(feature = "futures_api", since = "1.36.0")]
    #[lang = "future_output"]
    type Output;

    #[lang = "poll"]
    #[stable(feature = "futures_api", since = "1.36.0")]
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output>;
}
