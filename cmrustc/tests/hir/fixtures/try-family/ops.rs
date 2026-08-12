use crate::control_flow::ControlFlow;

#[unstable(feature = "try_trait_v2", issue = "84277", old_name = "try_trait")]
#[rustc_on_unimplemented(
    on(
        all(from_desugaring = "TryBlock"),
        message = "a `try` block must return `Result` or `Option` (or another type that implements `{This}`)",
        label = "could not wrap the final value of the block as `{Self}` doesn't implement `Try`",
    ),
    on(
        all(from_desugaring = "QuestionMark"),
        message = "the `?` operator can only be applied to values that implement `{This}`",
        label = "the `?` operator cannot be applied to type `{Self}`"
    )
)]
#[doc(alias = "?")]
#[lang = "Try"]
#[const_trait]
#[rustc_const_unstable(feature = "const_try", issue = "74935")]
pub trait Try: ~const FromResidual {
    #[unstable(feature = "try_trait_v2", issue = "84277", old_name = "try_trait")]
    type Output;

    #[unstable(feature = "try_trait_v2", issue = "84277", old_name = "try_trait")]
    type Residual;

    #[lang = "from_output"]
    #[unstable(feature = "try_trait_v2", issue = "84277", old_name = "try_trait")]
    fn from_output(output: Self::Output) -> Self;

    #[lang = "branch"]
    #[unstable(feature = "try_trait_v2", issue = "84277", old_name = "try_trait")]
    fn branch(self) -> ControlFlow<Self::Residual, Self::Output>;
}

// Rust 1.90's full declaration is generic and has a projection default.
// This local target is deliberately reduced because this fixture exercises
// `Try`'s exact supertrait edge, not generic-trait default substitution.
pub trait FromResidual {}
