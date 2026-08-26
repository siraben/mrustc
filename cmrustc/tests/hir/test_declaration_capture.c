#include "cm/hir/declaration_capture.h"
#include "cm/hir/declaration_materialize.h"
#include "cm/hir/lower.h"
#include "cm/alloc.h"
#include "cm/driver/cfg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct CaptureFixture {
    CmSourceSet sources;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmImportResolver imports;
    CmHirModuleMap modules;
    CmHirContext hir;
    CmModuleGraphResult graph_result;
    CmHirLowerResult lower_result;
    CmHirArtifactConfig config;
    const CmTargetDesc *target;
    char data_layout[64];
    size_t data_layout_length;
} CaptureFixture;

static const CmHirItem *find_item(const CaptureFixture *fixture,
    const char *name, CmHirItemId *out_id);

static const unsigned char fixture_source[] =
    "mod layout {\n"
    "  #[stable(feature = \"alloc_layout\", since = \"1.28.0\")]\n"
    "  #[deprecated(since = \"1.52.0\", note = \"use LayoutError\")]\n"
    "  pub type LayoutErr = LayoutError;\n"
    "  #[stable(feature = \"alloc_layout_error\", since = \"1.50.0\")]\n"
    "  #[non_exhaustive]\n"
    "  #[derive(Clone, PartialEq, Eq, Debug)]\n"
    "  pub struct LayoutError;\n"
    "}\n"
    "#[stable(feature = \"alloc_layout\", since = \"1.28.0\")]\n"
    "#[deprecated(since = \"1.52.0\", note = \"use LayoutError\")]\n"
    "#[allow(deprecated, deprecated_in_future)]\n"
    "pub use layout::LayoutErr;\n"
    "#[stable(feature = \"alloc_layout_error\", since = \"1.50.0\")]\n"
    "pub use layout::LayoutError;\n"
    "#[unstable(feature = \"allocator_api\", issue = \"32838\")]\n"
    "#[derive(Copy, Clone, PartialEq, Eq, Debug)]\n"
    "pub struct AllocError;\n"
    "pub use AllocError as AllocAlias;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub use Gate as GateReexport;\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char const_fixture_source[] =
    "#[stable(feature = \"rust1\", since = \"1.0.0\")]\n"
    "pub const MAX: char = char::MAX;\n"
    "#[unstable(feature = \"next_char\", issue = \"none\")]\n"
    "pub const NEXT: usize = usize::MAX;\n"
    "#[deprecated(since = \"1.1.0\", note = \"old\")]\n"
    "pub const OLD: char = char::MAX;\n"
    "#[deprecated(since = \"1.1.0\", note = \"renamed\")]\n"
    "pub use MAX as RENAMED;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char static_fixture_source[] =
    "#[doc(hidden)]\n"
    "pub static TABLE: [(u64, i16, i16); 2] = "
        "[(1, -2, -3), (4, 5, 6)];\n"
    "pub use TABLE as RENAMED_TABLE;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char duplicate_static_fixture_source[] =
    "#[doc(hidden)]\n"
    "pub static FIRST: [(u64, i16, i16); 2] = "
        "[(1, -2, -3), (4, 5, 6)];\n"
    "#[doc(hidden)]\n"
    "pub static SECOND: [(u64, i16, i16); 2] = "
        "[(7, -8, -9), (10, 11, 12)];\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char default_enum_fixture_source[] =
    "#[rustc_diagnostic_item = \"mir_basic_block\"]\n"
    "pub enum BasicBlock { Normal, Cleanup }\n"
    "#[rustc_diagnostic_item = \"mir_unwind_terminate_reason\"]\n"
    "pub enum UnwindTerminateReason { Abi, InCleanup }\n"
    "pub use UnwindTerminateReason::{\n"
    "  Abi as ReasonAbi, InCleanup as ReasonInCleanup\n"
    "};\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char aggregate_fixture_source[] =
    "mod manually_drop {\n"
    "  #[stable(feature = \"manual\", since = \"1.0.0\")]\n"
    "  #[lang = \"manually_drop\"]\n"
    "  #[derive(Copy, Clone)]\n"
    "  #[repr(transparent)]\n"
    "  #[rustc_pub_transparent]\n"
    "  pub struct Manual<T: ?Sized> { value: T }\n"
    "}\n"
    "#[stable(feature = \"manual\", since = \"1.0.0\")]\n"
    "pub use manually_drop::Manual;\n"
    "mod maybe_uninit {\n"
    "  use super::Manual;\n"
    "  #[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "  #[lang = \"maybe_uninit\"]\n"
    "  #[derive(Copy)]\n"
    "  #[repr(transparent)]\n"
    "  #[rustc_pub_transparent]\n"
    "  pub union Maybe<T> { uninit: (), value: Manual<T> }\n"
    "}\n"
    "#[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "pub use maybe_uninit::Maybe;\n"
    "mod transmutability {\n"
    "  #[unstable(feature = \"transmute\", issue = \"none\")]\n"
    "  #[lang = \"transmute_opts\"]\n"
    "  #[derive(PartialEq, Eq, Clone, Copy, Debug)]\n"
    "  pub struct Assumptions {\n"
    "    pub alignment: bool,\n"
    "    pub lifetimes: bool,\n"
    "    pub safety: bool,\n"
    "    pub validity: bool,\n"
    "  }\n"
    "}\n"
    "#[unstable(feature = \"transmute\", issue = \"none\")]\n"
    "pub use transmutability::Assumptions;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char rust_tuple_fixture_source[] =
    "#[stable(feature = \"try_from\", since = \"1.34.0\")]\n"
    "#[derive(Debug, Copy, Clone)]\n"
    "pub struct TupleError(());\n"
    "pub use TupleError as TupleErrorAlias;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char into_iter_fixture_source[] =
    "mod manually_drop {\n"
    "  #[stable(feature = \"manual\", since = \"1.0.0\")]\n"
    "  #[lang = \"manually_drop\"]\n"
    "  #[derive(Copy, Clone)]\n"
    "  #[repr(transparent)]\n"
    "  #[rustc_pub_transparent]\n"
    "  pub struct Manual<T: ?Sized> { value: T }\n"
    "}\n"
    "#[stable(feature = \"manual\", since = \"1.0.0\")]\n"
    "pub use manually_drop::Manual;\n"
    "mod maybe_uninit {\n"
    "  use super::Manual;\n"
    "  #[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "  #[lang = \"maybe_uninit\"]\n"
    "  #[derive(Copy)]\n"
    "  #[repr(transparent)]\n"
    "  #[rustc_pub_transparent]\n"
    "  pub union Maybe<T> { uninit: (), value: Manual<T> }\n"
    "}\n"
    "#[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "pub use maybe_uninit::Maybe;\n"
    "mod array_iter {\n"
    "  use super::Maybe;\n"
    "  #[allow(private_bounds)]\n"
    "  trait PartialDrop {\n"
    "    unsafe fn partial_drop(&mut self, alive: IndexRange);\n"
    "  }\n"
    "  pub(crate) struct IndexRange { start: usize, end: usize }\n"
    "  mod iter_inner {\n"
    "    use super::{IndexRange, PartialDrop};\n"
    "    #[allow(private_bounds)]\n"
    "    pub(super) struct PolymorphicIter<DATA: ?Sized>\n"
    "    where DATA: PartialDrop { alive: IndexRange, data: DATA }\n"
    "  }\n"
    "  type InnerSized<T, const N: usize> =\n"
    "    iter_inner::PolymorphicIter<[Maybe<T>; N]>;\n"
    "  #[stable(feature = \"array_into_iter\", since = \"1.53.0\")]\n"
    "  #[rustc_insignificant_dtor]\n"
    "  #[rustc_diagnostic_item = \"ArrayIntoIter\"]\n"
    "  #[derive(Clone)]\n"
    "  pub struct IntoIter<T, const N: usize> { inner: InnerSized<T, N> }\n"
    "}\n"
    "#[stable(feature = \"array_into_iter\", since = \"1.53.0\")]\n"
    "pub use array_iter::IntoIter;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char from_fn_fixture_source[] =
    "#[unstable(feature = \"tuple_trait\", issue = \"none\")]\n"
    "#[lang = \"tuple_trait\"]\n"
    "#[diagnostic::on_unimplemented(message = \"not a tuple\")]\n"
    "#[rustc_deny_explicit_impl]\n"
    "#[rustc_do_not_implement_via_object]\n"
    "pub trait Tuple {}\n"
    "#[lang = \"fn_once\"]\n"
    "#[stable(feature = \"rust1\", since = \"1.0.0\")]\n"
    "#[rustc_paren_sugar]\n"
    "#[rustc_on_unimplemented(message = \"not callable\")]\n"
    "#[fundamental]\n"
    "#[must_use = \"closures are lazy\"]\n"
    "#[const_trait]\n"
    "#[rustc_const_unstable(feature = \"const_trait_impl\", "
        "issue = \"none\")]\n"
    "pub trait FnOnce<Args: Tuple> {\n"
    "  #[lang = \"fn_once_output\"]\n"
    "  #[stable(feature = \"fn_once_output\", since = \"1.0.0\")]\n"
    "  type Output;\n"
    "  #[unstable(feature = \"fn_traits\", issue = \"none\")]\n"
    "  extern \"rust-call\" fn call_once(self, args: Args) "
        "-> Self::Output;\n"
    "}\n"
    "#[lang = \"fn_mut\"]\n"
    "#[stable(feature = \"rust1\", since = \"1.0.0\")]\n"
    "#[rustc_paren_sugar]\n"
    "#[rustc_on_unimplemented(message = \"not callable\")]\n"
    "#[fundamental]\n"
    "#[must_use = \"closures are lazy\"]\n"
    "#[const_trait]\n"
    "#[rustc_const_unstable(feature = \"const_trait_impl\", "
        "issue = \"none\")]\n"
    "pub trait FnMut<Args: Tuple>: FnOnce<Args> {\n"
    "  #[unstable(feature = \"fn_traits\", issue = \"none\")]\n"
    "  extern \"rust-call\" fn call_mut(&mut self, args: Args) "
        "-> Self::Output;\n"
    "}\n"
    "#[inline]\n"
    "#[stable(feature = \"array_from_fn\", since = \"1.63.0\")]\n"
    "pub fn build<T, const N: usize, F>(f: F) -> [T; N]\n"
    "where F: FnMut(usize) -> T { f }\n";

static const unsigned char from_mut_fixture_source[] =
    "pub trait Gate {}\n"
    "pub fn needs<X: Gate>() {}\n"
    "#[stable(feature = \"array_from_mut\", since = \"1.53.0\")]\n"
    "#[rustc_const_stable(feature = \"const_array_from_mut\", "
        "since = \"1.83.0\")]\n"
    "pub const fn borrow<T>(s: &mut T) -> &mut [T; 1] { s }\n";

static const unsigned char from_mut_explicit_infer_source[] =
    "pub trait Gate {}\n"
    "pub fn needs<X: Gate>() {}\n"
    "#[stable(feature = \"array_from_mut\", since = \"1.53.0\")]\n"
    "#[rustc_const_stable(feature = \"const_array_from_mut\", "
        "since = \"1.83.0\")]\n"
    "pub const fn borrow<T>(s: &'_ mut T) -> &'_ mut [T; 1] { s }\n";

static const unsigned char from_ref_fixture_source[] =
    "pub trait Gate {}\n"
    "pub fn needs<X: Gate>() {}\n"
    "#[stable(feature = \"array_from_ref\", since = \"1.53.0\")]\n"
    "#[rustc_const_stable(feature = \"const_array_from_ref\", "
        "since = \"1.83.0\")]\n"
    "pub const fn borrow_shared<T>(s: &T) -> &[T; 1] { s }\n";

static const unsigned char from_ref_explicit_infer_source[] =
    "pub trait Gate {}\n"
    "pub fn needs<X: Gate>() {}\n"
    "#[stable(feature = \"array_from_ref\", since = \"1.53.0\")]\n"
    "#[rustc_const_stable(feature = \"const_array_from_ref\", "
        "since = \"1.83.0\")]\n"
    "pub const fn borrow_shared<T>(s: &'_ T) -> &'_ [T; 1] { s }\n";

static const unsigned char from_ref_mixed_source[] =
    "pub trait Gate {}\n"
    "pub fn needs<X: Gate>() {}\n"
    "#[stable(feature = \"array_from_ref\", since = \"1.53.0\")]\n"
    "#[rustc_const_stable(feature = \"const_array_from_ref\", "
        "since = \"1.83.0\")]\n"
    "pub const fn borrow_shared<T>(s: &T) -> &mut [T; 1] { s }\n";

static const unsigned char from_ref_extra_input_source[] =
    "pub trait Gate {}\n"
    "pub fn needs<X: Gate>() {}\n"
    "#[stable(feature = \"array_from_ref\", since = \"1.53.0\")]\n"
    "#[rustc_const_stable(feature = \"const_array_from_ref\", "
        "since = \"1.83.0\")]\n"
    "pub const fn borrow_shared<T>(s: &T, extra: &T) -> &[T; 1] { s }\n";

static const char generic_enum_fixture_template[] =
    "mod option_like {\n"
    "  #[doc(search_unbox)]\n"
    "  #[derive(Copy, Eq)]\n"
    "  #[rustc_diagnostic_item = \"Maybe\"]\n"
    "  #[lang = \"Maybe\"]\n"
    "  #[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "  #[allow(dead_code)]\n"
    "  pub enum Maybe<T> {\n"
    "    #[lang = \"Nothing\"]\n"
    "    #[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "    Nothing,\n"
    "    #[lang = \"Just\"]\n"
    "    #[stable(feature = \"maybe\", since = \"1.0.0\")]\n"
    "    Just(%sT),\n"
    "  }\n"
    "}\n"
    "mod result_like {\n"
    "  #[doc(search_unbox)]\n"
    "  #[derive(Copy, Eq)]\n"
    "  #[must_use = \"this outcome must be handled\"]\n"
    "  #[rustc_diagnostic_item = \"Outcome\"]\n"
    "  #[stable(feature = \"outcome\", since = \"1.0.0\")]\n"
    "  pub enum Outcome<T, E> {\n"
    "    #[lang = \"Good\"]\n"
    "    #[stable(feature = \"outcome\", since = \"1.0.0\")]\n"
    "    Good(%sT),\n"
    "    #[lang = \"Bad\"]\n"
    "    #[stable(feature = \"outcome\", since = \"1.0.0\")]\n"
    "    Bad(%sE),\n"
    "  }\n"
    "}\n"
    "pub mod v1 {\n"
    "  #[stable(feature = \"prelude\", since = \"1.0.0\")]\n"
    "  #[doc(no_inline)]\n"
    "  pub use crate::option_like::Maybe::{self, Nothing, Just};\n"
    "  #[stable(feature = \"prelude\", since = \"1.0.0\")]\n"
    "  #[doc(no_inline)]\n"
    "  pub use crate::result_like::Outcome::{self, Good, Bad};\n"
    "}\n"
    "pub mod rust_2015 {\n"
    "  #[stable(feature = \"prelude_2015\", since = \"1.0.0\")]\n"
    "  #[doc(no_inline)]\n"
    "  pub use crate::v1::*;\n"
    "}\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char primitive_reexport_fixture_source[] =
    "pub mod primitive {\n"
    "  #[stable(feature = \"primitive\", since = \"1.0.0\")]\n"
    "  pub use {\n"
    "    bool, char, str, i8, i16, i32, i64, i128, isize,\n"
    "    u8, u16, u32, u64, u128, usize, f32, f64\n"
    "  };\n"
    "  pub mod base {\n"
    "    #[stable(feature = \"primitive_alias\", since = \"1.0.0\")]\n"
    "    pub use u8 as byte;\n"
    "  }\n"
    "  #[stable(feature = \"primitive_transitive_alias\", since = \"1.0.0\")]\n"
    "  pub use base::byte as octet;\n"
    "}\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const char allocator_like_fixture_template[] =
    "#[stable(feature = \"marker\", since = \"1.0.0\")]\n"
    "pub trait Marker {}\n"
    "%s"
    "#[unstable(feature = \"allocator_like\", issue = \"none\")]\n"
    "pub unsafe trait AllocatorLike {\n"
    "  #[unstable(feature = \"allocator_like\", issue = \"none\")]\n"
    "  fn allocate(&self, size: usize) -> usize;\n"
    "  #[stable(feature = \"allocator_like\", since = \"1.0.0\")]\n"
    "  fn allocate_zeroed(&self, size: usize) -> usize { size }\n"
    "  #[deprecated(since = \"1.1.0\", note = \"example\")]\n"
    "  unsafe fn deallocate(&self, token: usize);\n"
    "  #[unstable(feature = \"allocator_like\", issue = \"none\")]\n"
    "  unsafe fn grow(&self, token: usize, new_size: usize) -> usize "
        "{ new_size }\n"
    "  #[stable(feature = \"allocator_like\", since = \"1.0.0\")]\n"
    "  unsafe fn grow_zeroed(&self, token: usize, new_size: usize) -> usize "
        "{ new_size }\n"
    "  #[unstable(feature = \"allocator_like\", issue = \"none\")]\n"
    "  unsafe fn shrink(&self, token: usize, new_size: usize) -> usize "
        "{ new_size }\n"
    "  #[stable(feature = \"allocator_like\", since = \"1.0.0\")]\n"
    "%s"
    "  fn by_ref(&self) where Self: Marker {}\n"
    "}\n"
    "pub fn needs<X: Marker>() {}\n";

static const unsigned char inferred_associated_return_fixture_source[] =
    "pub trait Marker {}\n"
    "pub unsafe trait AllocatorLike {\n"
    "  fn by_ref(&self) -> &Self where Self: Marker;\n"
    "}\n"
    "pub fn needs<X: Marker>() {}\n";

static const unsigned char safe_associated_trait_fixture_source[] =
    "pub trait Marker {}\n"
    "pub trait SafeLike { fn ping(&self); }\n"
    "pub fn needs<X: Marker>() {}\n";

static const unsigned char any_like_fixture_source[] =
    "#[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
    "#[rustc_diagnostic_item = \"AnyLike\"]\n"
    "pub trait AnyLike: 'static {\n"
    "  #[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
    "  fn type_id(&self) -> TypeIdLike;\n"
    "}\n"
    "pub struct OtherId;\n"
    "pub struct TypeIdLike;\n"
    "pub trait Marker {}\n"
    "pub fn needs<X: Marker>() {}\n";

static const unsigned char inline_free_function_fixture_source[] =
    "pub trait Marker {}\n"
    "#[inline(always)]\n"
    "pub fn needs<X: Marker>() {}\n";

static const unsigned char composite_associated_fixture_source[] =
    "#[stable(feature = \"wrap\", since = \"1.0.0\")]\n"
    "#[lang = \"manually_drop\"]\n"
    "#[derive(Copy)]\n"
    "#[repr(transparent)]\n"
    "#[rustc_pub_transparent]\n"
    "pub struct Wrap<T: ?Sized> { value: T }\n"
    "pub struct Error;\n"
    "#[doc(search_unbox)]\n"
    "#[derive(Copy)]\n"
    "#[rustc_diagnostic_item = \"Outcome\"]\n"
    "#[stable(feature = \"outcome\", since = \"1.0.0\")]\n"
    "pub enum Outcome<T, E> {\n"
    "  #[lang = \"Good\"]\n"
    "  #[stable(feature = \"outcome\", since = \"1.0.0\")]\n"
    "  Good(T),\n"
    "  #[lang = \"Bad\"]\n"
    "  #[stable(feature = \"outcome\", since = \"1.0.0\")]\n"
    "  Bad(E),\n"
    "}\n"
    "mod shadow {\n"
    "  #[doc(search_unbox)]\n"
    "  #[derive(Copy)]\n"
    "  #[rustc_diagnostic_item = \"ShadowOutcome\"]\n"
    "  #[stable(feature = \"shadow\", since = \"1.0.0\")]\n"
    "  pub enum Outcome<T, E> {\n"
    "    #[lang = \"ShadowGood\"]\n"
    "    #[stable(feature = \"shadow\", since = \"1.0.0\")]\n"
    "    Good(T),\n"
    "    #[lang = \"ShadowBad\"]\n"
    "    #[stable(feature = \"shadow\", since = \"1.0.0\")]\n"
    "    Bad(E),\n"
    "  }\n"
    "}\n"
    "pub unsafe trait Composite {\n"
    "  fn outcome(&self, bytes: *const u8)\n"
    "    -> Outcome<Wrap<[u8]>, Error>;\n"
    "  fn raw_mut(&self) -> *mut u8;\n"
    "}\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char unreachable_private_type_fixture_source[] =
    "fn hidden(value: (u8, u8)) {}\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char repeated_composite_fixture_source[] =
    "#[doc(hidden)]\n"
    "pub static MANY: [((u8,), (u8,)); 1] = [((1,), (2,))];\n"
    "pub unsafe trait Unary {\n"
    "  fn raw(&self, bytes: *const [u8]) -> *mut u8;\n"
    "}\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char type_id_like_fixture_source[] =
    "#[derive(Clone, Copy, Eq, PartialOrd, Ord)]\n"
    "#[stable(feature = \"rust1\", since = \"1.0.0\")]\n"
    "#[lang = \"type_id_like\"]\n"
    "pub struct TypeIdLike {\n"
    "  pub(crate) data: [*const (); 16 / size_of::<*const ()>()],\n"
    "}\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char type_name_fixture_source[] =
    "pub trait Marker {}\n"
    "#[must_use]\n"
    "#[stable(feature = \"type_name_like\", since = \"1.0.0\")]\n"
    "#[rustc_const_unstable(feature = \"const_type_name_like\", "
        "issue = \"none\")]\n"
    "pub const fn type_name_like<T: ?Sized>() -> &'static str { \"\" }\n";

static const unsigned char unit_function_fixture_source[] =
    "pub trait Marker {}\n"
    "#[unstable(feature = \"breakpoint_like\", issue = \"123\")]\n"
    "#[inline(always)]\n"
    "pub fn breakpoint_like() {}\n";

static const unsigned char unit_function_stable_fixture_source[] =
    "pub trait Marker {}\n"
    "#[stable(feature = \"breakpoint_like\", since = \"1.0.0\")]\n"
    "#[inline]\n"
    "pub fn breakpoint_like() {}\n";

static const unsigned char type_name_of_val_fixture_source[] =
    "pub trait Marker {}\n"
    "#[must_use]\n"
    "#[stable(feature = \"type_name_of_val_like\", since = \"1.0.0\")]\n"
    "#[rustc_const_unstable(feature = \"const_type_name_like\", "
        "issue = \"none\")]\n"
    "pub const fn type_name_of_val_like<T: ?Sized>(_val: &T) "
        "-> &'static str { \"\" }\n";

static CmHirArtifactBytes test_bytes(const char *text)
{
    CmHirArtifactBytes bytes;
    bytes.data = (const unsigned char *)text;
    bytes.length = strlen(text);
    return bytes;
}

static void fixture_init_source_target(CaptureFixture *fixture,
    int with_noise, const char *path, const unsigned char *source,
    size_t source_length, const char *target_triple)
{
    CmSourceId root;
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    int layout_length;
    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    cm_module_graph_init(&fixture->graph);
    cm_import_resolver_init(&fixture->imports);
    cm_hir_module_map_init(&fixture->modules);
    cm_hir_context_init(&fixture->hir);
    cm_hir_artifact_config_init(&fixture->config);
    fixture->target = cm_target_find(target_triple);
    assert(fixture->target != NULL
        && cm_target_cfg_set(&fixture->cfg, fixture->target));
    if (with_noise) {
        CmSourceId ignored_source;
        CmHirCrateId ignored_crate;
        CmHirModuleId ignored_module;
        CmHirType type;
        CmHirTypeId ignored_type;
        static const unsigned char ignored[] = "pub struct Noise;\n";
        assert(cm_source_add_memory(&fixture->sources, "noise.rs", ignored,
            sizeof(ignored) - 1u, &ignored_source) == CM_SOURCE_OK);
        assert(cm_hir_create_crate(&fixture->hir,
            cm_hir_intern(&fixture->hir, "noise"), CM_HIR_EDITION_2024,
            (CmSpan){ ignored_source, 0u, 1u }, &ignored_crate,
            &ignored_module) == CM_HIR_OK);
        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_UNIT_KIND;
        type.span = (CmSpan){ ignored_source, 0u, 1u };
        assert(cm_hir_add_type(&fixture->hir, &type, &ignored_type)
            == CM_HIR_OK);
    }
    assert(cm_source_add_memory(&fixture->sources, path, source,
        source_length, &root) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2024;
    graph_options.cfg = &fixture->cfg;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, root, &graph_options);
    assert(fixture->graph_result.error_count == 0u);
    import_result = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    assert(import_result.error_count == 0u
        && import_result.revision == fixture->graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "v30_provider";
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_options.source = root;
    lower_options.pointer_bits = fixture->target->pointer_bits;
    fixture->lower_result = cm_hir_lower_module_graph(&fixture->hir,
        &fixture->graph, fixture->graph_result.revision, &fixture->imports,
        &fixture->modules, &lower_options);
    assert(fixture->lower_result.error_count == 0u);
    assert(cm_hir_artifact_config_build(fixture->target, CM_EDITION_2024,
        CM_HIR_ARTIFACT_PANIC_ABORT, &fixture->cfg, &fixture->config)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    layout_length = snprintf(fixture->data_layout,
        sizeof(fixture->data_layout), "capture-test-layout-v1:%c-p:%u:%u",
        fixture->target->endian == CM_ENDIAN_LITTLE ? 'e' : 'E',
        fixture->target->pointer_bits, fixture->target->pointer_bits);
    assert(layout_length > 0
        && (size_t)layout_length < sizeof(fixture->data_layout));
    fixture->data_layout_length = (size_t)layout_length;
}

static void fixture_init_source(CaptureFixture *fixture, int with_noise,
    const char *path, const unsigned char *source, size_t source_length)
{
    fixture_init_source_target(fixture, with_noise, path, source,
        source_length, "x86_64-unknown-linux-gnu");
}

static void fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-trait-provider.rs",
        fixture_source, sizeof(fixture_source) - 1u);
}

static void const_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-const-provider.rs",
        const_fixture_source, sizeof(const_fixture_source) - 1u);
}

static void static_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-static-provider.rs",
        static_fixture_source, sizeof(static_fixture_source) - 1u);
}

static void type_id_like_fixture_init(CaptureFixture *fixture,
    int with_noise, const char *target_triple)
{
    fixture_init_source_target(fixture, with_noise, "type-id-like.rs",
        type_id_like_fixture_source,
        sizeof(type_id_like_fixture_source) - 1u, target_triple);
}

static void type_name_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "type-name-like.rs",
        type_name_fixture_source, sizeof(type_name_fixture_source) - 1u);
}

static void unit_function_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "unit-function-like.rs",
        unit_function_fixture_source,
        sizeof(unit_function_fixture_source) - 1u);
}

static void type_name_of_val_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    fixture_init_source(fixture, with_noise, "type-name-of-val-like.rs",
        type_name_of_val_fixture_source,
        sizeof(type_name_of_val_fixture_source) - 1u);
}

static void default_enum_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-default-enum-provider.rs",
        default_enum_fixture_source,
        sizeof(default_enum_fixture_source) - 1u);
}

static void aggregate_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-aggregate-provider.rs",
        aggregate_fixture_source, sizeof(aggregate_fixture_source) - 1u);
}

static void rust_tuple_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "rust-tuple.rs",
        rust_tuple_fixture_source, sizeof(rust_tuple_fixture_source) - 1u);
}

static void into_iter_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "into-iter.rs",
        into_iter_fixture_source, sizeof(into_iter_fixture_source) - 1u);
}

static void from_fn_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "from-fn-like.rs",
        from_fn_fixture_source, sizeof(from_fn_fixture_source) - 1u);
}

static void from_mut_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "from-mut-like.rs",
        from_mut_fixture_source, sizeof(from_mut_fixture_source) - 1u);
}

static void from_ref_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "from-ref-like.rs",
        from_ref_fixture_source, sizeof(from_ref_fixture_source) - 1u);
}

static void layout_dependency_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    CmByteBuf source;
    size_t index;
    char variant[64];
    int written;
    cm_byte_buf_init(&source);
    cm_byte_buf_append(&source, (const unsigned char *)
        "mod ptr {\n"
        "  #[unstable(feature = \"ptr_alignment_type\", issue = \"102070\")]\n"
        "  #[derive(Copy, Clone, PartialEq, Eq)]\n"
        "  #[repr(transparent)]\n"
        "  pub struct Alignment(AlignmentEnum);\n"
        "  #[cfg(any())]\n"
        "  #[derive(Copy, Clone)]\n"
        "  #[repr(u16)]\n"
        "  enum AlignmentEnum { Disabled = 2 }\n"
        "  #[cfg(all())]\n"
        "  #[derive(Copy, Clone, PartialEq, Eq)]\n"
        "  #[repr(u64)]\n"
        "  enum AlignmentEnum {\n",
        sizeof("mod ptr {\n"
        "  #[unstable(feature = \"ptr_alignment_type\", issue = \"102070\")]\n"
        "  #[derive(Copy, Clone, PartialEq, Eq)]\n"
        "  #[repr(transparent)]\n"
        "  pub struct Alignment(AlignmentEnum);\n"
        "  #[cfg(any())]\n"
        "  #[derive(Copy, Clone)]\n"
        "  #[repr(u16)]\n"
        "  enum AlignmentEnum { Disabled = 2 }\n"
        "  #[cfg(all())]\n"
        "  #[derive(Copy, Clone, PartialEq, Eq)]\n"
        "  #[repr(u64)]\n"
        "  enum AlignmentEnum {\n") - 1u);
    for (index = 0u; index < 64u; ++index) {
        written = snprintf(variant, sizeof(variant),
            "    Align%lu = 1 << %lu,\n", (unsigned long)index,
            (unsigned long)index);
        assert(written > 0 && (size_t)written < sizeof(variant));
        cm_byte_buf_append(&source, (const unsigned char *)variant,
            (size_t)written);
    }
    cm_byte_buf_append(&source, (const unsigned char *)
        "  }\n"
        "  #[stable(feature = \"orphan\", since = \"1.0.0\")]\n"
        "  #[derive(Copy, Clone)]\n"
        "  struct Orphan { value: usize }\n"
        "}\n"
        "#[stable(feature = \"alloc_layout\", since = \"1.28.0\")]\n"
        "#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]\n"
        "#[lang = \"alloc_layout\"]\n"
        "pub struct Layout { size: usize, align: ptr::Alignment }\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        sizeof("  }\n"
        "  #[stable(feature = \"orphan\", since = \"1.0.0\")]\n"
        "  #[derive(Copy, Clone)]\n"
        "  struct Orphan { value: usize }\n"
        "}\n"
        "#[stable(feature = \"alloc_layout\", since = \"1.28.0\")]\n"
        "#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]\n"
        "#[lang = \"alloc_layout\"]\n"
        "pub struct Layout { size: usize, align: ptr::Alignment }\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n") - 1u);
    fixture_init_source(fixture, with_noise, "layout-dependency.rs",
        source.data, source.len);
    cm_byte_buf_destroy(&source);
}

static void wide_enum_fixture_init(CaptureFixture *fixture,
    const char *repr, const char *high)
{
    char source[2048];
    int written = snprintf(source, sizeof(source),
        "#[derive(Copy, Clone)]\n"
        "#[repr(%s)]\n"
        "pub enum Wide { Low = 1, High = %s }\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n", repr, high);
    assert(written > 0 && (size_t)written < sizeof(source));
    fixture_init_source(fixture, 0, "wide-enum.rs",
        (const unsigned char *)source, (size_t)written);
}

static void generic_enum_fixture_init(CaptureFixture *fixture,
    int with_noise, int with_field_stability)
{
    char source[8192];
    const char *field_attribute = with_field_stability
        ? "#[stable(feature = \"field\", since = \"1.0.0\")] " : "";
    int written = snprintf(source, sizeof(source),
        generic_enum_fixture_template, field_attribute, field_attribute,
        field_attribute);
    assert(written > 0 && (size_t)written < sizeof(source));
    fixture_init_source(fixture, with_noise, "v30-generic-enum-provider.rs",
        (const unsigned char *)source, (size_t)written);
}

static void primitive_reexport_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    fixture_init_source(fixture, with_noise,
        "v30-primitive-reexports.rs", primitive_reexport_fixture_source,
        sizeof(primitive_reexport_fixture_source) - 1u);
}

static void allocator_like_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    char source[8192];
    int written = snprintf(source, sizeof(source),
        allocator_like_fixture_template, "", "  #[inline(always)]\n");
    assert(written > 0 && (size_t)written < sizeof(source));
    fixture_init_source(fixture, with_noise, "allocator-like.rs",
        (const unsigned char *)source, (size_t)written);
}

static void allocator_like_fixture_init_hints(CaptureFixture *fixture,
    const char *outer_hint, const char *method_hint)
{
    char source[8192];
    int written = snprintf(source, sizeof(source),
        allocator_like_fixture_template, outer_hint, method_hint);
    assert(written > 0 && (size_t)written < sizeof(source));
    fixture_init_source(fixture, 0, "allocator-like-hint.rs",
        (const unsigned char *)source, (size_t)written);
}

static void any_like_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "any-like.rs",
        any_like_fixture_source, sizeof(any_like_fixture_source) - 1u);
}

static void composite_associated_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    fixture_init_source(fixture, with_noise, "associated-composite.rs",
        composite_associated_fixture_source,
        sizeof(composite_associated_fixture_source) - 1u);
}

static void many_unique_array_fixture_init(CaptureFixture *fixture,
    int with_noise, int reverse_discovery, size_t array_count)
{
    CmByteBuf source;
    CmHirItemId item_id;
    CmHirItem *item;
    CmHirType *outer;
    CmHirType *templates;
    CmHirTypeId *new_ids;
    size_t index;
    char fragment[64];
    int written;
    cm_byte_buf_init(&source);
    cm_byte_buf_append(&source,
        (const unsigned char *)"#[doc(hidden)]\npub static MANY: (",
        sizeof("#[doc(hidden)]\npub static MANY: (") - 1u);
    for (index = 0u; index < array_count; ++index) {
        written = snprintf(fragment, sizeof(fragment), "[u8; %lu],",
            (unsigned long)index);
        assert(written > 0 && (size_t)written < sizeof(fragment));
        cm_byte_buf_append(&source, (const unsigned char *)fragment,
            (size_t)written);
    }
    cm_byte_buf_append(&source, (const unsigned char *)") = (",
        sizeof(") = (") - 1u);
    for (index = 0u; index < array_count; ++index) {
        written = snprintf(fragment, sizeof(fragment), "[0; %lu],",
            (unsigned long)index);
        assert(written > 0 && (size_t)written < sizeof(fragment));
        cm_byte_buf_append(&source, (const unsigned char *)fragment,
            (size_t)written);
    }
    cm_byte_buf_append(&source, (const unsigned char *)
        ");\npub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        sizeof(");\npub trait Gate<T: ?Sized> {}\n"
            "pub fn needs<X: Gate<u8>>() {}\n") - 1u);
    fixture_init_source(fixture, with_noise, "many-unique-arrays.rs",
        source.data, source.len);
    cm_byte_buf_destroy(&source);

    item = (CmHirItem *)(void *)find_item(fixture, "MANY", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_STATIC);
    outer = (CmHirType *)cm_hir_get_type(&fixture->hir,
        item->data.value_item.type);
    assert(outer != NULL && outer->kind == CM_HIR_TYPE_TUPLE_KIND
        && outer->data.tuple_type.element_count == (uint32_t)array_count);
    templates = (CmHirType *)cm_alloc_zeroed(array_count,
        sizeof(*templates));
    new_ids = (CmHirTypeId *)cm_alloc_zeroed(array_count, sizeof(*new_ids));
    for (index = 0u; index < array_count; ++index) {
        const CmHirType *array = cm_hir_get_type(&fixture->hir,
            outer->data.tuple_type.elements[index]);
        assert(array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
            && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
            && array->data.array_type.length.data.value.low_bits == index);
        templates[index] = *array;
    }
    for (index = 0u; index < array_count; ++index) {
        size_t logical = reverse_discovery ? array_count - index - 1u : index;
        assert(cm_hir_add_type(&fixture->hir, &templates[logical],
                &new_ids[logical]) == CM_HIR_OK);
    }
    assert(array_count < 2u
        || (reverse_discovery && new_ids[0] > new_ids[array_count - 1u])
        || (!reverse_discovery && new_ids[0] < new_ids[array_count - 1u]));
    outer = (CmHirType *)cm_hir_get_type(&fixture->hir,
        item->data.value_item.type);
    for (index = 0u; index < array_count; ++index)
        outer->data.tuple_type.elements[index] = new_ids[index];
    cm_free(new_ids);
    cm_free(templates);
}

static void many_self_trait_fixture_init(CaptureFixture *fixture,
    size_t trait_count)
{
    CmByteBuf source;
    size_t index;
    char declaration[96];
    int written;
    cm_byte_buf_init(&source);
    for (index = 0u; index < trait_count; ++index) {
        written = snprintf(declaration, sizeof(declaration),
            "pub unsafe trait SelfTrait%lu { fn ping(&self); }\n",
            (unsigned long)index);
        assert(written > 0 && (size_t)written < sizeof(declaration));
        cm_byte_buf_append(&source, (const unsigned char *)declaration,
            (size_t)written);
    }
    cm_byte_buf_append(&source, (const unsigned char *)
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        sizeof("pub trait Gate<T: ?Sized> {}\n"
            "pub fn needs<X: Gate<u8>>() {}\n") - 1u);
    fixture_init_source(fixture, 0, "many-self-traits.rs", source.data,
        source.len);
    cm_byte_buf_destroy(&source);
}

static void fixture_destroy(CaptureFixture *fixture)
{
    cm_hir_artifact_config_destroy(&fixture->config);
    cm_hir_module_map_destroy(&fixture->modules);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
    cm_hir_context_destroy(&fixture->hir);
}

static CmHirDeclarationCaptureInput capture_input(
    const CaptureFixture *fixture)
{
    CmHirDeclarationCaptureInput input;
    memset(&input, 0, sizeof(input));
    input.hir = &fixture->hir;
    input.crate_id = fixture->lower_result.crate_id;
    input.graph = &fixture->graph;
    input.revision = fixture->graph_result.revision;
    input.imports = &fixture->imports;
    input.modules = &fixture->modules;
    input.configuration = &fixture->config;
    input.crate_disambiguator = test_bytes("capture-test-disambiguator");
    input.target_triple.data = fixture->target->triple;
    input.target_triple.length = strlen(fixture->target->triple);
    input.data_layout.data = fixture->data_layout;
    input.data_layout.length = fixture->data_layout_length;
    input.target_pointer_bits = fixture->target->pointer_bits;
    return input;
}

static const CmHirItem *find_item(const CaptureFixture *fixture,
    const char *name, CmHirItemId *out_id)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &fixture->hir.items, index);
        const CmInternedString *item_name = item == NULL ? NULL
            : cm_interner_get(&fixture->hir.strings, item->name);
        if (item != NULL
            && item->definition.crate_id == fixture->lower_result.crate_id
            && item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            *out_id = (CmHirItemId)(index + 1u);
            return item;
        }
    }
    return NULL;
}

static const CmHirModule *find_module(const CaptureFixture *fixture,
    const char *name)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < fixture->hir.modules.len; ++index) {
        const CmHirModule *module = (const CmHirModule *)cm_vec_at_const(
            &fixture->hir.modules, index);
        const CmInternedString *module_name = module == NULL ? NULL
            : cm_interner_get(&fixture->hir.strings, module->name);
        if (module != NULL && module_name != NULL
            && module_name->len == length
            && memcmp(module_name->bytes, name, length) == 0) return module;
    }
    return NULL;
}

static CmHirImport *find_unique_attributed_import(CaptureFixture *fixture)
{
    CmHirImport *result = NULL;
    size_t module_index;
    for (module_index = 0u; module_index < fixture->hir.modules.len;
            ++module_index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(
            &fixture->hir.modules, module_index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != fixture->lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *candidate = &module->imports[import_index];
            if (candidate->attribute_count == 0u) continue;
            assert(result == NULL);
            result = candidate;
        }
    }
    return result;
}

static CmHirImport *find_import_with_attribute_count(CaptureFixture *fixture,
    uint32_t attribute_count)
{
    CmHirImport *result = NULL;
    size_t module_index;
    for (module_index = 0u; module_index < fixture->hir.modules.len;
            ++module_index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(
            &fixture->hir.modules, module_index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != fixture->lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *candidate = &module->imports[import_index];
            if (candidate->attribute_count != attribute_count) continue;
            assert(result == NULL);
            result = candidate;
        }
    }
    return result;
}

static int declaration_string_is(CmHirDeclarationString value,
    const char *text)
{
    size_t length = strlen(text);
    return value.length == length
        && memcmp(value.data, text, length) == 0;
}

static const CmHirDeclarationItem *find_declaration_item(
    const CmHirDeclarationMetadata *metadata, const char *name,
    uint32_t *out_local)
{
    size_t index;
    for (index = 0u; index < metadata->item_count; ++index) {
        if (declaration_string_is(metadata->items[index].name, name)) {
            if (out_local != NULL) *out_local = (uint32_t)(index + 1u);
            return &metadata->items[index];
        }
    }
    if (out_local != NULL) *out_local = 0u;
    return NULL;
}

static const CmHirDeclarationValue *find_declaration_value(
    const CmHirDeclarationMetadata *metadata, const char *name,
    uint32_t *out_local)
{
    size_t index;
    for (index = 0u; index < metadata->value_count; ++index) {
        if (declaration_string_is(metadata->values[index].name, name)) {
            if (out_local != NULL) *out_local = (uint32_t)(index + 1u);
            return &metadata->values[index];
        }
    }
    if (out_local != NULL) *out_local = 0u;
    return NULL;
}

static uint32_t declaration_variant_local(
    const CmHirDeclarationMetadata *metadata, uint32_t item_local,
    uint32_t variant_index)
{
    uint32_t local = 0u;
    uint32_t item_index;
    for (item_index = 0u; item_index < metadata->item_count; ++item_index) {
        const CmHirDeclarationItem *item = &metadata->items[item_index];
        if (item->kind != CM_HIR_DECL_ITEM_ENUM) continue;
        if (item_index + 1u == item_local) {
            return variant_index < item->variant_count
                ? local + variant_index + 1u : 0u;
        }
        local += item->variant_count;
    }
    return 0u;
}

static const CmHirDeclarationNamespaceEntry *find_namespace_entry(
    const CmHirDeclarationMetadata *metadata, uint32_t owner_module,
    uint8_t namespace_kind, const char *name)
{
    size_t index;
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        if (entry->owner_module == owner_module
            && entry->namespace_kind == namespace_kind
            && declaration_string_is(entry->name, name)) return entry;
    }
    return NULL;
}

static void assert_exact_descriptor(const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *alloc_alias_type;
    const CmHirDeclarationNamespaceEntry *alloc_error_type;
    const CmHirDeclarationNamespaceEntry *gate;
    const CmHirDeclarationNamespaceEntry *gate_alias;
    const CmHirDeclarationNamespaceEntry *alloc_alias_value;
    const CmHirDeclarationNamespaceEntry *alloc_error_value;
    const CmHirDeclarationNamespaceEntry *layout_err_direct;
    const CmHirDeclarationNamespaceEntry *layout_err_export;
    const CmHirDeclarationNamespaceEntry *layout_error_direct;
    const CmHirDeclarationNamespaceEntry *layout_error_export;
    const CmHirDeclarationNamespaceEntry *needs;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 2u && metadata->root_module == 1u
        && metadata->modules[1].parent_module == 1u
        && declaration_string_is(metadata->modules[1].name, "layout"));
    assert(metadata->trait_count == 1u && metadata->generic_count == 2u);
    assert(metadata->item_count == 3u
        && metadata->items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && metadata->items[0].owner_module == 1u
        && metadata->items[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PUBLIC
        && metadata->items[0].visibility.restriction_module == 0u
        && metadata->items[0].source_ordinal == 3u
        && metadata->items[0].alias_target_type == 0u
        && declaration_string_is(metadata->items[0].name, "AllocError")
        && metadata->items[1].kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
        && metadata->items[1].owner_module == 2u
        && metadata->items[1].source_ordinal == 0u
        && metadata->items[1].alias_target_type == 4u
        && declaration_string_is(metadata->items[1].name, "LayoutErr")
        && metadata->items[2].kind == CM_HIR_DECL_ITEM_STRUCT
        && metadata->items[2].owner_module == 2u
        && metadata->items[2].source_ordinal == 1u
        && metadata->items[2].alias_target_type == 0u
        && declaration_string_is(metadata->items[2].name, "LayoutError"));
    assert(metadata->type_count == 4u && metadata->value_count == 1u);
    assert(metadata->predicate_count == 1u
        && metadata->namespace_count == 11u);
    assert(metadata->generics[0].owner_kind == CM_HIR_DECL_GENERIC_NOMINAL
        && metadata->generics[0].owner_local == 1u
        && metadata->generics[0].index == 0u
        && metadata->generics[0].is_relaxed_sized == 1u);
    assert(metadata->generics[1].owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && metadata->generics[1].owner_local == 1u
        && metadata->generics[1].index == 0u
        && metadata->generics[1].is_relaxed_sized == 0u);
    assert(metadata->types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT);
    assert(metadata->types[1].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[1].primitive == CM_HIR_DECL_PRIMITIVE_U8);
    assert(metadata->types[2].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[2].generic_local == 2u
        && metadata->types[3].kind == CM_HIR_DECL_TYPE_NAMED_ADT
        && metadata->types[3].item_local == 3u);
    assert(metadata->values[0].kind == CM_HIR_DECL_VALUE_FUNCTION
        && metadata->values[0].declared_type == 0u
        && metadata->values[0].mutability == 0u
        && metadata->values[0].parameter_count == 0u
        && metadata->values[0].parameter_types == NULL
        && metadata->values[0].return_type == 1u
        && metadata->values[0].has_body == 1u
        && metadata->values[0].predicate_start == 1u
        && metadata->values[0].predicate_count == 1u);
    assert(metadata->predicates[0].owner_value == 1u
        && metadata->predicates[0].ordinal == 0u
        && metadata->predicates[0].subject_type == 3u
        && metadata->predicates[0].trait_local == 1u
        && metadata->predicates[0].argument_count == 1u
        && metadata->predicates[0].argument_types[0] == 2u);
    alloc_alias_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AllocAlias");
    alloc_error_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AllocError");
    gate = find_namespace_entry(metadata, 1u, CM_HIR_DECL_NAMESPACE_TYPE,
        "Gate");
    gate_alias = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "GateReexport");
    layout_err_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutErr");
    layout_error_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutError");
    alloc_alias_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "AllocAlias");
    alloc_error_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "AllocError");
    needs = find_namespace_entry(metadata, 1u, CM_HIR_DECL_NAMESPACE_VALUE,
        "needs");
    layout_err_direct = find_namespace_entry(metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutErr");
    layout_error_direct = find_namespace_entry(metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutError");
    assert(alloc_alias_type != NULL && alloc_error_type != NULL
        && gate != NULL && gate_alias != NULL && layout_err_export != NULL
        && layout_error_export != NULL && alloc_alias_value != NULL
        && alloc_error_value != NULL && needs != NULL
        && layout_err_direct != NULL && layout_error_direct != NULL);
    assert(alloc_alias_type->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alloc_alias_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_alias_type->target_local == 1u
        && alloc_alias_type->export_ordinal == 4u
        && alloc_error_type->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alloc_error_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_error_type->target_local == 1u
        && alloc_error_type->export_ordinal == 3u);
    assert(gate->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate_alias->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate_alias->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate->target_local == 1u && gate_alias->target_local == 1u
        && gate->export_ordinal == 5u
        && gate_alias->export_ordinal == 6u);
    assert(layout_err_export->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_err_export->target_local == 2u
        && layout_err_export->export_ordinal == 1u
        && layout_err_direct->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_err_direct->target_local == 2u
        && layout_err_direct->export_ordinal == 0u
        && layout_error_export->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_error_export->target_local == 3u
        && layout_error_export->export_ordinal == 2u
        && layout_error_direct->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_error_direct->target_local == 3u
        && layout_error_direct->export_ordinal == 1u
        && find_namespace_entry(metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutErr") == NULL
        && find_namespace_entry(metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutError") == NULL
        && find_namespace_entry(metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutErr") == NULL
        && find_namespace_entry(metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutError") == NULL);
    assert(alloc_alias_value->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && alloc_alias_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_alias_value->target_local == 1u
        && alloc_alias_value->export_ordinal == 4u
        && alloc_error_value->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && alloc_error_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_error_value->target_local == 1u
        && alloc_error_value->export_ordinal == 3u);
    assert(needs->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && needs->target_kind == CM_HIR_DECL_TARGET_VALUE
        && needs->target_local == 1u && needs->export_ordinal == 7u);
}

static void test_fixture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput first_input;
    CmHirDeclarationCaptureInput noisy_input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    fixture_init(&first, 0);
    fixture_init(&noisy, 1);
    first_input = capture_input(&first);
    noisy_input = capture_input(&noisy);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    result = cm_hir_declaration_metadata_capture(&first_input,
        &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s item=%u type=%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_type);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_NONE
        && result.failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE
        && result.trait_count == 1u && result.item_count == 3u
        && result.value_count == 1u && result.namespace_count == 11u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION
        && result.projected_semantic_attribute_count == 11u);
    result = cm_hir_declaration_metadata_capture(&noisy_input,
        &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    assert_exact_descriptor(&first_metadata);
    assert_exact_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_failure_is_atomic(void)
{
    static const char *const rejected_attributes[] = {
        "repr(C)",
        "lang = \"alloc_error\"",
        "rustc_layout_scalar_valid_range_start(0)",
        "no_mangle",
        "unknown_projection"
    };
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId needs_id;
    CmHirItemId alloc_id;
    const CmHirItem *needs_const;
    const CmHirItem *alloc_const;
    CmHirItem *needs;
    CmHirItem *alloc;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmInternId saved_attribute_metadata;
    size_t rejected_index;
    fixture_init(&fixture, 0);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    needs_const = find_item(&fixture, "needs", &needs_id);
    assert(needs_const != NULL);
    needs = (CmHirItem *)needs_const;
    needs->data.function_item.signature.safety = CM_HIR_UNSAFE;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == needs_id
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED
        && result.has_rejected_binding && result.has_rejected_target
        && result.has_rejected_span
        && metadata.values == saved_values
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    needs->data.function_item.signature.safety = CM_HIR_SAFE;

    alloc_const = find_item(&fixture, "AllocError", &alloc_id);
    assert(alloc_const != NULL && alloc_const->attribute_count == 2u
        && alloc_const->attributes != NULL);
    alloc = (CmHirItem *)alloc_const;
    alloc->data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == alloc_id
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.values == saved_values && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;

    alloc->generic_parameter_count = 1u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == alloc_id
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->generic_parameter_count = 0u;

    alloc->predicate_count = 1u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == alloc_id
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->predicate_count = 0u;

    saved_attribute_metadata = alloc->attributes[0].metadata;
    for (rejected_index = 0u;
            rejected_index < sizeof(rejected_attributes)
                / sizeof(rejected_attributes[0]); ++rejected_index) {
        alloc->attributes[0].metadata = cm_hir_intern(&fixture.hir,
            rejected_attributes[rejected_index]);
        result = cm_hir_declaration_metadata_capture(&input, &metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.rejected_item == alloc_id
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && result.has_rejected_span && metadata.items == saved_items
            && metadata.namespace_entries == saved_namespace
            && strcmp(cm_hir_declaration_capture_reason_name(
                result.failure_reason),
                "item-attribute-projection-unsupported") == 0);
    }
    alloc->attributes[0].metadata = saved_attribute_metadata;

    alloc->attributes[1].expansion_depth = 1u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->attributes[1].expansion_depth = 0u;

    saved_attribute_metadata = alloc->attributes[1].metadata;
    alloc->attributes[1].metadata = alloc->attributes[0].metadata;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->attributes[1].metadata = saved_attribute_metadata;

    alloc->attribute_count = 0u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->attribute_count = 2u;

    input.revision += UINT64_C(1);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH
        && metadata.values == saved_values
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_zero_item_gate_path(void)
{
    static const unsigned char gate_source[] =
        "pub trait Gate<T: ?Sized> {}\n"
        "pub use Gate as GateReexport;\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    fixture_init_source(&fixture, 0, "zero-item-gate.rs", gate_source,
        sizeof(gate_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 0u && metadata.item_count == 0u
        && metadata.items == NULL && result.namespace_count == 3u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE
        && result.projected_semantic_attribute_count == 0u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_plain_unit_struct_has_exact_empty_attribute_profile(void)
{
    static const unsigned char plain_source[] =
        "pub struct Plain;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationNamespaceEntry *plain_type;
    const CmHirDeclarationNamespaceEntry *plain_value;
    fixture_init_source(&fixture, 0, "plain-unit.rs", plain_source,
        sizeof(plain_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && metadata.item_count == 1u
        && result.namespace_count == 4u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE
        && result.projected_semantic_attribute_count == 0u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    plain_type = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Plain");
    plain_value = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "Plain");
    assert(plain_type != NULL && plain_value != NULL
        && plain_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && plain_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && plain_type->target_local == plain_value->target_local
        && plain_type->export_ordinal == plain_value->export_ordinal);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_module_attribute_projection_and_provenance(void)
{
    static const unsigned char plain_source[] =
        "pub mod child {}\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char attributed_source[] =
        "#![allow(dead_code)]\n"
        "#[allow(non_snake_case)]\n"
        "pub mod child { #![allow(unused)] }\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture plain;
    CaptureFixture attributed;
    CmHirDeclarationCaptureInput plain_input;
    CmHirDeclarationCaptureInput attributed_input;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationMetadata attributed_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf plain_bytes;
    CmByteBuf attributed_bytes;
    CmHirCrate *crate_value;
    CmHirModule *child;
    CmHirAttribute *saved_child_attributes;
    CmInternId saved_metadata;
    CmSpan saved_span;
    CmHirDeclarationModule *saved_modules;
    CmHirDeclarationNamespaceEntry *saved_namespace;

    fixture_init_source(&plain, 0, "module-attributes.rs", plain_source,
        sizeof(plain_source) - 1u);
    fixture_init_source(&attributed, 0, "module-attributes.rs",
        attributed_source, sizeof(attributed_source) - 1u);
    plain_input = capture_input(&plain);
    attributed_input = capture_input(&attributed);
    cm_hir_declaration_metadata_init(&plain_metadata);
    cm_hir_declaration_metadata_init(&attributed_metadata);
    result = cm_hir_declaration_metadata_capture(&plain_input,
        &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE);
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 3u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION);
    cm_byte_buf_init(&plain_bytes);
    cm_byte_buf_init(&attributed_bytes);
    assert(cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&attributed_metadata,
            &attributed_bytes) == CM_HIR_DECL_METADATA_OK
        && plain_bytes.len == attributed_bytes.len
        && memcmp(plain_bytes.data, attributed_bytes.data,
            plain_bytes.len) == 0);

    saved_modules = attributed_metadata.modules;
    saved_namespace = attributed_metadata.namespace_entries;
    crate_value = (CmHirCrate *)cm_hir_get_crate(&attributed.hir,
        attributed.lower_result.crate_id);
    child = (CmHirModule *)find_module(&attributed, "child");
    assert(crate_value != NULL && crate_value->inner_attribute_count == 1u
        && crate_value->inner_attributes != NULL && child != NULL
        && child->outer_attribute_count == 1u
        && child->inner_attribute_count == 1u
        && child->inner_attributes != NULL);

    crate_value->inner_attribute_count = 0u;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_MODULES
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    crate_value->inner_attribute_count = 1u;

    saved_metadata = crate_value->inner_attributes[0].metadata;
    crate_value->inner_attributes[0].metadata = CM_INTERN_ID_NONE;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    crate_value->inner_attributes[0].metadata = saved_metadata;

    saved_span = crate_value->inner_attributes[0].span;
    crate_value->inner_attributes[0].span.start = 2u;
    crate_value->inner_attributes[0].span.end = 1u;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && result.has_rejected_span
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    crate_value->inner_attributes[0].span = saved_span;

    saved_child_attributes = child->inner_attributes;
    child->inner_attributes = NULL;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    child->inner_attributes = saved_child_attributes;

    cm_byte_buf_destroy(&attributed_bytes);
    cm_byte_buf_destroy(&plain_bytes);
    cm_hir_declaration_metadata_destroy(&attributed_metadata);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    fixture_destroy(&attributed);
    fixture_destroy(&plain);
}

static void test_item_shape_diagnostic(void)
{
    static const unsigned char unsupported_source[] =
        "pub struct Blocked(pub u8);\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    const CmSourceFile *source;

    fixture_init_source(&fixture, 0, "unsupported-public-struct.rs",
        unsupported_source, sizeof(unsupported_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        || result.failure_stage != CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        || result.failure_reason
            != CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED) {
        fprintf(stderr, "item diagnostic status=%s stage=%s reason=%s "
            "binding=%u ast=%u def=%u:%u item=%u:%u span=%d\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            (unsigned int)result.rejected_binding_kind,
            (unsigned int)result.rejected_ast_item_kind,
            (unsigned int)result.rejected_definition.crate_id,
            (unsigned int)result.rejected_definition.index,
            (unsigned int)result.rejected_source_item.source,
            (unsigned int)result.rejected_source_item.item,
            result.has_rejected_span);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && result.has_rejected_binding && result.has_rejected_target
        && result.rejected_binding_kind == CM_HIR_LIBRARY_BINDING_TYPE
        && result.rejected_ast_item_kind == CM_AST_ITEM_STRUCT
        && result.rejected_namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && result.rejected_definition.crate_id
            == fixture.lower_result.crate_id
        && result.rejected_definition.index != CM_HIR_DEF_INDEX_NONE
        && result.rejected_source_item.source != 0u
        && result.rejected_source_item.item != CM_AST_ITEM_NONE
        && result.has_rejected_span);
    source = cm_source_get(&fixture.sources, result.rejected_span.source);
    assert(source != NULL
        && strcmp(source->path, "unsupported-public-struct.rs") == 0
        && result.rejected_span.start == 0u
        && metadata.modules == NULL && metadata.module_count == 0u);
    assert(strcmp(cm_hir_declaration_capture_stage_name(
            result.failure_stage), "items") == 0
        && strcmp(cm_hir_declaration_capture_reason_name(
            result.failure_reason), "item-shape-unsupported") == 0);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_non_exhaustive_authorizes_missing_constructor_mate(void)
{
    static const unsigned char non_exhaustive_source[] =
        "#[non_exhaustive]\n"
        "pub struct SealedUnit;\n"
        "pub use SealedUnit as SealedAlias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationNamespaceEntry *sealed;
    const CmHirDeclarationNamespaceEntry *sealed_alias;

    fixture_init_source(&fixture, 0, "non-exhaustive-unit.rs",
        non_exhaustive_source, sizeof(non_exhaustive_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && result.namespace_count == 4u
        && result.projected_semantic_attribute_count == 1u
        && metadata.item_count == 1u
        && metadata.items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && declaration_string_is(metadata.items[0].name, "SealedUnit"));
    sealed = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "SealedUnit");
    sealed_alias = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "SealedAlias");
    assert(sealed != NULL && sealed_alias != NULL
        && sealed->target_kind == CM_HIR_DECL_TARGET_ITEM
        && sealed_alias->target_kind == CM_HIR_DECL_TARGET_ITEM
        && sealed->target_local == 1u && sealed_alias->target_local == 1u
        && find_namespace_entry(&metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "SealedUnit") == NULL
        && find_namespace_entry(&metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "SealedAlias") == NULL
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_char_shaped_reexport_projection(void)
{
    static const unsigned char attributed_source[] =
        "mod ascii_char {\n"
        "  #[non_exhaustive]\n"
        "  pub struct AsciiChar;\n"
        "}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char plain_source[] =
        "mod ascii_char {\n"
        "  #[non_exhaustive]\n"
        "  pub struct AsciiChar;\n"
        "}\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture attributed;
    CaptureFixture plain;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata attributed_metadata;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf attributed_bytes;
    CmByteBuf plain_bytes;
    const CmHirDeclarationNamespaceEntry *definition;
    const CmHirDeclarationNamespaceEntry *reexport;
    fixture_init_source(&attributed, 0, "ascii-char-projection.rs",
        attributed_source, sizeof(attributed_source) - 1u);
    fixture_init_source(&plain, 0, "ascii-char-projection.rs", plain_source,
        sizeof(plain_source) - 1u);
    cm_hir_declaration_metadata_init(&attributed_metadata);
    cm_hir_declaration_metadata_init(&plain_metadata);
    input = capture_input(&attributed);
    result = cm_hir_declaration_metadata_capture(&input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && result.namespace_count == 4u
        && result.projected_semantic_attribute_count == 3u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION);
    definition = find_namespace_entry(&attributed_metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AsciiChar");
    reexport = find_namespace_entry(&attributed_metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Char");
    assert(definition != NULL && reexport != NULL
        && definition->target_kind == CM_HIR_DECL_TARGET_ITEM
        && reexport->target_kind == CM_HIR_DECL_TARGET_ITEM
        && definition->target_local == reexport->target_local
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "Char") == NULL);
    input = capture_input(&plain);
    result = cm_hir_declaration_metadata_capture(&input, &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 1u);
    cm_byte_buf_init(&attributed_bytes);
    cm_byte_buf_init(&plain_bytes);
    assert(cm_hir_declaration_metadata_encode(&attributed_metadata,
            &attributed_bytes) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && attributed_bytes.len == plain_bytes.len
        && memcmp(attributed_bytes.data, plain_bytes.data,
            attributed_bytes.len) == 0);
    cm_byte_buf_destroy(&plain_bytes);
    cm_byte_buf_destroy(&attributed_bytes);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    cm_hir_declaration_metadata_destroy(&attributed_metadata);
    fixture_destroy(&plain);
    fixture_destroy(&attributed);
}

static void test_ascii_char_enum_projection_and_determinism(void)
{
    static const unsigned char source[] =
        "mod ascii_char {\n"
        "  #[derive(Copy, Clone, Eq, PartialEq)]\n"
        "  #[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "  #[repr(u8)]\n"
        "  pub enum AsciiChar {\n"
        "    #[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "    Null = 0,\n"
        "    #[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "    StartOfHeading = 1,\n"
        "  }\n"
        "}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    const CmHirDeclarationNamespaceEntry *definition;
    const CmHirDeclarationNamespaceEntry *reexport;
    fixture_init_source(&first, 0, "ascii-char-enum.rs", source,
        sizeof(source) - 1u);
    fixture_init_source(&noisy, 1, "ascii-char-enum.rs", source,
        sizeof(source) - 1u);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "enum capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s item=%u span=%u:%u-%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_span.source,
            (unsigned int)result.rejected_span.start,
            (unsigned int)result.rejected_span.end);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && result.namespace_count == 4u
        && result.projected_semantic_attribute_count == 6u
        && first_metadata.item_count == 1u
        && first_metadata.items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && first_metadata.items[0].enum_repr_primitive
            == CM_HIR_DECL_PRIMITIVE_U8
        && first_metadata.items[0].diagnostic_item.data == NULL
        && first_metadata.items[0].diagnostic_item.length == 0u
        && first_metadata.items[0].variant_count == 2u
        && first_metadata.items[0].variants != NULL
        && first_metadata.items[0].variants[0].kind
            == CM_HIR_DECL_VARIANT_UNIT
        && first_metadata.items[0].variants[0].source_ordinal == 0u
        && first_metadata.items[0].variants[0].discriminant_primitive
            == CM_HIR_DECL_PRIMITIVE_ISIZE
        && first_metadata.items[0].variants[0].discriminant_low == 0u
        && first_metadata.items[0].variants[0].discriminant_high == 0u
        && declaration_string_is(first_metadata.items[0].variants[0].name,
            "Null")
        && first_metadata.items[0].variants[1].source_ordinal == 1u
        && first_metadata.items[0].variants[1].discriminant_low == 1u
        && declaration_string_is(first_metadata.items[0].variants[1].name,
            "StartOfHeading"));
    definition = find_namespace_entry(&first_metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AsciiChar");
    reexport = find_namespace_entry(&first_metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Char");
    assert(definition != NULL && reexport != NULL
        && definition->target_kind == CM_HIR_DECL_TARGET_ITEM
        && reexport->target_kind == CM_HIR_DECL_TARGET_ITEM
        && definition->target_local == 1u
        && reexport->target_local == definition->target_local
        && find_namespace_entry(&first_metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "AsciiChar") == NULL
        && find_namespace_entry(&first_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "Char") == NULL);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 6u);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void assert_default_enum_descriptor(
    const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *basic_block;
    const CmHirDeclarationNamespaceEntry *unwind;
    const CmHirDeclarationNamespaceEntry *reason_abi_type;
    const CmHirDeclarationNamespaceEntry *reason_abi_value;
    const CmHirDeclarationNamespaceEntry *reason_cleanup_type;
    const CmHirDeclarationNamespaceEntry *reason_cleanup_value;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u
        && metadata->item_count == 2u
        && metadata->namespace_count == 8u
        && metadata->items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && metadata->items[0].owner_module == 1u
        && metadata->items[0].source_ordinal == 0u
        && declaration_string_is(metadata->items[0].name, "BasicBlock")
        && metadata->items[0].enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_RUST
        && declaration_string_is(metadata->items[0].diagnostic_item,
            "mir_basic_block")
        && metadata->items[0].variant_count == 2u
        && declaration_string_is(metadata->items[0].variants[0].name,
            "Normal")
        && metadata->items[0].variants[0].source_ordinal == 0u
        && metadata->items[0].variants[0].discriminant_primitive
            == CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
        && metadata->items[0].variants[0].discriminant_low == 0u
        && metadata->items[0].variants[0].discriminant_high == 0u
        && declaration_string_is(metadata->items[0].variants[1].name,
            "Cleanup")
        && metadata->items[0].variants[1].source_ordinal == 1u
        && metadata->items[0].variants[1].discriminant_primitive
            == CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
        && metadata->items[0].variants[1].discriminant_low == 0u
        && metadata->items[0].variants[1].discriminant_high == 0u
        && metadata->items[1].kind == CM_HIR_DECL_ITEM_ENUM
        && metadata->items[1].owner_module == 1u
        && metadata->items[1].source_ordinal == 1u
        && declaration_string_is(metadata->items[1].name,
            "UnwindTerminateReason")
        && metadata->items[1].enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_RUST
        && declaration_string_is(metadata->items[1].diagnostic_item,
            "mir_unwind_terminate_reason")
        && metadata->items[1].variant_count == 2u
        && declaration_string_is(metadata->items[1].variants[0].name,
            "Abi")
        && declaration_string_is(metadata->items[1].variants[1].name,
            "InCleanup"));
    basic_block = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "BasicBlock");
    unwind = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "UnwindTerminateReason");
    reason_abi_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "ReasonAbi");
    reason_abi_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "ReasonAbi");
    reason_cleanup_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "ReasonInCleanup");
    reason_cleanup_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "ReasonInCleanup");
    assert(basic_block != NULL && unwind != NULL
        && reason_abi_type != NULL && reason_abi_value != NULL
        && reason_cleanup_type != NULL && reason_cleanup_value != NULL
        && basic_block->target_kind == CM_HIR_DECL_TARGET_ITEM
        && basic_block->target_local == 1u
        && unwind->target_kind == CM_HIR_DECL_TARGET_ITEM
        && unwind->target_local == 2u
        && reason_abi_type->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_abi_value->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_abi_type->target_local == 3u
        && reason_abi_value->target_local == 3u
        && reason_abi_type->export_ordinal
            == reason_abi_value->export_ordinal
        && reason_cleanup_type->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_cleanup_value->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_cleanup_type->target_local == 4u
        && reason_cleanup_value->target_local == 4u
        && reason_cleanup_type->export_ordinal
            == reason_cleanup_value->export_ordinal);
}

static void test_default_enum_variant_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    default_enum_fixture_init(&first, 0);
    default_enum_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "default enum capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s binding=%u ast=%u namespace=%u "
            "item=%u def=%u:%u span=%u:%u-%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_binding_kind,
            (unsigned int)result.rejected_ast_item_kind,
            (unsigned int)result.rejected_namespace_kind,
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_definition.crate_id,
            (unsigned int)result.rejected_definition.index,
            (unsigned int)result.rejected_span.source,
            (unsigned int)result.rejected_span.start,
            (unsigned int)result.rejected_span.end);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 2u && result.namespace_count == 8u
        && result.projected_semantic_attribute_count == 0u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u);
    assert_default_enum_descriptor(&first_metadata);
    assert_default_enum_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_default_enum_hostile_mutations_are_atomic(void)
{
    static const char *const rejected_sources[] = {
        "#[rustc_diagnostic_item = \"\"]\n"
        "pub enum Bad { One }\n",
        "#[rustc_diagnostic_item = \"bad-item\"]\n"
        "pub enum Bad { One }\n",
        "#[rustc_diagnostic_item = \"bad_explicit\"]\n"
        "pub enum Bad { One = 0 }\n",
        "#[rustc_diagnostic_item = \"bad_attr\"]\n"
        "pub enum Bad { #[unstable(feature = \"bad\", issue = \"none\")] One }\n",
        "#[rustc_diagnostic_item = \"bad_tuple\"]\n"
        "pub enum Bad { One(u8) }\n"
    };
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    const CmHirItem *item_const;
    CmHirItem *item;
    CmHirItemId item_id;
    CmInternId saved_metadata;
    CmHirAttribute *saved_attributes;
    CmSpan saved_attribute_span;
    CmHirDefId saved_variant_definition;
    CmHirAggregateForm saved_variant_form;
    CmHirImportBinding *reason_abi_value = NULL;
    CmHirDefId reason_cleanup_definition;
    int saved_has_discriminant;
    size_t index;
    default_enum_fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    item_const = find_item(&good, "BasicBlock", &item_id);
    assert(item_const != NULL && item_const->kind == CM_HIR_ITEM_ENUM
        && item_const->attribute_count == 1u
        && item_const->attributes != NULL
        && item_const->data.enum_item.variant_count == 2u);
    item = (CmHirItem *)item_const;

#define ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&good.hir,
        "rustc_diagnostic_item = \"forged-name\"");
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes[0].metadata = saved_metadata;

    item->attributes[0].expansion_depth = 1u;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes[0].expansion_depth = 0u;

    saved_attribute_span = item->attributes[0].span;
    item->attributes[0].span.start += 1u;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes[0].span = saved_attribute_span;

    saved_attributes = item->attributes;
    item->attributes = NULL;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes = saved_attributes;

    saved_has_discriminant =
        item->data.enum_item.variants[0].has_discriminant;
    item->data.enum_item.variants[0].has_discriminant = 1;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->data.enum_item.variants[0].has_discriminant =
        saved_has_discriminant;

    saved_variant_definition =
        item->data.enum_item.variants[0].definition;
    item->data.enum_item.variants[0].definition =
        item->data.enum_item.variants[1].definition;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->data.enum_item.variants[0].definition = saved_variant_definition;

    saved_variant_form = item->data.enum_item.variants[0].form;
    item->data.enum_item.variants[0].form = CM_HIR_AGGREGATE_TUPLE;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->data.enum_item.variants[0].form = saved_variant_form;

    memset(&reason_cleanup_definition, 0, sizeof(reason_cleanup_definition));
    for (index = 0u; index < good.hir.modules.len; ++index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(&good.hir.modules,
            index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != good.lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *import = &module->imports[import_index];
            uint32_t binding_index;
            for (binding_index = 0u; binding_index < import->binding_count;
                    ++binding_index) {
                CmHirImportBinding *binding =
                    &import->bindings[binding_index];
                const CmInternedString *name = cm_interner_get(
                    &good.hir.strings, binding->name);
                if (name != NULL && name->len == strlen("ReasonAbi")
                    && memcmp(name->bytes, "ReasonAbi", name->len) == 0
                    && binding->namespace_kind == CM_HIR_NAMESPACE_VALUE)
                    reason_abi_value = binding;
                if (name != NULL && name->len == strlen("ReasonInCleanup")
                    && memcmp(name->bytes, "ReasonInCleanup",
                        name->len) == 0
                    && binding->namespace_kind == CM_HIR_NAMESPACE_VALUE)
                    reason_cleanup_definition = binding->target;
            }
        }
    }
    assert(reason_abi_value != NULL
        && !cm_hir_def_id_is_none(reason_cleanup_definition)
        && !cm_hir_def_id_equal(reason_abi_value->target,
            reason_cleanup_definition));
    saved_variant_definition = reason_abi_value->target;
    reason_abi_value->target = reason_cleanup_definition;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    reason_abi_value->target = saved_variant_definition;

    for (index = 0u;
            index < sizeof(rejected_sources) / sizeof(rejected_sources[0]);
            ++index) {
        CaptureFixture rejected;
        char source[2048];
        int written = snprintf(source, sizeof(source), "%s"
            "pub trait Gate<T: ?Sized> {}\n"
            "pub fn needs<X: Gate<u8>>() {}\n", rejected_sources[index]);
        assert(written > 0 && (size_t)written < sizeof(source));
        fixture_init_source(&rejected, 0, "bad-default-enum.rs",
            (const unsigned char *)source, (size_t)written);
        input = capture_input(&rejected);
        ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
        fixture_destroy(&rejected);
    }
    assert_default_enum_descriptor(&metadata);
#undef ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void test_ascii_char_128_variant_projection(void)
{
    char source[32768];
    size_t cursor = 0u;
    uint32_t index;
    int written;
    CaptureFixture fixture;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    written = snprintf(source + cursor, sizeof(source) - cursor,
        "mod ascii_char {\n"
        "#[derive(Copy, Clone, Eq, PartialEq)]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "#[repr(u8)]\n"
        "pub enum AsciiChar {\n");
    assert(written > 0 && (size_t)written < sizeof(source) - cursor);
    cursor += (size_t)written;
    for (index = 0u; index < 128u; ++index) {
        written = snprintf(source + cursor, sizeof(source) - cursor,
            "#[unstable(feature = \"ascii_char_variants\", "
            "issue = \"110998\")] Variant%u = %u,\n",
            (unsigned int)index, (unsigned int)index);
        assert(written > 0 && (size_t)written < sizeof(source) - cursor);
        cursor += (size_t)written;
    }
    written = snprintf(source + cursor, sizeof(source) - cursor,
        "}\n}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n");
    assert(written > 0 && (size_t)written < sizeof(source) - cursor);
    cursor += (size_t)written;
    fixture_init_source(&fixture, 0, "ascii-char-128.rs",
        (const unsigned char *)source, cursor);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 132u
        && metadata.item_count == 1u
        && metadata.items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && metadata.items[0].variant_count == 128u
        && metadata.items[0].variants[127].source_ordinal == 127u
        && metadata.items[0].variants[127].discriminant_low == 127u
        && declaration_string_is(metadata.items[0].variants[127].name,
            "Variant127")
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void fixture_init_enum_case(CaptureFixture *fixture,
    const char *path, const char *repr_attribute,
    const char *item_stability_attribute, const char *variant_source,
    const char *extra_module_item)
{
    char source[8192];
    int written = snprintf(source, sizeof(source),
        "mod ascii_char {\n"
        "#[derive(Copy, Clone, Eq, PartialEq)]\n"
        "#[%s]\n"
        "#[%s]\n"
        "pub enum AsciiChar {\n%s}\n%s}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        item_stability_attribute, repr_attribute, variant_source,
        extra_module_item);
    assert(written > 0 && (size_t)written < sizeof(source));
    fixture_init_source(fixture, 0, path, (const unsigned char *)source,
        (size_t)written);
}

static void assert_enum_failure_is_atomic(CaptureFixture *fixture,
    CmHirDeclarationMetadata *metadata, CmHirDeclarationItem *saved_items,
    CmHirDeclarationNamespaceEntry *saved_namespace)
{
    CmHirDeclarationCaptureInput input = capture_input(fixture);
    CmHirDeclarationCaptureResult result =
        cm_hir_declaration_metadata_capture(&input, metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata->items == saved_items
        && metadata->namespace_entries == saved_namespace);
}

static void test_enum_cfg_source_ordinal_and_atomic_negatives(void)
{
    static const char good_variants[] =
        "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "Null = 0,\n"
        "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "StartOfHeading = 1,\n";
    static const char cfg_variants[] =
        "#[cfg(any())]\n"
        "Hidden = 99,\n"
        "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "Null = 0,\n";
    static const struct {
        const char *path;
        const char *repr_attribute;
        const char *item_attribute;
        const char *variants;
        const char *extra;
    } rejected[] = {
        { "enum-tuple.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "Tuple(u8) = 0,\n", "" },
        { "enum-named.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "Named { value: u8 } = 0,\n", "" },
        { "enum-implicit.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "Implicit,\n", "" },
        { "enum-range.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "TooLarge = 256,\n", "" },
        { "enum-item-attr.rs", "repr(u8)",
          "stable(feature = \"ascii_char\", since = \"1.0.0\")",
          good_variants, "" },
        { "enum-variant-attr.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[stable(feature = \"ascii_char_variants\", since = \"1.0.0\")]\n"
          "Null = 0,\n", "" },
        { "enum-generated-variant-attr.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[cfg_attr(all(), unstable(feature = \"ascii_char_variants\", "
          "issue = \"110998\"))]\nNull = 0,\n", "" }
    };
    CaptureFixture good;
    CaptureFixture cfg;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId item_id;
    CmHirItemId needs_id;
    const CmHirItem *item_const;
    const CmHirItem *needs_const;
    CmHirItem *item;
    CmHirItem *needs;
    CmHirDefId saved_definition;
    uint64_t saved_discriminant;
    CmSpan saved_span;
    CmInternId saved_metadata;
    size_t index;
    fixture_init_enum_case(&good, "enum-atomic.rs", "repr(u8)",
        "unstable(feature = \"ascii_char\", issue = \"110998\")",
        good_variants, "");
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    item_const = find_item(&good, "AsciiChar", &item_id);
    needs_const = find_item(&good, "needs", &needs_id);
    assert(item_const != NULL && item_const->kind == CM_HIR_ITEM_ENUM
        && item_const->data.enum_item.variant_count == 2u
        && needs_const != NULL);
    item = (CmHirItem *)item_const;
    needs = (CmHirItem *)needs_const;

    saved_discriminant =
        item->data.enum_item.variants[0].discriminant.data.value.low_bits;
    item->data.enum_item.variants[0].discriminant.data.value.low_bits = 2u;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->data.enum_item.variants[0].discriminant.data.value.low_bits =
        saved_discriminant;

    saved_span = item->data.enum_item.variants[0].span;
    item->data.enum_item.variants[0].span.start += 1u;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->data.enum_item.variants[0].span = saved_span;

    saved_metadata = item->attributes[2].metadata;
    item->attributes[2].metadata = cm_hir_intern(&good.hir, "repr(u16)");
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->attributes[2].metadata = saved_metadata;

    item->attributes[1].expansion_depth = 1u;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->attributes[1].expansion_depth = 0u;

    /* A forged VALUE may not borrow the enum ITEM identity. */
    saved_definition = needs->definition;
    needs->definition = item->definition;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    needs->definition = saved_definition;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
            ++index) {
        CaptureFixture bad;
        fixture_init_enum_case(&bad, rejected[index].path,
            rejected[index].repr_attribute, rejected[index].item_attribute,
            rejected[index].variants, rejected[index].extra);
        assert_enum_failure_is_atomic(&bad, &metadata, saved_items,
            saved_namespace);
        fixture_destroy(&bad);
    }

    fixture_init_enum_case(&cfg, "enum-cfg-ordinal.rs", "repr(u8)",
        "unstable(feature = \"ascii_char\", issue = \"110998\")",
        cfg_variants, "");
    item_const = find_item(&cfg, "AsciiChar", &item_id);
    /* The graph has one effective variant at raw source ordinal 1, while
     * current graph-backed lowering retains both raw variants. Capture must
     * reject that live-HIR/effective-graph census mismatch atomically. */
    assert(item_const != NULL
        && item_const->data.enum_item.variant_count == 2u);
    assert_enum_failure_is_atomic(&cfg, &metadata, saved_items,
        saved_namespace);
    fixture_destroy(&cfg);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void assert_reexport_projection_failure(CaptureFixture *fixture,
    CmHirDeclarationMetadata *metadata,
    CmHirDeclarationItem *saved_items,
    CmHirDeclarationNamespaceEntry *saved_namespace,
    uint32_t rejected_attribute)
{
    CmHirDeclarationCaptureInput input = capture_input(fixture);
    CmHirDeclarationCaptureResult result;
    CmHirImport *import = find_unique_attributed_import(fixture);
    CmSpan expected_span;
    assert(import != NULL && import->attributes != NULL
        && rejected_attribute < import->attribute_count);
    expected_span = import->attributes[rejected_attribute].span;
    result = cm_hir_declaration_metadata_capture(&input, metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && result.has_rejected_binding && result.has_rejected_target
        && result.rejected_binding_kind == CM_HIR_LIBRARY_BINDING_TYPE
        && result.rejected_ast_item_kind == CM_AST_ITEM_STRUCT
        && result.rejected_namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && result.rejected_definition.crate_id
            == fixture->lower_result.crate_id
        && result.rejected_definition.index != CM_HIR_DEF_INDEX_NONE
        && result.rejected_source_item.source == import->span.source
        && result.rejected_source_item.item == import->source_item
        && result.has_rejected_span
        && result.rejected_span.source == expected_span.source
        && result.rejected_span.start == expected_span.start
        && result.rejected_span.end == expected_span.end
        && metadata->items == saved_items
        && metadata->namespace_entries == saved_namespace);
}

static void test_reexport_alias_spelling_and_duplicate_negatives(void)
{
    static const char *const sources[] = {
        "pub struct Unit;\n"
        "#[doc(alias = \"Unit\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[doc(alias(\"\"))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[doc(alias(\"not-an-identifier\"))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[doc(alias(\"Unit\"))]\n"
        "#[doc(alias(\"Other\"))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[unstable(feature = \"unit\", issue = \"none\")]\n"
        "#[unstable(feature = \"other\", issue = \"none\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n"
    };
    static const uint32_t rejected_attributes[] = { 0u, 0u, 0u, 1u, 1u };
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    size_t index;
    fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    for (index = 0u; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        CaptureFixture rejected;
        fixture_init_source(&rejected, 0, "bad-doc-alias.rs",
            (const unsigned char *)sources[index], strlen(sources[index]));
        assert_reexport_projection_failure(&rejected, &metadata, saved_items,
            saved_namespace, rejected_attributes[index]);
        fixture_destroy(&rejected);
    }
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void test_reexport_provenance_and_generated_negatives(void)
{
    static const unsigned char source[] =
        "pub struct Unit;\n"
        "#[doc(alias(\"Unit\"))]\n"
        "#[unstable(feature = \"unit\", issue = \"none\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char generated_attribute_source[] =
        "pub struct Unit;\n"
        "#[cfg_attr(all(), doc(alias(\"Unit\")))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CaptureFixture generated_attribute;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirImport *import;
    CmInternId saved_metadata;
    CmSpan saved_span;
    fixture_init_source(&fixture, 0, "reexport-provenance.rs", source,
        sizeof(source) - 1u);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    import = find_unique_attributed_import(&fixture);
    assert(import != NULL && import->attribute_count == 2u
        && import->attributes != NULL);

    saved_metadata = import->attributes[0].metadata;
    import->attributes[0].metadata = cm_hir_intern(&fixture.hir,
        "doc(alias(\"Forged\"))");
    assert_reexport_projection_failure(&fixture, &metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].metadata = saved_metadata;

    saved_span = import->attributes[0].span;
    import->attributes[0].span.start += 1u;
    assert_reexport_projection_failure(&fixture, &metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].span = saved_span;

    import->attributes[1].expansion_depth = 1u;
    assert_reexport_projection_failure(&fixture, &metadata, saved_items,
        saved_namespace, 1u);
    import->attributes[1].expansion_depth = 0u;

    fixture_init_source(&generated_attribute, 0,
        "generated-attribute-reexport.rs", generated_attribute_source,
        sizeof(generated_attribute_source) - 1u);
    assert_reexport_projection_failure(&generated_attribute, &metadata,
        saved_items, saved_namespace, 0u);
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&generated_attribute);
    fixture_destroy(&fixture);
}

static void test_rustfmt_skip_reexport_projection_and_negatives(void)
{
    static const unsigned char source[] =
        "mod convert { pub struct CharTryFromError; }\n"
        "#[rustfmt::skip]\n"
        "#[stable(feature = \"try_from\", since = \"1.34.0\")]\n"
        "pub use self::convert::CharTryFromError;\n"
        "use self::convert::CharTryFromError as PrivateError;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char projected_source[] =
        "mod convert { pub struct CharTryFromError; }\n"
        "pub use self::convert::CharTryFromError;\n"
        "#[rustfmt::skip]\n"
        "use self::convert::CharTryFromError as PrivateError;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const struct {
        const char *path;
        const unsigned char *source;
        size_t source_length;
        uint32_t rejected_attribute;
    } rejected[] = {
        { "rustfmt-skip-call.rs",
            (const unsigned char *)
                "mod convert { pub struct CharTryFromError; }\n"
                "#[rustfmt::skip()]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod convert { pub struct CharTryFromError; }\n"
                "#[rustfmt::skip()]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u },
        { "rustfmt-skip-malformed.rs",
            (const unsigned char *)
                "mod convert { pub struct CharTryFromError; }\n"
                "#[rustfmt::skip = \"yes\"]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod convert { pub struct CharTryFromError; }\n"
                "#[rustfmt::skip = \"yes\"]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u },
        { "rustfmt-skip-duplicate.rs",
            (const unsigned char *)
                "mod convert { pub struct CharTryFromError; }\n"
                "#[rustfmt::skip]\n"
                "#[rustfmt::skip]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod convert { pub struct CharTryFromError; }\n"
                "#[rustfmt::skip]\n"
                "#[rustfmt::skip]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 1u },
        { "rustfmt-skip-generated.rs",
            (const unsigned char *)
                "mod convert { pub struct CharTryFromError; }\n"
                "#[cfg_attr(all(), rustfmt::skip)]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod convert { pub struct CharTryFromError; }\n"
                "#[cfg_attr(all(), rustfmt::skip)]\n"
                "pub use convert::CharTryFromError;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u }
    };
    static const unsigned char item_attribute_source[] =
        "#[rustfmt::skip]\n"
        "pub struct CharTryFromError;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture projected;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata projected_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirImport *import;
    CmInternId saved_metadata;
    CmSpan saved_span;
    uint32_t saved_source_attribute;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    CmByteBuf projected_bytes;
    size_t index;
    fixture_init_source(&first, 0, "char-try-from-error.rs", source,
        sizeof(source) - 1u);
    fixture_init_source(&noisy, 1, "char-try-from-error.rs", source,
        sizeof(source) - 1u);
    fixture_init_source(&projected, 0, "char-try-from-error.rs",
        projected_source, sizeof(projected_source) - 1u);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    cm_hir_declaration_metadata_init(&projected_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u
        && find_namespace_entry(&first_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "CharTryFromError") != NULL);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    input = capture_input(&projected);
    result = cm_hir_declaration_metadata_capture(&input,
        &projected_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    cm_byte_buf_init(&projected_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&projected_metadata,
            &projected_bytes) == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0
        && first_bytes.len == projected_bytes.len
        && memcmp(first_bytes.data, projected_bytes.data,
            first_bytes.len) == 0);
    cm_byte_buf_destroy(&projected_bytes);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    saved_items = first_metadata.items;
    saved_namespace = first_metadata.namespace_entries;
    import = find_import_with_attribute_count(&first, 2u);
    assert(import != NULL && import->attribute_count == 2u
        && import->attributes != NULL);

    saved_metadata = import->attributes[0].metadata;
    import->attributes[0].metadata = cm_hir_intern(&first.hir,
        "rustfmt::skip()");
    assert_reexport_projection_failure(&first, &first_metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].metadata = saved_metadata;

    saved_span = import->attributes[0].span;
    import->attributes[0].span.start += 1u;
    assert_reexport_projection_failure(&first, &first_metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].span = saved_span;

    saved_source_attribute = import->attributes[0].source_attribute;
    import->attributes[0].source_attribute += 1u;
    assert_reexport_projection_failure(&first, &first_metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].source_attribute = saved_source_attribute;

    import->attributes[0].expansion_depth = 1u;
    assert_reexport_projection_failure(&first, &first_metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].expansion_depth = 0u;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
            ++index) {
        CaptureFixture bad;
        fixture_init_source(&bad, 0, rejected[index].path,
            rejected[index].source, rejected[index].source_length);
        assert_reexport_projection_failure(&bad, &first_metadata,
            saved_items, saved_namespace, rejected[index].rejected_attribute);
        fixture_destroy(&bad);
    }
    {
        CaptureFixture item_attribute;
        fixture_init_source(&item_attribute, 0, "rustfmt-skip-item.rs",
            item_attribute_source, sizeof(item_attribute_source) - 1u);
        input = capture_input(&item_attribute);
        result = cm_hir_declaration_metadata_capture(&input,
            &first_metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && first_metadata.items == saved_items
            && first_metadata.namespace_entries == saved_namespace);
        fixture_destroy(&item_attribute);
    }
    assert(cm_hir_declaration_metadata_validate(&first_metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&projected_metadata);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&projected);
    fixture_destroy(&first);
}

static void test_doc_inline_reexport_projection_and_negatives(void)
{
    static const unsigned char attributed_source[] =
        "mod ffi { pub struct CStr; pub struct CString; }\n"
        "#[doc(inline)]\n"
        "#[stable(feature = \"cstr\", since = \"1.64.0\")]\n"
        "pub use self::ffi::CStr;\n"
        "#[doc(inline)]\n"
        "#[stable(feature = \"cstring\", since = \"1.64.0\")]\n"
        "pub use self::ffi::{CString as OwnedCStr, CStr as BorrowedCStr};\n"
        "#[doc(inline)]\n"
        "use self::ffi::CString as PrivateCString;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char plain_source[] =
        "mod ffi { pub struct CStr; pub struct CString; }\n"
        "pub use self::ffi::CStr;\n"
        "pub use self::ffi::{CString as OwnedCStr, CStr as BorrowedCStr};\n"
        "use self::ffi::CString as PrivateCString;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char mutation_source[] =
        "mod ffi { pub struct CStr; }\n"
        "#[doc(inline)]\n"
        "#[stable(feature = \"cstr\", since = \"1.64.0\")]\n"
        "pub use self::ffi::CStr;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const struct {
        const char *path;
        const unsigned char *source;
        size_t source_length;
        uint32_t rejected_attribute;
    } rejected[] = {
        { "doc-inline-call.rs",
            (const unsigned char *)
                "mod ffi { pub struct CStr; }\n"
                "#[doc(inline())]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod ffi { pub struct CStr; }\n"
                "#[doc(inline())]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u },
        { "doc-inline-malformed.rs",
            (const unsigned char *)
                "mod ffi { pub struct CStr; }\n"
                "#[doc(inline = \"yes\")]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod ffi { pub struct CStr; }\n"
                "#[doc(inline = \"yes\")]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u },
        { "doc-inline-duplicate.rs",
            (const unsigned char *)
                "mod ffi { pub struct CStr; }\n"
                "#[doc(inline)]\n"
                "#[doc(inline)]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod ffi { pub struct CStr; }\n"
                "#[doc(inline)]\n"
                "#[doc(inline)]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 1u },
        { "doc-inline-generated.rs",
            (const unsigned char *)
                "mod ffi { pub struct CStr; }\n"
                "#[cfg_attr(all(), doc(inline))]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod ffi { pub struct CStr; }\n"
                "#[cfg_attr(all(), doc(inline))]\n"
                "pub use self::ffi::CStr;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u }
    };
    static const unsigned char item_attribute_source[] =
        "#[doc(inline)]\n"
        "pub struct CStr;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture attributed;
    CaptureFixture plain;
    CaptureFixture mutation;
    CmHirDeclarationMetadata attributed_metadata;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationMetadata mutation_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirImport *import;
    CmInternId saved_metadata;
    CmSpan saved_span;
    uint32_t saved_source_attribute;
    CmByteBuf attributed_bytes;
    CmByteBuf plain_bytes;
    size_t index;
    fixture_init_source(&attributed, 0, "doc-inline.rs",
        attributed_source, sizeof(attributed_source) - 1u);
    fixture_init_source(&plain, 0, "doc-inline.rs", plain_source,
        sizeof(plain_source) - 1u);
    fixture_init_source(&mutation, 0, "doc-inline-mutation.rs",
        mutation_source, sizeof(mutation_source) - 1u);
    cm_hir_declaration_metadata_init(&attributed_metadata);
    cm_hir_declaration_metadata_init(&plain_metadata);
    cm_hir_declaration_metadata_init(&mutation_metadata);
    input = capture_input(&attributed);
    result = cm_hir_declaration_metadata_capture(&input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 4u
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "CStr") != NULL
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "OwnedCStr") != NULL
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "BorrowedCStr") != NULL
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "PrivateCString") == NULL);
    input = capture_input(&plain);
    result = cm_hir_declaration_metadata_capture(&input, &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u);
    cm_byte_buf_init(&attributed_bytes);
    cm_byte_buf_init(&plain_bytes);
    assert(cm_hir_declaration_metadata_encode(&attributed_metadata,
            &attributed_bytes) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && attributed_bytes.len == plain_bytes.len
        && memcmp(attributed_bytes.data, plain_bytes.data,
            attributed_bytes.len) == 0);
    cm_byte_buf_destroy(&plain_bytes);
    cm_byte_buf_destroy(&attributed_bytes);

    input = capture_input(&mutation);
    result = cm_hir_declaration_metadata_capture(&input,
        &mutation_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    saved_items = mutation_metadata.items;
    saved_namespace = mutation_metadata.namespace_entries;
    import = find_unique_attributed_import(&mutation);
    assert(import != NULL && import->attribute_count == 2u
        && import->attributes != NULL);

    saved_metadata = import->attributes[0].metadata;
    import->attributes[0].metadata = cm_hir_intern(&mutation.hir,
        "doc(inline())");
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 0u);
    import->attributes[0].metadata = saved_metadata;

    saved_span = import->attributes[0].span;
    import->attributes[0].span.end -= 1u;
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 0u);
    import->attributes[0].span = saved_span;

    saved_source_attribute = import->attributes[0].source_attribute;
    import->attributes[0].source_attribute += 1u;
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 0u);
    import->attributes[0].source_attribute = saved_source_attribute;

    import->attributes[0].expansion_depth = 1u;
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 0u);
    import->attributes[0].expansion_depth = 0u;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
            ++index) {
        CaptureFixture bad;
        fixture_init_source(&bad, 0, rejected[index].path,
            rejected[index].source, rejected[index].source_length);
        assert_reexport_projection_failure(&bad, &mutation_metadata,
            saved_items, saved_namespace, rejected[index].rejected_attribute);
        fixture_destroy(&bad);
    }
    {
        CaptureFixture item_attribute;
        fixture_init_source(&item_attribute, 0, "doc-inline-item.rs",
            item_attribute_source, sizeof(item_attribute_source) - 1u);
        input = capture_input(&item_attribute);
        result = cm_hir_declaration_metadata_capture(&input,
            &mutation_metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && mutation_metadata.items == saved_items
            && mutation_metadata.namespace_entries == saved_namespace);
        fixture_destroy(&item_attribute);
    }
    assert(cm_hir_declaration_metadata_validate(&mutation_metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&mutation_metadata);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    cm_hir_declaration_metadata_destroy(&attributed_metadata);
    fixture_destroy(&mutation);
    fixture_destroy(&plain);
    fixture_destroy(&attributed);
}

static void test_doc_hidden_reexport_projection_and_negatives(void)
{
    static const unsigned char attributed_source[] =
        "mod sip { pub struct SipHasher13; pub struct SipHasher24; }\n"
        "#[stable(feature = \"sip_hash\", since = \"1.0.0\")]\n"
        "#[doc(hidden)]\n"
        "pub use self::sip::SipHasher13;\n"
        "#[unstable(feature = \"hashmap_internals\", issue = \"none\")]\n"
        "#[allow(deprecated)]\n"
        "#[doc(hidden)]\n"
        "pub use self::sip::{SipHasher24 as HiddenHasher};\n"
        "#[doc(hidden)]\n"
        "use self::sip::SipHasher24 as PrivateHasher;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char plain_source[] =
        "mod sip { pub struct SipHasher13; pub struct SipHasher24; }\n"
        "pub use self::sip::SipHasher13;\n"
        "pub use self::sip::{SipHasher24 as HiddenHasher};\n"
        "use self::sip::SipHasher24 as PrivateHasher;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char mutation_source[] =
        "mod sip { pub struct SipHasher13; }\n"
        "#[stable(feature = \"sip_hash\", since = \"1.0.0\")]\n"
        "#[doc(hidden)]\n"
        "pub use self::sip::SipHasher13;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const struct {
        const char *path;
        const unsigned char *source;
        size_t source_length;
        uint32_t rejected_attribute;
    } rejected[] = {
        { "doc-hidden-call.rs",
            (const unsigned char *)
                "mod sip { pub struct SipHasher13; }\n"
                "#[doc(hidden())]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod sip { pub struct SipHasher13; }\n"
                "#[doc(hidden())]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u },
        { "doc-hidden-malformed.rs",
            (const unsigned char *)
                "mod sip { pub struct SipHasher13; }\n"
                "#[doc(hidden = \"yes\")]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod sip { pub struct SipHasher13; }\n"
                "#[doc(hidden = \"yes\")]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u },
        { "doc-hidden-duplicate.rs",
            (const unsigned char *)
                "mod sip { pub struct SipHasher13; }\n"
                "#[doc(hidden)]\n"
                "#[doc(hidden)]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod sip { pub struct SipHasher13; }\n"
                "#[doc(hidden)]\n"
                "#[doc(hidden)]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 1u },
        { "doc-hidden-generated.rs",
            (const unsigned char *)
                "mod sip { pub struct SipHasher13; }\n"
                "#[cfg_attr(all(), doc(hidden))]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod sip { pub struct SipHasher13; }\n"
                "#[cfg_attr(all(), doc(hidden))]\n"
                "pub use self::sip::SipHasher13;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u, 0u }
    };
    static const unsigned char item_attribute_source[] =
        "#[doc(hidden)]\n"
        "pub struct SipHasher13;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture attributed;
    CaptureFixture plain;
    CaptureFixture mutation;
    CmHirDeclarationMetadata attributed_metadata;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationMetadata mutation_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirImport *import;
    CmInternId saved_metadata;
    CmSpan saved_span;
    uint32_t saved_source_attribute;
    CmByteBuf attributed_bytes;
    CmByteBuf plain_bytes;
    size_t index;
    fixture_init_source(&attributed, 0, "doc-hidden.rs",
        attributed_source, sizeof(attributed_source) - 1u);
    fixture_init_source(&plain, 0, "doc-hidden.rs", plain_source,
        sizeof(plain_source) - 1u);
    fixture_init_source(&mutation, 0, "doc-hidden-mutation.rs",
        mutation_source, sizeof(mutation_source) - 1u);
    cm_hir_declaration_metadata_init(&attributed_metadata);
    cm_hir_declaration_metadata_init(&plain_metadata);
    cm_hir_declaration_metadata_init(&mutation_metadata);
    input = capture_input(&attributed);
    result = cm_hir_declaration_metadata_capture(&input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 5u
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "SipHasher13") != NULL
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "HiddenHasher") != NULL
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_TYPE, "PrivateHasher") == NULL);
    input = capture_input(&plain);
    result = cm_hir_declaration_metadata_capture(&input, &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u);
    cm_byte_buf_init(&attributed_bytes);
    cm_byte_buf_init(&plain_bytes);
    assert(cm_hir_declaration_metadata_encode(&attributed_metadata,
            &attributed_bytes) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && attributed_bytes.len == plain_bytes.len
        && memcmp(attributed_bytes.data, plain_bytes.data,
            attributed_bytes.len) == 0);
    cm_byte_buf_destroy(&plain_bytes);
    cm_byte_buf_destroy(&attributed_bytes);

    input = capture_input(&mutation);
    result = cm_hir_declaration_metadata_capture(&input,
        &mutation_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    saved_items = mutation_metadata.items;
    saved_namespace = mutation_metadata.namespace_entries;
    import = find_unique_attributed_import(&mutation);
    assert(import != NULL && import->attribute_count == 2u
        && import->attributes != NULL);

    saved_metadata = import->attributes[1].metadata;
    import->attributes[1].metadata = cm_hir_intern(&mutation.hir,
        "doc(hidden())");
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 1u);
    import->attributes[1].metadata = saved_metadata;

    saved_span = import->attributes[1].span;
    import->attributes[1].span.start += 1u;
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 1u);
    import->attributes[1].span = saved_span;

    saved_source_attribute = import->attributes[1].source_attribute;
    import->attributes[1].source_attribute += 1u;
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 1u);
    import->attributes[1].source_attribute = saved_source_attribute;

    import->attributes[1].expansion_depth = 1u;
    assert_reexport_projection_failure(&mutation, &mutation_metadata,
        saved_items, saved_namespace, 1u);
    import->attributes[1].expansion_depth = 0u;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
            ++index) {
        CaptureFixture bad;
        fixture_init_source(&bad, 0, rejected[index].path,
            rejected[index].source, rejected[index].source_length);
        assert_reexport_projection_failure(&bad, &mutation_metadata,
            saved_items, saved_namespace, rejected[index].rejected_attribute);
        fixture_destroy(&bad);
    }
    {
        CaptureFixture item_attribute;
        fixture_init_source(&item_attribute, 0, "doc-hidden-item.rs",
            item_attribute_source, sizeof(item_attribute_source) - 1u);
        input = capture_input(&item_attribute);
        result = cm_hir_declaration_metadata_capture(&input,
            &mutation_metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && mutation_metadata.items == saved_items
            && mutation_metadata.namespace_entries == saved_namespace);
        fixture_destroy(&item_attribute);
    }
    assert(cm_hir_declaration_metadata_validate(&mutation_metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&mutation_metadata);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    cm_hir_declaration_metadata_destroy(&attributed_metadata);
    fixture_destroy(&mutation);
    fixture_destroy(&plain);
    fixture_destroy(&attributed);
}

static void test_alias_and_reexport_attributes_fail_closed_atomically(void)
{
    static const unsigned char bad_alias_attribute_source[] =
        "pub struct Unit;\n"
        "#[repr(C)]\n"
        "pub type Alias = Unit;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char bad_reexport_attribute_source[] =
        "pub struct Unit;\n"
        "#[doc(notable_trait)]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char conflicting_reexport_stability_source[] =
        "pub struct Unit;\n"
        "#[stable(feature = \"unit\", since = \"1.0.0\")]\n"
        "#[unstable(feature = \"unit_next\", issue = \"none\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char bad_alias_target_source[] =
        "pub type Alias = u8;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture good;
    CaptureFixture bad_alias_attribute;
    CaptureFixture bad_reexport_attribute;
    CaptureFixture conflicting_reexport_stability;
    CaptureFixture bad_alias_target;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;

    fixture_init(&good, 0);
    fixture_init_source(&bad_alias_attribute, 0, "bad-alias-attr.rs",
        bad_alias_attribute_source, sizeof(bad_alias_attribute_source) - 1u);
    fixture_init_source(&bad_reexport_attribute, 0,
        "bad-reexport-attr.rs", bad_reexport_attribute_source,
        sizeof(bad_reexport_attribute_source) - 1u);
    fixture_init_source(&conflicting_reexport_stability, 0,
        "conflicting-reexport-stability.rs",
        conflicting_reexport_stability_source,
        sizeof(conflicting_reexport_stability_source) - 1u);
    fixture_init_source(&bad_alias_target, 0, "bad-alias-target.rs",
        bad_alias_target_source, sizeof(bad_alias_target_source) - 1u);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;

    input = capture_input(&bad_alias_attribute);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);

    assert_reexport_projection_failure(&bad_reexport_attribute, &metadata,
        saved_items, saved_namespace, 0u);

    assert_reexport_projection_failure(&conflicting_reexport_stability,
        &metadata, saved_items, saved_namespace, 1u);

    input = capture_input(&bad_alias_target);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&bad_alias_target);
    fixture_destroy(&conflicting_reexport_stability);
    fixture_destroy(&bad_reexport_attribute);
    fixture_destroy(&bad_alias_attribute);
    fixture_destroy(&good);
}

static void test_constructor_omission_authority_is_not_forgeable(void)
{
    static const unsigned char omitted_source[] =
        "#[non_exhaustive]\n"
        "pub struct Omitted;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char paired_source[] =
        "#[stable(feature = \"paired\", since = \"1.0.0\")]\n"
        "pub struct Paired;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture good;
    CaptureFixture omitted;
    CaptureFixture paired;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId item_id;
    CmHirItem *item;
    CmInternId saved_metadata;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;

    fixture_init(&good, 0);
    fixture_init_source(&omitted, 0, "omitted-constructor.rs",
        omitted_source, sizeof(omitted_source) - 1u);
    fixture_init_source(&paired, 0, "paired-constructor.rs", paired_source,
        sizeof(paired_source) - 1u);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;

    item = (CmHirItem *)find_item(&omitted, "Omitted", &item_id);
    assert(item != NULL && item->attribute_count == 1u
        && item->attributes != NULL);
    item->attribute_count = 0u;
    input = capture_input(&omitted);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == item_id
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    item->attribute_count = 1u;
    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&omitted.hir,
        "stable(feature = \"forged\", since = \"1.0.0\")");
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    item->attributes[0].metadata = saved_metadata;

    item = (CmHirItem *)find_item(&paired, "Paired", &item_id);
    assert(item != NULL && item->attribute_count == 1u);
    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&paired.hir,
        "non_exhaustive");
    input = capture_input(&paired);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    item->attributes[0].metadata = saved_metadata;
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&paired);
    fixture_destroy(&omitted);
    fixture_destroy(&good);
}

static void test_many_private_bindings_do_not_consume_public_cap(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf source;
    char declaration[64];
    size_t index;
    int length;

    cm_byte_buf_init(&source);
    cm_byte_buf_append(&source, fixture_source, sizeof(fixture_source) - 1u);
    for (index = 0u; index < 2048u; ++index) {
        length = snprintf(declaration, sizeof(declaration),
            "fn private_%lu() {}\n", (unsigned long)index);
        assert(length > 0 && (size_t)length < sizeof(declaration));
        cm_byte_buf_append(&source, declaration, (size_t)length);
    }
    fixture_init_source(&fixture, 0, "many-private.rs", source.data,
        source.len);
    cm_byte_buf_destroy(&source);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_NONE
        && result.failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE
        && result.namespace_count == 11u
        && result.item_count == 3u
        && result.projected_semantic_attribute_count == 11u);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void assert_const_descriptor(const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *direct;
    const CmHirDeclarationNamespaceEntry *renamed;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u
        && metadata->trait_count == 1u && metadata->item_count == 0u
        && metadata->value_count == 4u && metadata->generic_count == 2u
        && metadata->predicate_count == 1u && metadata->type_count == 5u
        && metadata->namespace_count == 6u);
    assert(metadata->types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && metadata->types[1].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[1].primitive == CM_HIR_DECL_PRIMITIVE_CHAR
        && metadata->types[2].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[2].primitive == CM_HIR_DECL_PRIMITIVE_U8
        && metadata->types[3].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[3].primitive == CM_HIR_DECL_PRIMITIVE_USIZE
        && metadata->types[4].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[4].generic_local == 2u);
    assert(metadata->values[0].kind == CM_HIR_DECL_VALUE_CONST
        && declaration_string_is(metadata->values[0].name, "MAX")
        && metadata->values[0].source_ordinal == 0u
        && metadata->values[0].generic_start == 0u
        && metadata->values[0].generic_count == 0u
        && metadata->values[0].predicate_start == 0u
        && metadata->values[0].predicate_count == 0u
        && metadata->values[0].parameter_count == 0u
        && metadata->values[0].parameter_types == NULL
        && metadata->values[0].return_type == 0u
        && metadata->values[0].declared_type == 2u
        && metadata->values[0].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[0].has_body == 1u);
    assert(metadata->values[1].kind == CM_HIR_DECL_VALUE_CONST
        && declaration_string_is(metadata->values[1].name, "NEXT")
        && metadata->values[1].source_ordinal == 1u
        && metadata->values[1].declared_type == 4u
        && metadata->values[1].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[1].has_body == 1u
        && metadata->values[2].kind == CM_HIR_DECL_VALUE_CONST
        && declaration_string_is(metadata->values[2].name, "OLD")
        && metadata->values[2].source_ordinal == 2u
        && metadata->values[2].declared_type == 2u
        && metadata->values[2].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[2].has_body == 1u);
    assert(metadata->values[3].kind == CM_HIR_DECL_VALUE_FUNCTION
        && declaration_string_is(metadata->values[3].name, "needs")
        && metadata->values[3].source_ordinal == 5u
        && metadata->values[3].generic_count == 1u
        && metadata->values[3].predicate_count == 1u
        && metadata->values[3].declared_type == 0u
        && metadata->values[3].mutability == 0u
        && metadata->values[3].return_type == 1u
        && metadata->values[3].has_body == 1u);
    assert(metadata->generics[1].owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && metadata->generics[1].owner_local == 4u
        && metadata->predicates[0].owner_value == 4u
        && metadata->predicates[0].subject_type == 5u
        && metadata->predicates[0].argument_types[0] == 3u);
    direct = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "MAX");
    renamed = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "RENAMED");
    assert(direct != NULL && renamed != NULL
        && direct->target_kind == CM_HIR_DECL_TARGET_VALUE
        && renamed->target_kind == CM_HIR_DECL_TARGET_VALUE
        && direct->target_local == 1u && renamed->target_local == 1u
        && direct->export_ordinal == 0u && renamed->export_ordinal == 3u);
}

static void test_char_const_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    const_fixture_init(&first, 0);
    const_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.value_count == 4u && result.namespace_count == 6u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION
        && result.projected_semantic_attribute_count == 4u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 4u);
    assert_const_descriptor(&first_metadata);
    assert_const_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_char_const_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId item_id;
    CmHirItem *item;
    CmHirBody *body;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmInternId saved_metadata;
    CmHirAttribute *saved_attributes;
    uint32_t saved_source_expression;
    CmSourceId saved_source;
    CmSpan saved_body_span;
    CmHirType mismatched;
    CmHirTypeId mismatched_id;
    CmHirTypeId saved_type;
    const CmHirType *saved_type_value;
    CmHirBodyId saved_body_id;
    const_fixture_init(&fixture, 1);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)find_item(&fixture, "MAX", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_CONST
        && item->attribute_count == 1u
        && item->data.value_item.body != CM_HIR_BODY_NONE);
    body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        item->data.value_item.body);
    assert(body != NULL);
    saved_body_id = item->data.value_item.body;

#define ASSERT_CONST_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_source_expression = body->source_expression_id;
    body->source_expression_id = UINT32_MAX;
    ASSERT_CONST_ATOMIC_FAILURE();
    body->source_expression_id = saved_source_expression;

    saved_source = body->source;
    saved_body_span = body->span;
    body->source = 1u;
    body->span.source = 1u;
    ASSERT_CONST_ATOMIC_FAILURE();
    body->source = saved_source;
    body->span = saved_body_span;

    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&fixture.hir,
        "stable(feature = \"forged\", since = \"1.0.0\")");
    ASSERT_CONST_ATOMIC_FAILURE();
    item->attributes[0].metadata = saved_metadata;

    item->attributes[0].expansion_depth = 1u;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->attributes[0].expansion_depth = 0u;

    saved_attributes = item->attributes;
    item->attributes = NULL;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->attributes = saved_attributes;

    item->predicate_count = 1u;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->predicate_count = 0u;

    saved_type = item->data.value_item.type;
    saved_type_value = cm_hir_get_type(&fixture.hir, saved_type);
    assert(saved_type_value != NULL
        && saved_type_value->kind == CM_HIR_TYPE_CHAR_KIND);
    memset(&mismatched, 0, sizeof(mismatched));
    mismatched.kind = CM_HIR_TYPE_INTEGER_KIND;
    mismatched.data.integer_type.kind = CM_HIR_INT_USIZE;
    mismatched.span = saved_type_value->span;
    assert(cm_hir_add_type(&fixture.hir, &mismatched, &mismatched_id)
        == CM_HIR_OK);
    item->data.value_item.type = mismatched_id;
    body->expected_type = mismatched_id;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->data.value_item.type = saved_type;
    body->expected_type = saved_type;

    item->data.value_item.mutability = CM_HIR_MUTABLE;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->data.value_item.mutability = CM_HIR_IMMUTABLE;

    item->data.value_item.body = CM_HIR_BODY_NONE;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->data.value_item.body = saved_body_id;

    assert_const_descriptor(&metadata);
#undef ASSERT_CONST_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_char_const_attributes_fail_closed_atomically(void)
{
    static const unsigned char stable_unstable[] =
        "#[stable(feature = \"one\", since = \"1.0.0\")]\n"
        "#[unstable(feature = \"two\", issue = \"none\")]\n"
        "pub const C: char = char::MAX;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char unknown[] =
        "#[unknown_projection]\n"
        "pub const C: char = char::MAX;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    const unsigned char *sources[2];
    size_t lengths[2];
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    size_t index;
    sources[0] = stable_unstable;
    sources[1] = unknown;
    lengths[0] = sizeof(stable_unstable) - 1u;
    lengths[1] = sizeof(unknown) - 1u;
    const_fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_namespace = metadata.namespace_entries;
    for (index = 0u; index < 2u; ++index) {
        CaptureFixture bad;
        fixture_init_source(&bad, 0, "bad-const-attributes.rs",
            sources[index], lengths[index]);
        input = capture_input(&bad);
        result = cm_hir_declaration_metadata_capture(&input, &metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && metadata.values == saved_values
            && metadata.namespace_entries == saved_namespace);
        fixture_destroy(&bad);
    }
    assert_const_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void assert_aggregate_descriptor(
    const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationItem *manual;
    const CmHirDeclarationItem *maybe;
    const CmHirDeclarationItem *assume;
    const CmHirDeclarationNamespaceEntry *manual_direct;
    const CmHirDeclarationNamespaceEntry *manual_export;
    const CmHirDeclarationNamespaceEntry *maybe_direct;
    const CmHirDeclarationNamespaceEntry *maybe_export;
    const CmHirDeclarationNamespaceEntry *assume_direct;
    const CmHirDeclarationNamespaceEntry *assume_export;
    assert(metadata->module_count == 4u && metadata->item_count == 3u
        && metadata->generic_count == 4u && metadata->type_count == 7u
        && metadata->namespace_count == 8u);
    manual = &metadata->items[0];
    maybe = &metadata->items[1];
    assume = &metadata->items[2];
    assert(manual->kind == CM_HIR_DECL_ITEM_STRUCT
        && manual->owner_module == 2u
        && declaration_string_is(manual->name, "Manual")
        && manual->generic_start == 2u && manual->generic_count == 1u
        && manual->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
        && manual->aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
        && manual->aggregate_flags
            == (CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
                | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT)
        && declaration_string_is(manual->lang_item, "manually_drop")
        && manual->field_count == 1u
        && declaration_string_is(manual->fields[0].name, "value")
        && manual->fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && manual->fields[0].source_ordinal == 0u
        && manual->fields[0].type_local == 4u);
    assert(maybe->kind == CM_HIR_DECL_ITEM_UNION
        && maybe->owner_module == 3u
        && declaration_string_is(maybe->name, "Maybe")
        && maybe->generic_start == 3u && maybe->generic_count == 1u
        && maybe->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
        && maybe->aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
        && maybe->aggregate_flags
            == (CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
                | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT)
        && declaration_string_is(maybe->lang_item, "maybe_uninit")
        && maybe->field_count == 2u
        && declaration_string_is(maybe->fields[0].name, "uninit")
        && declaration_string_is(maybe->fields[1].name, "value")
        && maybe->fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && maybe->fields[1].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && maybe->fields[0].source_ordinal == 0u
        && maybe->fields[1].source_ordinal == 1u
        && maybe->fields[0].type_local == 1u
        && maybe->fields[1].type_local == 7u);
    assert(assume->kind == CM_HIR_DECL_ITEM_STRUCT
        && assume->owner_module == 4u
        && declaration_string_is(assume->name, "Assumptions")
        && assume->generic_start == 0u && assume->generic_count == 0u
        && assume->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
        && assume->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST
        && assume->aggregate_flags == CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        && declaration_string_is(assume->lang_item, "transmute_opts")
        && assume->field_count == 4u);
    assert(metadata->types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && metadata->types[1].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[1].primitive == CM_HIR_DECL_PRIMITIVE_BOOL
        && metadata->types[2].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[2].primitive == CM_HIR_DECL_PRIMITIVE_U8
        && metadata->types[3].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[3].generic_local == 2u
        && metadata->types[4].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[4].generic_local == 3u
        && metadata->types[5].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[5].generic_local == 4u
        && metadata->types[6].kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
        && metadata->types[6].item_local == 1u
        && metadata->types[6].argument_count == 1u
        && metadata->types[6].argument_types[0] == 5u);
    manual_direct = find_namespace_entry(metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Manual");
    manual_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Manual");
    maybe_direct = find_namespace_entry(metadata, 3u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Maybe");
    maybe_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Maybe");
    assume_direct = find_namespace_entry(metadata, 4u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Assumptions");
    assume_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Assumptions");
    assert(manual_direct != NULL && manual_export != NULL
        && maybe_direct != NULL && maybe_export != NULL
        && assume_direct != NULL && assume_export != NULL
        && manual_direct->target_kind == CM_HIR_DECL_TARGET_ITEM
        && manual_direct->target_local == 1u
        && manual_export->target_local == 1u
        && maybe_direct->target_local == 2u
        && maybe_export->target_local == 2u
        && assume_direct->target_local == 3u
        && assume_export->target_local == 3u);
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
}

static void test_named_aggregate_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    aggregate_fixture_init(&first, 0);
    aggregate_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "aggregate capture failed: %s stage=%s reason=%s "
            "item=%u type=%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_type);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 3u && result.trait_count == 1u
        && result.value_count == 1u && result.namespace_count == 8u
        && result.projected_semantic_attribute_count == 9u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 9u);
    assert_aggregate_descriptor(&first_metadata);
    assert_aggregate_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_layout_private_dependency_closure_and_determinism(void)
{
    static const struct {
        const char *repr;
        const char *high;
        uint8_t expected_repr;
        uint64_t expected_high;
    } wide_cases[] = {
        { "u16", "65535", CM_HIR_DECL_ENUM_REPR_U16, UINT64_C(65535) },
        { "u32", "1 << 31", CM_HIR_DECL_ENUM_REPR_U32,
            UINT64_C(2147483648) }
    };
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationItem *layout;
    const CmHirDeclarationItem *alignment;
    const CmHirDeclarationItem *alignment_enum;
    uint32_t layout_local = 0u;
    uint32_t alignment_local = 0u;
    uint32_t enum_local = 0u;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;
    size_t index;
    layout_dependency_fixture_init(&first, 0);
    layout_dependency_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "layout capture failed status=%s stage=%s reason=%s "
            "item=%u type=%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_type);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 3u
        && result.projected_semantic_attribute_count == 5u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 5u);
    layout = find_declaration_item(&metadata, "Layout", &layout_local);
    alignment = find_declaration_item(&metadata, "Alignment",
        &alignment_local);
    alignment_enum = find_declaration_item(&metadata, "AlignmentEnum",
        &enum_local);
    assert(find_declaration_item(&metadata, "Orphan", NULL) == NULL);
    assert(layout != NULL && alignment != NULL && alignment_enum != NULL
        && layout->kind == CM_HIR_DECL_ITEM_STRUCT
        && layout->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && layout->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
        && layout->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST
        && layout->aggregate_flags == CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        && declaration_string_is(layout->lang_item, "alloc_layout")
        && layout->field_count == 2u
        && declaration_string_is(layout->fields[0].name, "size")
        && declaration_string_is(layout->fields[1].name, "align")
        && layout->fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && layout->fields[1].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && alignment->kind == CM_HIR_DECL_ITEM_STRUCT
        && alignment->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && alignment->aggregate_form == CM_HIR_DECL_AGGREGATE_TUPLE
        && alignment->aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
        && alignment->aggregate_flags == 0u
        && alignment->field_count == 1u
        && alignment->fields[0].name.data == NULL
        && alignment->fields[0].name.length == 0u
        && alignment->fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && alignment_enum->kind == CM_HIR_DECL_ITEM_ENUM
        && alignment_enum->visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && alignment_enum->enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_U64
        && alignment_enum->variant_count == 64u
        && alignment_enum->variants[0].discriminant_low == UINT64_C(1)
        && alignment_enum->variants[63].discriminant_low
            == (UINT64_C(1) << 63)
        && alignment_enum->variants[63].discriminant_high == 0u);
    assert(layout_local != 0u && alignment_local != 0u && enum_local != 0u);
    for (index = 0u; index < metadata.namespace_count; ++index)
        assert(metadata.namespace_entries[index].target_kind
                != CM_HIR_DECL_TARGET_ITEM
            || metadata.namespace_entries[index].target_local != enum_local);
    assert(find_namespace_entry(&metadata, layout->owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE, "Layout") != NULL
        && find_namespace_entry(&metadata, alignment->owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE, "Alignment") != NULL
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);

    for (index = 0u; index < sizeof(wide_cases) / sizeof(wide_cases[0]);
            ++index) {
        CaptureFixture wide;
        CmHirDeclarationMetadata wide_metadata;
        const CmHirDeclarationItem *wide_item;
        uint32_t wide_local = 0u;
        wide_enum_fixture_init(&wide, wide_cases[index].repr,
            wide_cases[index].high);
        cm_hir_declaration_metadata_init(&wide_metadata);
        input = capture_input(&wide);
        result = cm_hir_declaration_metadata_capture(&input, &wide_metadata);
        wide_item = find_declaration_item(&wide_metadata, "Wide",
            &wide_local);
        assert(result.status == CM_HIR_DECL_CAPTURE_OK
            && wide_item != NULL && wide_local != 0u
            && wide_item->enum_repr_primitive
                == wide_cases[index].expected_repr
            && wide_item->variants[1].discriminant_low
                == wide_cases[index].expected_high
            && cm_hir_declaration_metadata_validate(&wide_metadata)
                == CM_HIR_DECL_METADATA_OK);
        cm_hir_declaration_metadata_destroy(&wide_metadata);
        fixture_destroy(&wide);
    }
}

static void test_layout_private_dependency_hostiles_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId layout_id;
    CmHirItemId alignment_id;
    CmHirItemId enum_id;
    CmHirItem *layout;
    CmHirItem *alignment;
    CmHirItem *alignment_enum;
    CmHirType *alignment_type;
    CmHirDefId saved_definition;
    CmHirVisibility saved_visibility;
    CmInternId saved_metadata;
    CmSpan saved_span;
    uint64_t saved_discriminant;
    layout_dependency_fixture_init(&fixture, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    layout = (CmHirItem *)find_item(&fixture, "Layout", &layout_id);
    alignment = (CmHirItem *)find_item(&fixture, "Alignment",
        &alignment_id);
    alignment_enum = (CmHirItem *)find_item(&fixture, "AlignmentEnum",
        &enum_id);
    assert(layout != NULL && alignment != NULL && alignment_enum != NULL
        && layout->data.aggregate_item.field_count == 2u
        && alignment->data.aggregate_item.field_count == 1u
        && alignment_enum->data.enum_item.variant_count == 64u);
#define ASSERT_ATOMIC_LAYOUT_FAILURE() do { \
    input = capture_input(&fixture); \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.namespace_entries == saved_namespace); \
} while (0)
    alignment_type = (CmHirType *)cm_hir_get_type(&fixture.hir,
        layout->data.aggregate_item.fields[1].type);
    assert(alignment_type != NULL && alignment_type->kind
        == CM_HIR_TYPE_ADT_KIND);
    saved_definition = alignment_type->data.named_type.definition;
    alignment_type->data.named_type.definition = layout->definition;
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment_type->data.named_type.definition = saved_definition;

    saved_visibility = alignment_enum->visibility;
    alignment_enum->visibility.kind = CM_HIR_VIS_PUBLIC;
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment_enum->visibility = saved_visibility;

    saved_metadata = alignment_enum->attributes[1].metadata;
    alignment_enum->attributes[1].metadata = cm_hir_intern(&fixture.hir,
        "repr(u32)");
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment_enum->attributes[1].metadata = saved_metadata;

    saved_span = alignment->attributes[2].span;
    alignment->attributes[2].span.start += 1u;
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment->attributes[2].span = saved_span;

    saved_visibility = alignment->data.aggregate_item.fields[0].visibility;
    alignment->data.aggregate_item.fields[0].visibility.kind =
        CM_HIR_VIS_PUBLIC;
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment->data.aggregate_item.fields[0].visibility = saved_visibility;

    saved_discriminant = alignment_enum->data.enum_item.variants[63]
        .discriminant.data.value.low_bits;
    alignment_enum->data.enum_item.variants[63].discriminant.data.value
        .low_bits = UINT64_C(1) << 62;
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment_enum->data.enum_item.variants[63].discriminant.data.value
        .low_bits = saved_discriminant;

    saved_definition = alignment_enum->definition;
    alignment_enum->definition = layout->definition;
    ASSERT_ATOMIC_LAYOUT_FAILURE();
    alignment_enum->definition = saved_definition;
#undef ASSERT_ATOMIC_LAYOUT_FAILURE
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_named_aggregate_hostile_mutations_are_atomic(void)
{
    static const unsigned char drop_guard_source[] =
        "pub trait Gate<T: ?Sized> {}\n"
        "#[stable(feature = \"guard\", since = \"1.0.0\")]\n"
        "#[lang = \"guard\"]\n"
        "#[derive(Copy)]\n"
        "#[repr(transparent)]\n"
        "#[rustc_pub_transparent]\n"
        "pub struct Guard<T: ?Sized> where T: Gate<u8> { value: T }\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture good;
    CaptureFixture drop_guard;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItem *manual;
    CmHirItem *maybe;
    CmHirItem *assume;
    CmHirItemId item_id;
    CmHirGenericParam *generic;
    CmHirVisibilityKind saved_visibility;
    CmHirTypeId saved_type;
    CmHirGenericArg *application_arguments;
    CmHirTypeId saved_argument_type;
    CmInternId saved_metadata;
    CmInternId saved_field_name;
    CmSpan saved_span;
    uint32_t saved_attribute_count;
    aggregate_fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    manual = (CmHirItem *)find_item(&good, "Manual", &item_id);
    assert(manual != NULL && manual->generic_parameter_count == 1u);
    generic = (CmHirGenericParam *)cm_hir_get_generic_param(&good.hir,
        manual->generic_parameter_start);
    assert(generic != NULL && generic->is_relaxed_sized);

#define ASSERT_AGGREGATE_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    generic->is_relaxed_sized = 0;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    generic->is_relaxed_sized = 1;

    saved_visibility = manual->data.aggregate_item.fields[0].visibility.kind;
    manual->data.aggregate_item.fields[0].visibility.kind = CM_HIR_VIS_PUBLIC;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    manual->data.aggregate_item.fields[0].visibility.kind = saved_visibility;

    maybe = (CmHirItem *)find_item(&good, "Maybe", &item_id);
    assert(maybe != NULL && maybe->kind == CM_HIR_ITEM_UNION
        && maybe->data.aggregate_item.field_count == 2u);
    saved_type = maybe->data.aggregate_item.fields[1].type;
    maybe->data.aggregate_item.fields[1].type =
        maybe->data.aggregate_item.fields[0].type;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    maybe->data.aggregate_item.fields[1].type = saved_type;

    application_arguments = ((CmHirType *)cm_hir_get_type(&good.hir,
        saved_type))->data.named_type.arguments;
    assert(application_arguments != NULL
        && application_arguments[0].kind == CM_HIR_GENERIC_ARG_TYPE);
    saved_argument_type = application_arguments[0].data.type;
    application_arguments[0].data.type =
        manual->data.aggregate_item.fields[0].type;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    application_arguments[0].data.type = saved_argument_type;

    saved_span = maybe->data.aggregate_item.fields[1].span;
    maybe->data.aggregate_item.fields[1].span.start += 1u;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    maybe->data.aggregate_item.fields[1].span = saved_span;

    saved_field_name = maybe->data.aggregate_item.fields[1].name;
    maybe->data.aggregate_item.fields[1].name = cm_hir_intern(&good.hir,
        "forged_field");
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    maybe->data.aggregate_item.fields[1].name = saved_field_name;

    saved_metadata = maybe->attributes[1].metadata;
    maybe->attributes[1].metadata = cm_hir_intern(&good.hir,
        "lang = \"forged\"");
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    maybe->attributes[1].metadata = saved_metadata;

    maybe->attributes[2].expansion_depth = 1u;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    maybe->attributes[2].expansion_depth = 0u;

    saved_attribute_count = maybe->attribute_count;
    maybe->attribute_count -= 1u;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    maybe->attribute_count = saved_attribute_count;

    assume = (CmHirItem *)find_item(&good, "Assumptions", &item_id);
    assert(assume != NULL && assume->data.aggregate_item.field_count == 4u);
    saved_visibility = assume->data.aggregate_item.fields[0].visibility.kind;
    assume->data.aggregate_item.fields[0].visibility.kind = CM_HIR_VIS_PRIVATE;
    ASSERT_AGGREGATE_ATOMIC_FAILURE();
    assume->data.aggregate_item.fields[0].visibility.kind = saved_visibility;

    fixture_init_source(&drop_guard, 0, "drop-guard.rs", drop_guard_source,
        sizeof(drop_guard_source) - 1u);
    input = capture_input(&drop_guard);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&drop_guard);
    assert_aggregate_descriptor(&metadata);
#undef ASSERT_AGGREGATE_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void test_rust_tuple_struct_capture_and_atomic_boundaries(void)
{
    static const unsigned char generic_tuple_source[] =
        "#[stable(feature = \"generic_tuple\", since = \"1.0.0\")]\n"
        "#[derive(Copy, Clone)]\n"
        "pub struct GenericTuple<T>(T);\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture generic;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationItem *wire;
    const CmHirDeclarationType *field_type;
    CmHirItem *item;
    CmHirItemId item_id;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirAggregateForm saved_form;
    CmHirVisibility saved_visibility;
    CmInternId saved_name;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;

    rust_tuple_fixture_init(&first, 0);
    rust_tuple_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    wire = find_declaration_item(&metadata, "TupleError", NULL);
    assert(wire != NULL && wire->kind == CM_HIR_DECL_ITEM_STRUCT
        && wire->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && wire->aggregate_form == CM_HIR_DECL_AGGREGATE_TUPLE
        && wire->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST
        && wire->aggregate_flags == 0u && wire->generic_count == 0u
        && wire->field_count == 1u && wire->fields != NULL
        && wire->fields[0].name.data == NULL
        && wire->fields[0].name.length == 0u
        && wire->fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PRIVATE
        && wire->fields[0].type_local != 0u
        && (size_t)wire->fields[0].type_local <= metadata.type_count);
    field_type = &metadata.types[wire->fields[0].type_local - 1u];
    assert(field_type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && field_type->primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && find_namespace_entry(&metadata, wire->owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE, "TupleError") != NULL
        && find_namespace_entry(&metadata, wire->owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE, "TupleErrorAlias") != NULL
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)(void *)find_item(&first, "TupleError", &item_id);
    assert(item != NULL && item_id != CM_HIR_ITEM_NONE
        && item->data.aggregate_item.field_count == 1u);
#define ASSERT_RUST_TUPLE_ATOMIC_FAILURE() do { \
    input = capture_input(&first); \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.namespace_entries == saved_namespace); \
} while (0)
    saved_visibility = item->data.aggregate_item.fields[0].visibility;
    item->data.aggregate_item.fields[0].visibility.kind = CM_HIR_VIS_PUBLIC;
    ASSERT_RUST_TUPLE_ATOMIC_FAILURE();
    item->data.aggregate_item.fields[0].visibility = saved_visibility;
    saved_name = item->data.aggregate_item.fields[0].name;
    item->data.aggregate_item.fields[0].name = cm_hir_intern(&first.hir,
        "forged");
    ASSERT_RUST_TUPLE_ATOMIC_FAILURE();
    item->data.aggregate_item.fields[0].name = saved_name;
    saved_form = item->data.aggregate_item.form;
    item->data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    ASSERT_RUST_TUPLE_ATOMIC_FAILURE();
    item->data.aggregate_item.form = saved_form;

    fixture_init_source(&generic, 0, "generic-rust-tuple.rs",
        generic_tuple_source, sizeof(generic_tuple_source) - 1u);
    input = capture_input(&generic);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&generic);
#undef ASSERT_RUST_TUPLE_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_into_iter_private_closure_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    const CmHirDeclarationItem *into_iter;
    const CmHirDeclarationItem *poly;
    const CmHirDeclarationItem *range;
    const CmHirDeclarationType *poly_application;
    const CmHirDeclarationType *array;
    const CmHirDeclarationTrait *partial = NULL;
    uint32_t into_local = 0u;
    uint32_t poly_local = 0u;
    uint32_t partial_local = 0u;
    size_t index;
    into_iter_fixture_init(&first, 0);
    into_iter_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 11u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 11u);
    assert(cm_hir_declaration_metadata_validate(&first_metadata)
        == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_validate(&noisy_metadata)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    into_iter = find_declaration_item(&first_metadata, "IntoIter",
        &into_local);
    poly = find_declaration_item(&first_metadata, "PolymorphicIter",
        &poly_local);
    range = find_declaration_item(&first_metadata, "IndexRange", NULL);
    assert(into_iter != NULL && poly != NULL && range != NULL
        && into_iter->kind == CM_HIR_DECL_ITEM_STRUCT
        && into_iter->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && into_iter->generic_count == 2u
        && (into_iter->aggregate_flags
            & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) != 0u
        && (into_iter->aggregate_flags
            & CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR) != 0u
        && declaration_string_is(into_iter->diagnostic_item,
            "ArrayIntoIter")
        && into_iter->field_count == 1u
        && poly->visibility.kind == CM_HIR_DECL_VISIBILITY_RESTRICTED
        && poly->visibility.restriction_module != 0u
        && poly->generic_count == 1u && poly->predicate_count == 1u
        && range->visibility.kind == CM_HIR_DECL_VISIBILITY_CRATE
        && find_namespace_entry(&first_metadata, poly->owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE, "PolymorphicIter") == NULL
        && find_namespace_entry(&first_metadata, range->owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE, "IndexRange") == NULL);
    assert(first_metadata.generics[into_iter->generic_start].kind
            == CM_HIR_DECL_GENERIC_CONST
        && first_metadata.generics[into_iter->generic_start].declared_type
            != 0u);
    poly_application = &first_metadata.types[
        into_iter->fields[0].type_local - 1u];
    assert(poly_application->kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
        && poly_application->item_local == poly_local
        && poly_application->argument_count == 1u);
    array = &first_metadata.types[
        poly_application->argument_types[0] - 1u];
    assert(array->kind == CM_HIR_DECL_TYPE_ARRAY
        && array->array_length_kind
            == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
        && array->array_length_generic_local
            == into_iter->generic_start + 1u);
    for (index = 0u; index < first_metadata.trait_count; ++index)
        if (declaration_string_is(first_metadata.traits[index].name,
                "PartialDrop")) {
            partial = &first_metadata.traits[index];
            partial_local = (uint32_t)(index + 1u);
        }
    assert(partial != NULL && partial_local != 0u
        && partial->visibility.kind == CM_HIR_DECL_VISIBILITY_PRIVATE
        && partial->associated_count == 1u
        && first_metadata.associated_items[partial->associated_start - 1u]
            .receiver == CM_HIR_DECL_RECEIVER_REF_MUTABLE
        && first_metadata.associated_items[partial->associated_start - 1u]
            .safety == CM_HIR_DECL_SAFETY_UNSAFE
        && first_metadata.predicates[poly->predicate_start - 1u].owner_kind
            == CM_HIR_DECL_PREDICATE_OWNER_ITEM
        && first_metadata.predicates[poly->predicate_start - 1u].owner_item
            == poly_local
        && first_metadata.predicates[poly->predicate_start - 1u].trait_local
            == partial_local);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_into_iter_hostile_shapes_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationTrait *saved_traits;
    CmHirDeclarationAssociatedItem *saved_associated;
    CmHirDeclarationType *saved_types;
    CmHirItemId ignored_id;
    CmHirItem *into_iter;
    CmHirItem *polymorphic;
    CmHirItem *partial_drop;
    CmHirItem *partial_method;
    CmHirItem *alias;
    CmHirItem *gate;
    CmHirGenericParam *const_generic;
    CmHirType *field_application;
    CmHirType *field_array;
    CmHirGenericParamKind saved_generic_kind;
    CmHirTypeId saved_declared_type;
    CmHirGenericParamId saved_length_parameter;
    CmHirVisibilityKind saved_visibility;
    CmHirDefId saved_definition;
    CmInternId saved_metadata;
    CmHirReceiverKind saved_receiver;
    CmHirSafety saved_safety;
    CmHirTypeId saved_parameter_type;
    CmHirTypeId saved_alias_target;
    uint32_t saved_predicate_span_start;

    into_iter_fixture_init(&fixture, 0);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_traits = metadata.traits;
    saved_associated = metadata.associated_items;
    saved_types = metadata.types;
    into_iter = (CmHirItem *)(void *)find_item(&fixture, "IntoIter",
        &ignored_id);
    polymorphic = (CmHirItem *)(void *)find_item(&fixture,
        "PolymorphicIter", &ignored_id);
    partial_drop = (CmHirItem *)(void *)find_item(&fixture, "PartialDrop",
        &ignored_id);
    partial_method = (CmHirItem *)(void *)find_item(&fixture,
        "partial_drop", &ignored_id);
    alias = (CmHirItem *)(void *)find_item(&fixture, "InnerSized",
        &ignored_id);
    gate = (CmHirItem *)(void *)find_item(&fixture, "Gate", &ignored_id);
    assert(into_iter != NULL && polymorphic != NULL && partial_drop != NULL
        && partial_method != NULL && alias != NULL && gate != NULL
        && into_iter->generic_parameter_count == 2u
        && into_iter->data.aggregate_item.field_count == 1u
        && polymorphic->predicate_count == 1u
        && partial_drop->attribute_count == 1u
        && partial_method->data.function_item.signature.parameter_count == 2u);
    const_generic = (CmHirGenericParam *)(void *)cm_hir_get_generic_param(
        &fixture.hir, into_iter->generic_parameter_start + 1u);
    field_application = (CmHirType *)(void *)cm_hir_get_type(&fixture.hir,
        into_iter->data.aggregate_item.fields[0].type);
    field_array = field_application == NULL
            || field_application->kind != CM_HIR_TYPE_ADT_KIND
            || field_application->data.named_type.argument_count != 1u
        ? NULL : (CmHirType *)(void *)cm_hir_get_type(&fixture.hir,
            field_application->data.named_type.arguments[0].data.type);
    assert(const_generic != NULL && field_array != NULL
        && const_generic->kind == CM_HIR_GENERIC_CONST
        && field_array->kind == CM_HIR_TYPE_ARRAY_KIND);

#define ASSERT_INTO_ITER_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.traits == saved_traits \
        && metadata.associated_items == saved_associated \
        && metadata.types == saved_types); \
} while (0)

    saved_generic_kind = const_generic->kind;
    const_generic->kind = CM_HIR_GENERIC_TYPE;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    const_generic->kind = saved_generic_kind;

    const_generic->has_default = 1;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    const_generic->has_default = 0;

    saved_declared_type = const_generic->declared_type;
    const_generic->declared_type =
        partial_method->data.function_item.signature.return_type;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    const_generic->declared_type = saved_declared_type;

    saved_length_parameter =
        field_array->data.array_type.length.data.parameter;
    field_array->data.array_type.length.data.parameter =
        into_iter->generic_parameter_start;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    field_array->data.array_type.length.data.parameter =
        saved_length_parameter;

    saved_alias_target = alias->data.type_alias_item.target;
    alias->data.type_alias_item.target = const_generic->declared_type;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    alias->data.type_alias_item.target = saved_alias_target;

    saved_visibility = polymorphic->visibility.kind;
    polymorphic->visibility.kind = CM_HIR_VIS_PRIVATE;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    polymorphic->visibility.kind = saved_visibility;

    saved_definition = polymorphic->predicates[0].trait_type.definition;
    polymorphic->predicates[0].trait_type.definition = gate->definition;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    polymorphic->predicates[0].trait_type.definition = saved_definition;

    saved_predicate_span_start = polymorphic->predicates[0].span.start;
    polymorphic->predicates[0].span.start += 1u;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    polymorphic->predicates[0].span.start = saved_predicate_span_start;

    saved_metadata = partial_drop->attributes[0].metadata;
    partial_drop->attributes[0].metadata = cm_hir_intern(&fixture.hir,
        "stable(feature = \"partial\", since = \"1.0.0\")");
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    partial_drop->attributes[0].metadata = saved_metadata;

    assert(into_iter->attribute_count == 4u
        && into_iter->attributes != NULL);
    saved_metadata = into_iter->attributes[1].metadata;
    into_iter->attributes[1].metadata = cm_hir_intern(&fixture.hir,
        "rustc_layout_scalar_valid_range_start(0)");
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    into_iter->attributes[1].metadata = saved_metadata;

    saved_receiver = partial_method->data.function_item.signature.receiver;
    partial_method->data.function_item.signature.receiver =
        CM_HIR_RECEIVER_REF_SHARED;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    partial_method->data.function_item.signature.receiver = saved_receiver;

    saved_safety = partial_method->data.function_item.signature.safety;
    partial_method->data.function_item.signature.safety = CM_HIR_SAFE;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    partial_method->data.function_item.signature.safety = saved_safety;

    saved_parameter_type = partial_method->data.function_item.signature
        .parameters[1].type;
    partial_method->data.function_item.signature.parameters[1].type =
        partial_method->data.function_item.signature.return_type;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    partial_method->data.function_item.signature.parameters[1].type =
        saved_parameter_type;

    saved_parameter_type = into_iter->data.aggregate_item.fields[0].type;
    into_iter->data.aggregate_item.fields[0].type =
        const_generic->declared_type;
    ASSERT_INTO_ITER_ATOMIC_FAILURE();
    into_iter->data.aggregate_item.fields[0].type = saved_parameter_type;

#undef ASSERT_INTO_ITER_ATOMIC_FAILURE
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_from_fn_callable_closure_and_determinism(void)
{
    CaptureFixture fixture;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureInput noisy_input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;
    const CmHirDeclarationTrait *fn_once = NULL;
    const CmHirDeclarationTrait *fn_mut = NULL;
    const CmHirDeclarationAssociatedItem *output = NULL;
    const CmHirDeclarationValue *build;
    const CmHirDeclarationPredicate *predicate;
    const CmHirDeclarationType *tuple;
    const CmHirDeclarationType *array;
    uint32_t fn_once_local = 0u;
    uint32_t fn_mut_local = 0u;
    uint32_t output_local = 0u;
    size_t index;
    from_fn_fixture_init(&fixture, 0);
    from_fn_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&fixture);
    noisy_input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.trait_count == 3u && result.associated_count == 3u
        && result.value_count == 1u && result.predicate_count == 3u
        && result.projected_semantic_attribute_count == 15u
        && metadata.type_count == 12u);
    result = cm_hir_declaration_metadata_capture(&noisy_input,
        &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    for (index = 0u; index < metadata.trait_count; ++index) {
        if (declaration_string_is(metadata.traits[index].lang_item,
                "fn_once")) {
            fn_once = &metadata.traits[index];
            fn_once_local = (uint32_t)(index + 1u);
        } else if (declaration_string_is(metadata.traits[index].lang_item,
                "fn_mut")) {
            fn_mut = &metadata.traits[index];
            fn_mut_local = (uint32_t)(index + 1u);
        }
    }
    for (index = 0u; index < metadata.associated_count; ++index)
        if (declaration_string_is(metadata.associated_items[index].lang_item,
                "fn_once_output")) {
            output = &metadata.associated_items[index];
            output_local = (uint32_t)(index + 1u);
        }
    build = find_declaration_value(&metadata, "build", NULL);
    assert(fn_once != NULL && fn_mut != NULL && output != NULL
        && build != NULL && fn_once_local != 0u && fn_mut_local != 0u
        && output_local != 0u
        && fn_once->generic_count == 1u && fn_once->predicate_count == 1u
        && fn_once->associated_count == 2u
        && fn_mut->generic_count == 1u && fn_mut->predicate_count == 1u
        && fn_mut->supertrait_count == 1u
        && fn_mut->supertraits[0].trait_local == fn_once_local
        && fn_mut->supertraits[0].argument_count == 1u
        && output->kind == CM_HIR_DECL_ASSOCIATED_TYPE
        && output->parent_local == fn_once_local
        && build->generic_count == 3u && build->parameter_count == 1u
        && build->predicate_count == 1u && build->has_body == 1u);
    predicate = &metadata.predicates[build->predicate_start - 1u];
    tuple = &metadata.types[predicate->argument_types[0] - 1u];
    array = &metadata.types[build->return_type - 1u];
    assert(predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_VALUE
        && predicate->trait_local == fn_mut_local
        && predicate->argument_count == 1u
        && predicate->equality_count == 1u
        && predicate->equalities[0].associated_local == output_local
        && tuple->kind == CM_HIR_DECL_TYPE_TUPLE
        && tuple->element_count == 1u
        && array->kind == CM_HIR_DECL_TYPE_ARRAY
        && array->array_length_kind
            == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
        && array->array_length_generic_local == build->generic_start + 1u);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&fixture);
}

static void test_from_fn_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationTrait *saved_traits;
    CmHirDeclarationAssociatedItem *saved_associated;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationType *saved_types;
    CmHirItem *build;
    CmHirItem *tuple_trait;
    CmHirItem *fn_mut;
    CmHirItem *output;
    CmHirItem *call_mut;
    CmHirItemId ignored_id;
    CmHirGenericParam *const_generic;
    CmHirType *return_type;
    CmHirBody *body;
    CmHirGenericParamKind saved_generic_kind;
    CmHirTypeId saved_declared_type;
    CmHirGenericParamId saved_length_parameter;
    CmHirDefId saved_definition;
    CmHirReceiverKind saved_receiver;
    CmHirTypeId saved_expected_type;
    CmInternId saved_attribute_metadata;

    from_fn_fixture_init(&fixture, 0);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_traits = metadata.traits;
    saved_associated = metadata.associated_items;
    saved_values = metadata.values;
    saved_types = metadata.types;
    build = (CmHirItem *)find_item(&fixture, "build", &ignored_id);
    tuple_trait = (CmHirItem *)find_item(&fixture, "Tuple", &ignored_id);
    fn_mut = (CmHirItem *)find_item(&fixture, "FnMut", &ignored_id);
    output = (CmHirItem *)find_item(&fixture, "Output", &ignored_id);
    call_mut = (CmHirItem *)find_item(&fixture, "call_mut", &ignored_id);
    assert(build != NULL && tuple_trait != NULL && fn_mut != NULL
        && output != NULL && call_mut != NULL
        && build->generic_parameter_count == 3u
        && build->predicate_count == 1u
        && build->data.function_item.signature.return_type
            != CM_HIR_TYPE_NONE);
    const_generic = (CmHirGenericParam *)cm_hir_get_generic_param(
        &fixture.hir, build->generic_parameter_start + 1u);
    return_type = (CmHirType *)cm_hir_get_type(&fixture.hir,
        build->data.function_item.signature.return_type);
    body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        build->data.function_item.body);
    assert(const_generic != NULL && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_ARRAY_KIND && body != NULL);

#define ASSERT_FROM_FN_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.traits == saved_traits \
        && metadata.associated_items == saved_associated \
        && metadata.values == saved_values \
        && metadata.types == saved_types); \
} while (0)

    saved_generic_kind = const_generic->kind;
    const_generic->kind = CM_HIR_GENERIC_TYPE;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    const_generic->kind = saved_generic_kind;

    saved_declared_type = const_generic->declared_type;
    const_generic->declared_type = CM_HIR_TYPE_NONE;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    const_generic->declared_type = saved_declared_type;

    saved_length_parameter =
        return_type->data.array_type.length.data.parameter;
    return_type->data.array_type.length.data.parameter =
        build->generic_parameter_start;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    return_type->data.array_type.length.data.parameter =
        saved_length_parameter;

    saved_definition = build->predicates[0].trait_type.definition;
    build->predicates[0].trait_type.definition = tuple_trait->definition;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    build->predicates[0].trait_type.definition = saved_definition;

    saved_definition = build->predicates[0].equalities[0].associated_type;
    build->predicates[0].equalities[0].associated_type =
        fn_mut->definition;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    build->predicates[0].equalities[0].associated_type = saved_definition;

    saved_definition = fn_mut->data.trait_item.supertraits[0]
        .trait_type.definition;
    fn_mut->data.trait_item.supertraits[0].trait_type.definition =
        tuple_trait->definition;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    fn_mut->data.trait_item.supertraits[0].trait_type.definition =
        saved_definition;

    saved_receiver = call_mut->data.function_item.signature.receiver;
    call_mut->data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    call_mut->data.function_item.signature.receiver = saved_receiver;

    saved_definition = output->parent_definition;
    output->parent_definition = fn_mut->definition;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    output->parent_definition = saved_definition;

    saved_expected_type = body->expected_type;
    body->expected_type = build->data.function_item.signature
        .parameters[0].type;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    body->expected_type = saved_expected_type;

    assert(build->attribute_count == 2u);
    saved_attribute_metadata = build->attributes[1].metadata;
    build->attributes[1].metadata = build->attributes[0].metadata;
    ASSERT_FROM_FN_ATOMIC_FAILURE();
    build->attributes[1].metadata = saved_attribute_metadata;

#undef ASSERT_FROM_FN_ATOMIC_FAILURE
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_from_mut_elision_profile_and_determinism(void)
{
    CaptureFixture fixture;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureInput noisy_input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationValue *value;
    const CmHirDeclarationType *input_reference;
    const CmHirDeclarationType *output_reference;
    const CmHirDeclarationType *array;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;
    from_mut_fixture_init(&fixture, 0);
    from_mut_fixture_init(&noisy, 1);
    input = capture_input(&fixture);
    noisy_input = capture_input(&noisy);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    result = cm_hir_declaration_metadata_capture(&noisy_input,
        &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    value = find_declaration_value(&metadata, "borrow", NULL);
    assert(value != NULL && value->kind == CM_HIR_DECL_VALUE_FUNCTION
        && value->generic_count == 1u && value->predicate_count == 0u
        && value->parameter_count == 1u && value->parameter_types != NULL
        && value->is_const == 1u && value->has_body == 1u);
    input_reference = &metadata.types[value->parameter_types[0] - 1u];
    output_reference = &metadata.types[value->return_type - 1u];
    assert(input_reference->kind == CM_HIR_DECL_TYPE_REFERENCE
        && input_reference->mutability == CM_HIR_DECL_MUTABLE
        && input_reference->region.kind == CM_HIR_DECL_REGION_ERASED
        && output_reference->kind == CM_HIR_DECL_TYPE_REFERENCE
        && output_reference->mutability == CM_HIR_DECL_MUTABLE
        && output_reference->region.kind == CM_HIR_DECL_REGION_ERASED);
    array = &metadata.types[output_reference->child_type - 1u];
    assert(array->kind == CM_HIR_DECL_TYPE_ARRAY
        && array->array_length_kind == CM_HIR_DECL_ARRAY_LENGTH_SCALAR
        && array->array_length_low_bits == 1u
        && array->array_length_high_bits == 0u
        && input_reference->child_type == array->child_type);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&fixture);
}

static void test_from_mut_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CaptureFixture explicit_infer;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationType *saved_types;
    CmHirItem *item;
    CmHirItemId item_id;
    CmHirGenericParam *generic;
    CmHirFunctionSignature *signature;
    CmHirType *input_reference;
    CmHirType *output_reference;
    CmHirType *array;
    CmHirBody *body;
    CmHirRegionKind saved_region;
    CmHirMutability saved_mutability;
    uint64_t saved_low;
    uint64_t saved_high;
    CmHirTypeId saved_type;
    int saved_flag;
    CmHirDefId saved_definition;
    CmInternId saved_metadata;
    CmSpan saved_span;

    from_mut_fixture_init(&fixture, 0);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_types = metadata.types;
    item = (CmHirItem *)find_item(&fixture, "borrow", &item_id);
    assert(item != NULL && item->generic_parameter_count == 1u);
    generic = (CmHirGenericParam *)cm_hir_get_generic_param(&fixture.hir,
        item->generic_parameter_start);
    signature = &item->data.function_item.signature;
    input_reference = (CmHirType *)cm_hir_get_type(&fixture.hir,
        signature->parameters[0].type);
    output_reference = (CmHirType *)cm_hir_get_type(&fixture.hir,
        signature->return_type);
    array = output_reference == NULL ? NULL
        : (CmHirType *)cm_hir_get_type(&fixture.hir,
            output_reference->data.reference_type.pointee);
    body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        item->data.function_item.body);
    assert(generic != NULL && input_reference != NULL
        && output_reference != NULL && array != NULL && body != NULL);

#define ASSERT_FROM_MUT_ATOMIC_FAILURE(input_) do { \
    result = cm_hir_declaration_metadata_capture(&(input_), &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values \
        && metadata.types == saved_types); \
} while (0)

    saved_region = input_reference->data.reference_type.region.kind;
    input_reference->data.reference_type.region.kind = CM_HIR_REGION_INFER;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    input_reference->data.reference_type.region.kind = saved_region;

    saved_region = output_reference->data.reference_type.region.kind;
    output_reference->data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    output_reference->data.reference_type.region.kind = saved_region;

    saved_mutability = input_reference->data.reference_type.mutability;
    input_reference->data.reference_type.mutability = CM_HIR_IMMUTABLE;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    input_reference->data.reference_type.mutability = saved_mutability;

    saved_mutability = output_reference->data.reference_type.mutability;
    output_reference->data.reference_type.mutability = CM_HIR_IMMUTABLE;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    output_reference->data.reference_type.mutability = saved_mutability;

    saved_low = array->data.array_type.length.data.value.low_bits;
    array->data.array_type.length.data.value.low_bits = 2u;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    array->data.array_type.length.data.value.low_bits = saved_low;

    saved_high = array->data.array_type.length.data.value.high_bits;
    array->data.array_type.length.data.value.high_bits = 1u;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    array->data.array_type.length.data.value.high_bits = saved_high;

    saved_type = array->data.array_type.element;
    array->data.array_type.element = array->data.array_type.length.type;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    array->data.array_type.element = saved_type;

    saved_flag = generic->is_relaxed_sized;
    generic->is_relaxed_sized = 1;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    generic->is_relaxed_sized = saved_flag;

    saved_definition = generic->owner;
    generic->owner.index += 1000u;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    generic->owner = saved_definition;

    saved_flag = signature->is_const;
    signature->is_const = 0;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    signature->is_const = saved_flag;

    saved_type = body->expected_type;
    body->expected_type = signature->parameters[0].type;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    body->expected_type = saved_type;

    saved_type = body->locals[0].type;
    body->locals[0].type = signature->return_type;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    body->locals[0].type = saved_type;

    body->locals[0].parameter_index = 1u;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    body->locals[0].parameter_index = 0u;

    saved_definition = item->definition;
    item->definition.index += 1000u;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    item->definition = saved_definition;

    assert(item->attribute_count == 2u && item->attributes != NULL);
    saved_metadata = item->attributes[1].metadata;
    item->attributes[1].metadata = item->attributes[0].metadata;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    item->attributes[1].metadata = saved_metadata;

    saved_span = item->attributes[1].span;
    item->attributes[1].span.start += 1u;
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    item->attributes[1].span = saved_span;

    fixture_init_source(&explicit_infer, 0, "from-mut-explicit.rs",
        from_mut_explicit_infer_source,
        sizeof(from_mut_explicit_infer_source) - 1u);
    input = capture_input(&explicit_infer);
    ASSERT_FROM_MUT_ATOMIC_FAILURE(input);
    fixture_destroy(&explicit_infer);

#undef ASSERT_FROM_MUT_ATOMIC_FAILURE
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_from_ref_elision_profile_and_negatives(void)
{
    CaptureFixture fixture;
    CaptureFixture noisy;
    CaptureFixture rejected;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureInput noisy_input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationType *saved_types;
    const CmHirDeclarationValue *value;
    const CmHirDeclarationType *decl_input;
    const CmHirDeclarationType *decl_output;
    const CmHirDeclarationType *decl_array;
    CmHirItem *item;
    CmHirItemId item_id;
    CmHirGenericParam *generic;
    CmHirFunctionSignature *signature;
    CmHirType *input_reference;
    CmHirType *output_reference;
    CmHirType *array;
    CmHirBody *body;
    CmHirMutability saved_mutability;
    CmHirRegionKind saved_region;
    CmHirDefId saved_definition;
    CmHirTypeId saved_type;
    uint64_t saved_length;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;

    from_ref_fixture_init(&fixture, 0);
    from_ref_fixture_init(&noisy, 1);
    input = capture_input(&fixture);
    noisy_input = capture_input(&noisy);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    result = cm_hir_declaration_metadata_capture(&noisy_input,
        &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    value = find_declaration_value(&metadata, "borrow_shared", NULL);
    assert(value != NULL && value->kind == CM_HIR_DECL_VALUE_FUNCTION
        && value->generic_count == 1u && value->predicate_count == 0u
        && value->parameter_count == 1u && value->parameter_types != NULL
        && value->is_const == 1u && value->has_body == 1u);
    decl_input = &metadata.types[value->parameter_types[0] - 1u];
    decl_output = &metadata.types[value->return_type - 1u];
    assert(decl_input->kind == CM_HIR_DECL_TYPE_REFERENCE
        && decl_input->mutability == CM_HIR_DECL_IMMUTABLE
        && decl_input->region.kind == CM_HIR_DECL_REGION_ERASED
        && decl_output->kind == CM_HIR_DECL_TYPE_REFERENCE
        && decl_output->mutability == CM_HIR_DECL_IMMUTABLE
        && decl_output->region.kind == CM_HIR_DECL_REGION_ERASED);
    decl_array = &metadata.types[decl_output->child_type - 1u];
    assert(decl_array->kind == CM_HIR_DECL_TYPE_ARRAY
        && decl_array->array_length_kind == CM_HIR_DECL_ARRAY_LENGTH_SCALAR
        && decl_array->array_length_low_bits == 1u
        && decl_array->array_length_high_bits == 0u
        && decl_input->child_type == decl_array->child_type);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);

    saved_values = metadata.values;
    saved_types = metadata.types;
    item = (CmHirItem *)find_item(&fixture, "borrow_shared", &item_id);
    assert(item != NULL && item->generic_parameter_count == 1u);
    generic = (CmHirGenericParam *)cm_hir_get_generic_param(&fixture.hir,
        item->generic_parameter_start);
    signature = &item->data.function_item.signature;
    input_reference = (CmHirType *)cm_hir_get_type(&fixture.hir,
        signature->parameters[0].type);
    output_reference = (CmHirType *)cm_hir_get_type(&fixture.hir,
        signature->return_type);
    array = output_reference == NULL ? NULL
        : (CmHirType *)cm_hir_get_type(&fixture.hir,
            output_reference->data.reference_type.pointee);
    body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        item->data.function_item.body);
    assert(generic != NULL && input_reference != NULL
        && output_reference != NULL && array != NULL && body != NULL);

#define ASSERT_FROM_REF_ATOMIC_FAILURE(input_) do { \
    result = cm_hir_declaration_metadata_capture(&(input_), &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values \
        && metadata.types == saved_types); \
} while (0)

    saved_mutability = input_reference->data.reference_type.mutability;
    input_reference->data.reference_type.mutability = CM_HIR_MUTABLE;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    input_reference->data.reference_type.mutability = saved_mutability;

    saved_mutability = output_reference->data.reference_type.mutability;
    output_reference->data.reference_type.mutability = CM_HIR_MUTABLE;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    output_reference->data.reference_type.mutability = saved_mutability;

    saved_region = input_reference->data.reference_type.region.kind;
    input_reference->data.reference_type.region.kind = CM_HIR_REGION_INFER;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    input_reference->data.reference_type.region.kind = saved_region;

    saved_region = output_reference->data.reference_type.region.kind;
    output_reference->data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    output_reference->data.reference_type.region.kind = saved_region;

    saved_length = array->data.array_type.length.data.value.low_bits;
    array->data.array_type.length.data.value.low_bits = 2u;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    array->data.array_type.length.data.value.low_bits = saved_length;

    saved_definition = generic->owner;
    generic->owner.index += 1000u;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    generic->owner = saved_definition;

    saved_type = body->locals[0].type;
    body->locals[0].type = signature->return_type;
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    body->locals[0].type = saved_type;

    fixture_init_source(&rejected, 0, "from-ref-explicit.rs",
        from_ref_explicit_infer_source,
        sizeof(from_ref_explicit_infer_source) - 1u);
    input = capture_input(&rejected);
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    fixture_destroy(&rejected);

    fixture_init_source(&rejected, 0, "from-ref-mixed.rs",
        from_ref_mixed_source, sizeof(from_ref_mixed_source) - 1u);
    input = capture_input(&rejected);
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    fixture_destroy(&rejected);

    fixture_init_source(&rejected, 0, "from-ref-extra.rs",
        from_ref_extra_input_source,
        sizeof(from_ref_extra_input_source) - 1u);
    input = capture_input(&rejected);
    ASSERT_FROM_REF_ATOMIC_FAILURE(input);
    fixture_destroy(&rejected);

#undef ASSERT_FROM_REF_ATOMIC_FAILURE
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&fixture);
}

static const CmHirDeclarationType *type_id_like_array(
    const CmHirDeclarationMetadata *metadata, uint64_t expected_length)
{
    const CmHirDeclarationItem *item;
    const CmHirDeclarationNamespaceEntry *type_entry;
    const CmHirDeclarationNamespaceEntry *value_entry;
    const CmHirDeclarationType *array;
    const CmHirDeclarationType *pointer;
    const CmHirDeclarationType *pointee;
    const CmHirDeclarationType *length_type;
    uint32_t item_local = 0u;
    item = find_declaration_item(metadata, "TypeIdLike", &item_local);
    assert(item != NULL && item_local != 0u
        && item->kind == CM_HIR_DECL_ITEM_STRUCT
        && item->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && item->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
        && item->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST
        && (item->aggregate_flags & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM)
            != 0u
        && declaration_string_is(item->lang_item, "type_id_like")
        && item->field_count == 1u
        && declaration_string_is(item->fields[0].name, "data")
        && item->fields[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_CRATE
        && item->fields[0].visibility.restriction_module == 0u
        && item->fields[0].source_ordinal == 0u
        && item->fields[0].type_local != 0u
        && item->fields[0].type_local <= metadata->type_count);
    type_entry = find_namespace_entry(metadata, item->owner_module,
        CM_HIR_DECL_NAMESPACE_TYPE, "TypeIdLike");
    value_entry = find_namespace_entry(metadata, item->owner_module,
        CM_HIR_DECL_NAMESPACE_VALUE, "TypeIdLike");
    assert(type_entry != NULL
        && type_entry->target_kind == CM_HIR_DECL_TARGET_ITEM
        && type_entry->target_local == item_local && value_entry == NULL);
    array = &metadata->types[item->fields[0].type_local - 1u];
    assert(array->kind == CM_HIR_DECL_TYPE_ARRAY
        && array->child_type != 0u
        && array->child_type <= metadata->type_count
        && array->array_length_type != 0u
        && array->array_length_type <= metadata->type_count
        && array->array_length_low_bits == expected_length
        && array->array_length_high_bits == 0u);
    pointer = &metadata->types[array->child_type - 1u];
    length_type = &metadata->types[array->array_length_type - 1u];
    assert(pointer->kind == CM_HIR_DECL_TYPE_RAW_POINTER
        && pointer->mutability == CM_HIR_DECL_IMMUTABLE
        && pointer->child_type != 0u
        && pointer->child_type <= metadata->type_count
        && length_type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && length_type->primitive == CM_HIR_DECL_PRIMITIVE_USIZE);
    pointee = &metadata->types[pointer->child_type - 1u];
    assert(pointee->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && pointee->primitive == CM_HIR_DECL_PRIMITIVE_UNIT);
    return array;
}

static void test_type_id_like_target_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture narrow;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata narrow_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    type_id_like_fixture_init(&first, 0, "x86_64-unknown-linux-gnu");
    type_id_like_fixture_init(&noisy, 1, "x86_64-unknown-linux-gnu");
    type_id_like_fixture_init(&narrow, 0, "i686-unknown-linux-musl");
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    cm_hir_declaration_metadata_init(&narrow_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u
        && cm_hir_declaration_metadata_validate(&first_metadata)
            == CM_HIR_DECL_METADATA_OK);
    (void)type_id_like_array(&first_metadata, UINT64_C(2));
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    (void)type_id_like_array(&noisy_metadata, UINT64_C(2));
    input = capture_input(&narrow);
    result = cm_hir_declaration_metadata_capture(&input, &narrow_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u
        && cm_hir_declaration_metadata_validate(&narrow_metadata)
            == CM_HIR_DECL_METADATA_OK);
    (void)type_id_like_array(&narrow_metadata, UINT64_C(4));
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&narrow_metadata);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&narrow);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_type_id_like_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationType *saved_types;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId item_id;
    CmHirItem *item;
    CmHirField *field;
    CmHirType *array;
    CmHirType *pointer;
    CmHirTypeId saved_type_id;
    CmHirConstArg saved_length;
    CmHirVisibility saved_visibility;
    CmHirMutability saved_mutability;
    CmSpan saved_span;
    CmInternId saved_metadata;
    CmModuleId root_module;
    CmResolveEffectiveItem effective;
    const CmAst *borrowed_ast;
    CmAst *ast;
    CmAstItem *ast_item;
    CmAstType *ast_array;
    CmInternId saved_text;
    uint32_t saved_pointer_bits;
    CmHirArtifactBytes saved_triple;
    uint32_t attribute_index;
    type_id_like_fixture_init(&fixture, 1, "x86_64-unknown-linux-gnu");
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_types = metadata.types;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)find_item(&fixture, "TypeIdLike", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_STRUCT
        && item->data.aggregate_item.field_count == 1u);
    field = &item->data.aggregate_item.fields[0];
    array = (CmHirType *)cm_hir_get_type(&fixture.hir, field->type);
    assert(array != NULL && array->kind == CM_HIR_TYPE_ARRAY_KIND
        && array->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && array->data.array_type.length.data.value.low_bits == 2u);
    pointer = (CmHirType *)cm_hir_get_type(&fixture.hir,
        array->data.array_type.element);
    assert(pointer != NULL && pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && pointer->data.raw_pointer_type.mutability == CM_HIR_IMMUTABLE);
    assert(cm_module_graph_get_root(&fixture.graph, &root_module)
        && cm_module_graph_get_effective_item(&fixture.graph,
            fixture.graph_result.revision, root_module, 0u, &effective)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_borrow_ast(&fixture.graph, root_module,
            &borrowed_ast));
    ast = (CmAst *)(void *)borrowed_ast;
    ast_item = (CmAstItem *)(void *)cm_ast_get_item(ast,
        effective.declaration.item);
    assert(ast_item != NULL && ast_item->kind == CM_AST_ITEM_STRUCT
        && ast_item->data.aggregate_item.field_count == 1u);
    ast_array = (CmAstType *)(void *)cm_ast_get_type(ast,
        ast_item->data.aggregate_item.fields[0].type);
    assert(ast_array != NULL && ast_array->kind == CM_AST_TYPE_ARRAY);

#define ASSERT_TYPE_ID_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items && metadata.types == saved_types \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_visibility = field->visibility;
    field->visibility.kind = CM_HIR_VIS_PRIVATE;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    field->visibility = saved_visibility;
    field->visibility.restriction = item->definition;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    field->visibility = saved_visibility;

    saved_text = ast_array->text;
    ast_array->text = cm_interner_intern_c_str(&ast->strings,
        "16 * size_of::<*const ()>()");
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    ast_array->text = cm_interner_intern_c_str(&ast->strings,
        "24 / size_of::<*const ()>()");
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    ast_array->text = saved_text;

    saved_mutability = pointer->data.raw_pointer_type.mutability;
    pointer->data.raw_pointer_type.mutability = CM_HIR_MUTABLE;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    pointer->data.raw_pointer_type.mutability = saved_mutability;
    saved_type_id = pointer->data.raw_pointer_type.pointee;
    pointer->data.raw_pointer_type.pointee =
        array->data.array_type.length.type;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    pointer->data.raw_pointer_type.pointee = saved_type_id;

    saved_length = array->data.array_type.length;
    array->data.array_type.length.kind = CM_HIR_CONST_PARAMETER;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    array->data.array_type.length = saved_length;
    array->data.array_type.length.type =
        pointer->data.raw_pointer_type.pointee;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    array->data.array_type.length = saved_length;
    array->data.array_type.length.data.value.low_bits = 3u;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    array->data.array_type.length = saved_length;
    array->data.array_type.length.data.value.high_bits = 1u;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    array->data.array_type.length = saved_length;

    saved_pointer_bits = input.target_pointer_bits;
    input.target_pointer_bits = 32u;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    input.target_pointer_bits = saved_pointer_bits;
    saved_triple = input.target_triple;
    input.target_triple = test_bytes("i686-unknown-linux-musl");
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    input.target_triple = saved_triple;

    saved_span = field->span;
    field->span.start += 1u;
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    field->span = saved_span;
    for (attribute_index = 0u; attribute_index < item->attribute_count;
            ++attribute_index) {
        const CmInternedString *attribute = cm_interner_get(
            &fixture.hir.strings,
            item->attributes[attribute_index].metadata);
        if (attribute != NULL && attribute->len >= 4u
            && memcmp(attribute->bytes, "lang", 4u) == 0) break;
    }
    assert(attribute_index < item->attribute_count);
    saved_metadata = item->attributes[attribute_index].metadata;
    item->attributes[attribute_index].metadata = cm_hir_intern(&fixture.hir,
        "lang = \"forged_type_id\"");
    ASSERT_TYPE_ID_ATOMIC_FAILURE();
    item->attributes[attribute_index].metadata = saved_metadata;

#undef ASSERT_TYPE_ID_ATOMIC_FAILURE
    (void)type_id_like_array(&metadata, UINT64_C(2));
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void assert_static_descriptor(
    const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *direct;
    const CmHirDeclarationNamespaceEntry *renamed;
    const CmHirDeclarationType *tuple;
    const CmHirDeclarationType *array;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u
        && metadata->trait_count == 1u && metadata->item_count == 0u
        && metadata->value_count == 2u && metadata->generic_count == 2u
        && metadata->predicate_count == 1u && metadata->type_count == 8u
        && metadata->namespace_count == 4u);
    assert(metadata->types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && metadata->types[1].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[1].primitive == CM_HIR_DECL_PRIMITIVE_I16
        && metadata->types[2].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[2].primitive == CM_HIR_DECL_PRIMITIVE_U8
        && metadata->types[3].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[3].primitive == CM_HIR_DECL_PRIMITIVE_U64
        && metadata->types[4].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[4].primitive == CM_HIR_DECL_PRIMITIVE_USIZE
        && metadata->types[5].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[5].generic_local == 2u);
    tuple = &metadata->types[6];
    array = &metadata->types[7];
    assert(tuple->kind == CM_HIR_DECL_TYPE_TUPLE
        && tuple->element_count == 3u && tuple->element_types != NULL
        && tuple->element_types[0] == 4u
        && tuple->element_types[1] == 2u
        && tuple->element_types[2] == 2u);
    assert(array->kind == CM_HIR_DECL_TYPE_ARRAY
        && array->child_type == 7u && array->array_length_type == 5u
        && array->array_length_low_bits == UINT64_C(2)
        && array->array_length_high_bits == 0u);
    assert(metadata->values[0].kind == CM_HIR_DECL_VALUE_STATIC
        && declaration_string_is(metadata->values[0].name, "TABLE")
        && metadata->values[0].source_ordinal == 0u
        && metadata->values[0].generic_start == 0u
        && metadata->values[0].generic_count == 0u
        && metadata->values[0].predicate_start == 0u
        && metadata->values[0].predicate_count == 0u
        && metadata->values[0].parameter_count == 0u
        && metadata->values[0].parameter_types == NULL
        && metadata->values[0].return_type == 0u
        && metadata->values[0].declared_type == 8u
        && metadata->values[0].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[0].has_body == 1u);
    assert(metadata->values[1].kind == CM_HIR_DECL_VALUE_FUNCTION
        && declaration_string_is(metadata->values[1].name, "needs")
        && metadata->values[1].source_ordinal == 3u
        && metadata->values[1].generic_count == 1u
        && metadata->values[1].predicate_count == 1u
        && metadata->values[1].return_type == 1u);
    assert(metadata->generics[1].owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && metadata->generics[1].owner_local == 2u
        && metadata->predicates[0].owner_value == 2u
        && metadata->predicates[0].subject_type == 6u
        && metadata->predicates[0].argument_types[0] == 3u);
    direct = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "TABLE");
    renamed = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "RENAMED_TABLE");
    assert(direct != NULL && renamed != NULL
        && direct->target_kind == CM_HIR_DECL_TARGET_VALUE
        && renamed->target_kind == CM_HIR_DECL_TARGET_VALUE
        && direct->target_local == 1u && renamed->target_local == 1u
        && direct->export_ordinal == 0u && renamed->export_ordinal == 1u);
}

static void test_static_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    static_fixture_init(&first, 0);
    static_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.value_count == 2u && result.namespace_count == 4u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION
        && result.projected_semantic_attribute_count == 1u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 1u);
    assert_static_descriptor(&first_metadata);
    assert_static_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_static_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId item_id;
    CmHirItem *item;
    CmHirBody *body;
    CmHirType *array;
    CmHirType *tuple;
    CmHirTypeId saved_element;
    CmHirTypeId saved_length_type;
    uint64_t saved_length;
    CmSpan saved_span;
    CmInternId saved_metadata;
    uint32_t saved_source_expression;
    CmHirBodyId saved_body;
    CmHirDefId saved_definition;
    CmModuleId root_module;
    const CmAst *borrowed_ast;
    CmAstItem *ast_item;
    CmResolveEffectiveItem effective;
    static_fixture_init(&fixture, 1);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)find_item(&fixture, "TABLE", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_STATIC
        && item->attribute_count == 1u
        && item->data.value_item.body != CM_HIR_BODY_NONE);
    body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        item->data.value_item.body);
    array = (CmHirType *)cm_hir_get_type(&fixture.hir,
        item->data.value_item.type);
    assert(body != NULL && array != NULL
        && array->kind == CM_HIR_TYPE_ARRAY_KIND);
    tuple = (CmHirType *)cm_hir_get_type(&fixture.hir,
        array->data.array_type.element);
    assert(tuple != NULL && tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && tuple->data.tuple_type.element_count == 3u);

#define ASSERT_STATIC_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_length = array->data.array_type.length.data.value.low_bits;
    array->data.array_type.length.data.value.low_bits = UINT64_C(3);
    ASSERT_STATIC_ATOMIC_FAILURE();
    array->data.array_type.length.data.value.low_bits = saved_length;

    array->data.array_type.length.data.value.high_bits = UINT64_C(1);
    ASSERT_STATIC_ATOMIC_FAILURE();
    array->data.array_type.length.data.value.high_bits = 0u;

    saved_length_type = array->data.array_type.length.type;
    array->data.array_type.length.type = tuple->data.tuple_type.elements[0];
    ASSERT_STATIC_ATOMIC_FAILURE();
    array->data.array_type.length.type = saved_length_type;

    saved_element = tuple->data.tuple_type.elements[1];
    tuple->data.tuple_type.elements[1] = tuple->data.tuple_type.elements[0];
    ASSERT_STATIC_ATOMIC_FAILURE();
    tuple->data.tuple_type.elements[1] = saved_element;

    tuple->data.tuple_type.element_count = 2u;
    ASSERT_STATIC_ATOMIC_FAILURE();
    tuple->data.tuple_type.element_count = 3u;

    saved_span = tuple->span;
    tuple->span.start += 1u;
    ASSERT_STATIC_ATOMIC_FAILURE();
    tuple->span = saved_span;

    item->data.value_item.mutability = CM_HIR_MUTABLE;
    ASSERT_STATIC_ATOMIC_FAILURE();
    item->data.value_item.mutability = CM_HIR_IMMUTABLE;

    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&fixture.hir, "doc(alias)");
    ASSERT_STATIC_ATOMIC_FAILURE();
    item->attributes[0].metadata = saved_metadata;

    item->attributes[0].expansion_depth = 1u;
    ASSERT_STATIC_ATOMIC_FAILURE();
    item->attributes[0].expansion_depth = 0u;

    saved_source_expression = body->source_expression_id;
    body->source_expression_id = UINT32_MAX;
    ASSERT_STATIC_ATOMIC_FAILURE();
    body->source_expression_id = saved_source_expression;

    saved_body = item->data.value_item.body;
    item->data.value_item.body = CM_HIR_BODY_NONE;
    ASSERT_STATIC_ATOMIC_FAILURE();
    item->data.value_item.body = saved_body;

    body->expected_type = array->data.array_type.element;
    ASSERT_STATIC_ATOMIC_FAILURE();
    body->expected_type = item->data.value_item.type;

    assert(cm_module_graph_get_root(&fixture.graph, &root_module)
        && cm_module_graph_get_effective_item(&fixture.graph,
            fixture.graph_result.revision, root_module, 0u, &effective)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_borrow_ast(&fixture.graph, root_module,
            &borrowed_ast));
    ast_item = (CmAstItem *)(void *)cm_ast_get_item(borrowed_ast,
        effective.declaration.item);
    assert(ast_item != NULL && ast_item->kind == CM_AST_ITEM_STATIC
        && ast_item->data.value_item.has_value);
    ast_item->data.value_item.has_value = 0;
    ASSERT_STATIC_ATOMIC_FAILURE();
    ast_item->data.value_item.has_value = 1;

    saved_definition = item->definition;
    item->definition.index += 1u;
    ASSERT_STATIC_ATOMIC_FAILURE();
    item->definition = saved_definition;

    assert_static_descriptor(&metadata);
#undef ASSERT_STATIC_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_static_type_dag_is_structurally_deduplicated(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    fixture_init_source(&fixture, 1, "v30-duplicate-static.rs",
        duplicate_static_fixture_source,
        sizeof(duplicate_static_fixture_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u
        && metadata.type_count == 8u && metadata.value_count == 3u
        && metadata.values[0].kind == CM_HIR_DECL_VALUE_STATIC
        && metadata.values[1].kind == CM_HIR_DECL_VALUE_STATIC
        && metadata.values[0].declared_type == 8u
        && metadata.values[1].declared_type == 8u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void assert_generic_enum_descriptor(
    const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationItem *maybe;
    const CmHirDeclarationItem *outcome;
    uint32_t maybe_local;
    uint32_t outcome_local;
    uint32_t variant_locals[4];
    size_t index;
    unsigned int type_counts[4] = { 0u, 0u, 0u, 0u };
    unsigned int value_counts[4] = { 0u, 0u, 0u, 0u };
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    maybe = find_declaration_item(metadata, "Maybe", &maybe_local);
    outcome = find_declaration_item(metadata, "Outcome", &outcome_local);
    assert(maybe != NULL && outcome != NULL
        && maybe->kind == CM_HIR_DECL_ITEM_ENUM
        && maybe->generic_count == 1u
        && maybe->enum_repr_primitive == CM_HIR_DECL_ENUM_REPR_RUST
        && maybe->enum_flags == CM_HIR_DECL_ENUM_HAS_LANG_ITEM
        && declaration_string_is(maybe->enum_lang_item, "Maybe")
        && declaration_string_is(maybe->diagnostic_item, "Maybe")
        && maybe->variant_count == 2u
        && maybe->variants[0].kind == CM_HIR_DECL_VARIANT_UNIT
        && maybe->variants[0].field_count == 0u
        && maybe->variants[0].fields == NULL
        && maybe->variants[0].flags
            == CM_HIR_DECL_VARIANT_HAS_LANG_ITEM
        && declaration_string_is(maybe->variants[0].lang_item, "Nothing")
        && maybe->variants[1].kind == CM_HIR_DECL_VARIANT_TUPLE
        && maybe->variants[1].field_count == 1u
        && maybe->variants[1].fields != NULL
        && maybe->variants[1].fields[0].source_ordinal == 0u
        && maybe->variants[1].fields[0].type_local != 0u
        && declaration_string_is(maybe->variants[1].lang_item, "Just")
        && outcome->kind == CM_HIR_DECL_ITEM_ENUM
        && outcome->generic_count == 2u
        && outcome->enum_flags == 0u
        && outcome->enum_lang_item.data == NULL
        && outcome->enum_lang_item.length == 0u
        && declaration_string_is(outcome->diagnostic_item, "Outcome")
        && outcome->variant_count == 2u
        && outcome->variants[0].kind == CM_HIR_DECL_VARIANT_TUPLE
        && outcome->variants[0].field_count == 1u
        && declaration_string_is(outcome->variants[0].lang_item, "Good")
        && outcome->variants[1].kind == CM_HIR_DECL_VARIANT_TUPLE
        && outcome->variants[1].field_count == 1u
        && declaration_string_is(outcome->variants[1].lang_item, "Bad"));
    assert(metadata->types[maybe->variants[1].fields[0].type_local - 1u].kind
            == CM_HIR_DECL_TYPE_GENERIC
        && metadata->generics[metadata->types[
            maybe->variants[1].fields[0].type_local - 1u].generic_local - 1u]
            .owner_kind == CM_HIR_DECL_GENERIC_ITEM
        && metadata->generics[metadata->types[
            maybe->variants[1].fields[0].type_local - 1u].generic_local - 1u]
            .owner_local == maybe_local);
    variant_locals[0] = declaration_variant_local(metadata, maybe_local, 0u);
    variant_locals[1] = declaration_variant_local(metadata, maybe_local, 1u);
    variant_locals[2] = declaration_variant_local(metadata, outcome_local, 0u);
    variant_locals[3] = declaration_variant_local(metadata, outcome_local, 1u);
    assert(variant_locals[0] != 0u && variant_locals[1] != 0u
        && variant_locals[2] != 0u && variant_locals[3] != 0u);
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        uint32_t variant_index;
        if (entry->target_kind != CM_HIR_DECL_TARGET_ENUM_VARIANT) continue;
        for (variant_index = 0u; variant_index < 4u; ++variant_index) {
            if (entry->target_local != variant_locals[variant_index])
                continue;
            if (entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE)
                type_counts[variant_index] += 1u;
            else if (entry->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE)
                value_counts[variant_index] += 1u;
        }
    }
    for (index = 0u; index < 4u; ++index)
        assert(type_counts[index] == 2u && value_counts[index] == 2u);
}

static void test_generic_enum_projection_glob_and_field_boundary(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture no_field_attributes;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata no_field_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    CmByteBuf no_field_bytes;
    generic_enum_fixture_init(&first, 0, 1);
    generic_enum_fixture_init(&noisy, 1, 1);
    generic_enum_fixture_init(&no_field_attributes, 0, 0);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    cm_hir_declaration_metadata_init(&no_field_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "generic enum capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s binding=%u ast=%u namespace=%u "
            "item=%u def=%u:%u span=%u:%u-%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_binding_kind,
            (unsigned int)result.rejected_ast_item_kind,
            (unsigned int)result.rejected_namespace_kind,
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_definition.crate_id,
            (unsigned int)result.rejected_definition.index,
            (unsigned int)result.rejected_span.source,
            (unsigned int)result.rejected_span.start,
            (unsigned int)result.rejected_span.end);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 2u
        && result.projected_semantic_attribute_count == 18u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION);
    assert_generic_enum_descriptor(&first_metadata);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 18u);
    assert_generic_enum_descriptor(&noisy_metadata);
    input = capture_input(&no_field_attributes);
    result = cm_hir_declaration_metadata_capture(&input, &no_field_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 18u);
    assert_generic_enum_descriptor(&no_field_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    cm_byte_buf_init(&no_field_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&no_field_metadata,
            &no_field_bytes) == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && first_bytes.len == no_field_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0
        && memcmp(first_bytes.data, no_field_bytes.data,
            first_bytes.len) == 0);
    cm_byte_buf_destroy(&no_field_bytes);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&no_field_metadata);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&no_field_attributes);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_generic_enum_hostile_mutations_are_atomic(void)
{
    static const char source_template[] =
        "#[doc(search_unbox)]\n"
        "#[derive(Copy)]\n"
        "#[rustc_diagnostic_item = \"Choice\"]\n"
        "#[stable(feature = \"choice\", since = \"1.0.0\")]\n"
        "%s"
        "pub enum Choice<%s> {\n%s}\n"
        "#[stable(feature = \"choice\", since = \"1.0.0\")]\n"
        "#[%s]\n"
        "pub use Choice::{A, B};\n"
        "pub trait Gate<X: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const char good_variants[] =
        "  #[lang = \"A\"]\n"
        "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
        "  A,\n"
        "  #[lang = \"B\"]\n"
        "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
        "  B(T),\n";
    static const struct {
        const char *generic;
        const char *item_extra;
        const char *variants;
        const char *reexport_doc;
    } rejected[] = {
        { "T: ?Sized", "", good_variants, "doc(no_inline)" },
        { "T", "", "  #[lang = \"A\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  A,\n"
            "  #[lang = \"B\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  B(u8),\n", "doc(no_inline)" },
        { "T", "", "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  A,\n"
            "  #[lang = \"B\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  B(T),\n", "doc(no_inline)" },
        { "T", "", "  #[lang = \"A\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  A = 0,\n"
            "  #[lang = \"B\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  B(T),\n", "doc(no_inline)" },
        { "T", "#[repr(C)]\n", good_variants, "doc(no_inline)" },
        { "T", "", good_variants, "doc(notable_trait)" },
        { "T", "", "  #[lang = \"A\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  A,\n"
            "  #[lang = \"B\"]\n"
            "  #[stable(feature = \"choice\", since = \"1.0.0\")]\n"
            "  B,\n", "doc(no_inline)" }
    };
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItem *outcome;
    CmHirItemId outcome_id;
    CmHirTypeId saved_type;
    CmHirAggregateForm saved_form;
    CmInternId saved_lang_item;
    CmHirGenericParam *generic;
    CmHirImportBinding *good_value = NULL;
    CmHirDefId bad_definition;
    CmHirDefId saved_binding_target;
    int saved_relaxed;
    size_t index;
    generic_enum_fixture_init(&good, 0, 1);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    outcome = (CmHirItem *)find_item(&good, "Outcome", &outcome_id);
    assert(outcome != NULL && outcome->kind == CM_HIR_ITEM_ENUM
        && outcome->generic_parameter_count == 2u
        && outcome->data.enum_item.variant_count == 2u
        && outcome->data.enum_item.variants[0].field_count == 1u
        && outcome->data.enum_item.variants[1].field_count == 1u);

#define ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(input_) do { \
    result = cm_hir_declaration_metadata_capture(&(input_), &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_type = outcome->data.enum_item.variants[0].fields[0].type;
    outcome->data.enum_item.variants[0].fields[0].type =
        outcome->data.enum_item.variants[1].fields[0].type;
    ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(input);
    outcome->data.enum_item.variants[0].fields[0].type = saved_type;

    saved_form = outcome->data.enum_item.variants[0].form;
    outcome->data.enum_item.variants[0].form = CM_HIR_AGGREGATE_UNIT;
    ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(input);
    outcome->data.enum_item.variants[0].form = saved_form;

    generic = (CmHirGenericParam *)cm_hir_get_generic_param(&good.hir,
        outcome->generic_parameter_start);
    assert(generic != NULL);
    saved_relaxed = generic->is_relaxed_sized;
    generic->is_relaxed_sized = 1;
    ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(input);
    generic->is_relaxed_sized = saved_relaxed;

    saved_lang_item = outcome->data.enum_item.variants[0].lang_item;
    assert(saved_lang_item != CM_INTERN_ID_NONE
        && outcome->data.enum_item.variants[1].lang_item
            != CM_INTERN_ID_NONE
        && saved_lang_item
            != outcome->data.enum_item.variants[1].lang_item);
    outcome->data.enum_item.variants[0].lang_item =
        outcome->data.enum_item.variants[1].lang_item;
    ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(input);
    outcome->data.enum_item.variants[0].lang_item = saved_lang_item;

    memset(&bad_definition, 0, sizeof(bad_definition));
    for (index = 0u; index < good.hir.modules.len; ++index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(&good.hir.modules,
            index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != good.lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *import = &module->imports[import_index];
            uint32_t binding_index;
            for (binding_index = 0u; binding_index < import->binding_count;
                    ++binding_index) {
                CmHirImportBinding *binding =
                    &import->bindings[binding_index];
                const CmInternedString *name = cm_interner_get(
                    &good.hir.strings, binding->name);
                if (binding->namespace_kind != CM_HIR_NAMESPACE_VALUE
                    || name == NULL) continue;
                if (name->len == strlen("Good")
                    && memcmp(name->bytes, "Good", name->len) == 0)
                    good_value = binding;
                if (name->len == strlen("Bad")
                    && memcmp(name->bytes, "Bad", name->len) == 0)
                    bad_definition = binding->target;
            }
        }
    }
    assert(good_value != NULL && !cm_hir_def_id_is_none(bad_definition)
        && !cm_hir_def_id_equal(good_value->target, bad_definition));
    saved_binding_target = good_value->target;
    good_value->target = bad_definition;
    ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(input);
    good_value->target = saved_binding_target;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        CaptureFixture bad;
        char source[4096];
        int written = snprintf(source, sizeof(source), source_template,
            rejected[index].item_extra, rejected[index].generic,
            rejected[index].variants, rejected[index].reexport_doc);
        CmHirDeclarationCaptureInput bad_input;
        assert(written > 0 && (size_t)written < sizeof(source));
        fixture_init_source(&bad, 0, "bad-generic-enum.rs",
            (const unsigned char *)source, (size_t)written);
        bad_input = capture_input(&bad);
        ASSERT_GENERIC_ENUM_ATOMIC_FAILURE(bad_input);
        fixture_destroy(&bad);
    }
    assert_generic_enum_descriptor(&metadata);
#undef ASSERT_GENERIC_ENUM_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void assert_primitive_reexport_descriptor(
    const CmHirDeclarationMetadata *metadata)
{
    static const struct {
        uint32_t owner_module;
        const char *name;
        uint32_t primitive;
    } expected[] = {
        { 2u, "bool", CM_HIR_DECL_PRIMITIVE_BOOL },
        { 2u, "char", CM_HIR_DECL_PRIMITIVE_CHAR },
        { 2u, "str", CM_HIR_DECL_PRIMITIVE_STR },
        { 2u, "i8", CM_HIR_DECL_PRIMITIVE_I8 },
        { 2u, "i16", CM_HIR_DECL_PRIMITIVE_I16 },
        { 2u, "i32", CM_HIR_DECL_PRIMITIVE_I32 },
        { 2u, "i64", CM_HIR_DECL_PRIMITIVE_I64 },
        { 2u, "i128", CM_HIR_DECL_PRIMITIVE_I128 },
        { 2u, "isize", CM_HIR_DECL_PRIMITIVE_ISIZE },
        { 2u, "u8", CM_HIR_DECL_PRIMITIVE_U8 },
        { 2u, "u16", CM_HIR_DECL_PRIMITIVE_U16 },
        { 2u, "u32", CM_HIR_DECL_PRIMITIVE_U32 },
        { 2u, "u64", CM_HIR_DECL_PRIMITIVE_U64 },
        { 2u, "u128", CM_HIR_DECL_PRIMITIVE_U128 },
        { 2u, "usize", CM_HIR_DECL_PRIMITIVE_USIZE },
        { 2u, "f32", CM_HIR_DECL_PRIMITIVE_F32 },
        { 2u, "f64", CM_HIR_DECL_PRIMITIVE_F64 },
        { 3u, "byte", CM_HIR_DECL_PRIMITIVE_U8 },
        { 2u, "octet", CM_HIR_DECL_PRIMITIVE_U8 }
    };
    size_t primitive_count = 0u;
    size_t index;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    for (index = 0u; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        const CmHirDeclarationNamespaceEntry *entry = find_namespace_entry(
            metadata, expected[index].owner_module,
            CM_HIR_DECL_NAMESPACE_TYPE,
            expected[index].name);
        assert(entry != NULL
            && entry->target_kind == CM_HIR_DECL_TARGET_PRIMITIVE
            && entry->target_local == expected[index].primitive);
    }
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        if (entry->target_kind == CM_HIR_DECL_TARGET_PRIMITIVE) {
            assert(entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
                && entry->target_local >= CM_HIR_DECL_PRIMITIVE_BOOL
                && entry->target_local <= CM_HIR_DECL_PRIMITIVE_F64);
            primitive_count += 1u;
        }
    }
    assert(primitive_count == sizeof(expected) / sizeof(expected[0])
        && find_namespace_entry(metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "octet") == NULL
        && find_namespace_entry(metadata, 3u,
            CM_HIR_DECL_NAMESPACE_VALUE, "byte") == NULL);
}

static void test_primitive_reexports_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    primitive_reexport_fixture_init(&first, 0);
    primitive_reexport_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 3u
        && result.module_count == 3u);
    assert_primitive_reexport_descriptor(&first_metadata);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 3u);
    assert_primitive_reexport_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_primitive_reexport_hostile_mutations_are_atomic(void)
{
    static const struct {
        const char *path;
        const unsigned char *source;
        size_t source_length;
    } rejected[] = {
        { "primitive-f16.rs",
            (const unsigned char *)
                "#[stable(feature = \"p\", since = \"1.0.0\")]\n"
                "pub use f16;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("#[stable(feature = \"p\", since = \"1.0.0\")]\n"
                "pub use f16;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u },
        { "primitive-mixed.rs",
            (const unsigned char *)
                "pub trait Gate<T: ?Sized> {}\n"
                "#[stable(feature = \"p\", since = \"1.0.0\")]\n"
                "pub use {bool, Gate as OtherGate};\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("pub trait Gate<T: ?Sized> {}\n"
                "#[stable(feature = \"p\", since = \"1.0.0\")]\n"
                "pub use {bool, Gate as OtherGate};\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u },
        { "primitive-glob.rs",
            (const unsigned char *)
                "mod p { pub use bool; }\n"
                "#[stable(feature = \"p\", since = \"1.0.0\")]\n"
                "pub use p::*;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("mod p { pub use bool; }\n"
                "#[stable(feature = \"p\", since = \"1.0.0\")]\n"
                "pub use p::*;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u },
        { "primitive-attr.rs",
            (const unsigned char *)
                "#[doc(notable_trait)]\n"
                "pub use bool;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n",
            sizeof("#[doc(notable_trait)]\n"
                "pub use bool;\n"
                "pub trait Gate<T: ?Sized> {}\n"
                "pub fn needs<X: Gate<u8>>() {}\n") - 1u }
    };
    CaptureFixture good;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirDeclarationType *saved_types;
    CmHirImport *grouped = NULL;
    CmHirImport *alias = NULL;
    CmHirImportBinding *bool_binding = NULL;
    CmHirPrimitiveKind saved_primitive;
    CmHirNamespace saved_namespace_kind;
    CmHirDefId saved_target;
    CmInternId saved_tree;
    CmInternId saved_metadata;
    CmSpan saved_span;
    uint32_t saved_source_item;
    size_t index;
    primitive_reexport_fixture_init(&good, 0);
    input = capture_input(&good);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_namespace = metadata.namespace_entries;
    saved_types = metadata.types;
    for (index = 0u; index < good.hir.modules.len; ++index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(&good.hir.modules,
            index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != good.lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *candidate = &module->imports[import_index];
            if (candidate->binding_count == 17u) grouped = candidate;
            if (candidate->binding_count == 1u
                && candidate->bindings != NULL
                && candidate->bindings[0].primitive_kind
                    == CM_HIR_PRIMITIVE_U8) alias = candidate;
        }
    }
    assert(grouped != NULL && alias != NULL && grouped != alias
        && grouped->bindings != NULL && grouped->attribute_count == 1u
        && grouped->attributes != NULL);
    for (index = 0u; index < grouped->binding_count; ++index)
        if (grouped->bindings[index].primitive_kind
                == CM_HIR_PRIMITIVE_BOOL)
            bool_binding = &grouped->bindings[index];
    assert(bool_binding != NULL);

#define ASSERT_PRIMITIVE_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.namespace_entries == saved_namespace \
        && metadata.types == saved_types); \
} while (0)

    saved_primitive = alias->bindings[0].primitive_kind;
    alias->bindings[0].primitive_kind = CM_HIR_PRIMITIVE_CHAR;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    alias->bindings[0].primitive_kind = saved_primitive;

    saved_namespace_kind = bool_binding->namespace_kind;
    bool_binding->namespace_kind = CM_HIR_NAMESPACE_VALUE;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    bool_binding->namespace_kind = saved_namespace_kind;

    saved_target = bool_binding->target;
    bool_binding->target.crate_id = good.lower_result.crate_id;
    bool_binding->target.index = 1u;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    bool_binding->target = saved_target;

    saved_tree = grouped->tree;
    grouped->tree = alias->tree;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    grouped->tree = saved_tree;

    saved_source_item = grouped->source_item;
    grouped->source_item = alias->source_item;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    grouped->source_item = saved_source_item;

    saved_span = grouped->span;
    grouped->span.start += 1u;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    grouped->span = saved_span;

    saved_metadata = grouped->attributes[0].metadata;
    grouped->attributes[0].metadata = cm_hir_intern(&good.hir,
        "doc(hidden)");
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    grouped->attributes[0].metadata = saved_metadata;

    grouped->attributes[0].expansion_depth = 1u;
    ASSERT_PRIMITIVE_ATOMIC_FAILURE();
    grouped->attributes[0].expansion_depth = 0u;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]); ++index) {
        CaptureFixture bad;
        CmHirDeclarationCaptureInput bad_input;
        fixture_init_source(&bad, 0, rejected[index].path,
            rejected[index].source, rejected[index].source_length);
        bad_input = capture_input(&bad);
        result = cm_hir_declaration_metadata_capture(&bad_input, &metadata);
        assert(result.status != CM_HIR_DECL_CAPTURE_OK
            && metadata.namespace_entries == saved_namespace
            && metadata.types == saved_types);
        fixture_destroy(&bad);
    }
    assert_primitive_reexport_descriptor(&metadata);
#undef ASSERT_PRIMITIVE_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void test_allocator_like_associated_method_capture(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture plain;
    CaptureFixture rejected;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationTrait *allocator = NULL;
    CmHirDeclarationAssociatedItem *saved_associated;
    CmHirDeclarationTrait *saved_traits;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId trait_id;
    CmHirItemId method_id;
    CmHirItem *trait_item;
    CmHirItem *method;
    CmHirItem *by_ref_method;
    CmInternId saved_metadata;
    uint32_t saved_source_attribute;
    uint32_t saved_expansion_depth;
    CmSpan saved_attribute_span;
    CmHirDefId saved_parent;
    CmHirSafety saved_safety;
    int saved_default;
    CmHirBodyId saved_body;
    CmHirReceiverKind saved_receiver;
    CmHirType *inferred_return;
    size_t saved_hir_item_count;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;
    CmByteBuf plain_bytes;
    size_t index;
    allocator_like_fixture_init(&first, 0);
    allocator_like_fixture_init(&noisy, 1);
    allocator_like_fixture_init_hints(&plain, "", "");
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    cm_hir_declaration_metadata_init(&plain_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.trait_count == 2u && result.associated_count == 7u
        && result.projected_semantic_attribute_count == 10u
        && metadata.associated_count == 7u);
    for (index = 0u; index < metadata.trait_count; ++index) {
        if (declaration_string_is(metadata.traits[index].name,
                "AllocatorLike")) allocator = &metadata.traits[index];
    }
    assert(allocator != NULL
        && allocator->safety == CM_HIR_DECL_SAFETY_UNSAFE
        && allocator->associated_start == 1u
        && allocator->associated_count == 7u);
    for (index = 0u; index < metadata.associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata.associated_items[index];
        assert(associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD
            && associated->parent_kind
                == CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL
            && associated->parent_local == 1u
            && associated->visibility.kind
                == CM_HIR_DECL_VISIBILITY_PRIVATE
            && associated->source_ordinal == index
            && associated->receiver == CM_HIR_DECL_RECEIVER_REF_SHARED
            && associated->parameter_count >= 1u
            && associated->parameter_types != NULL
            && associated->return_type != 0u
            && declaration_string_is(associated->abi, "Rust")
            && associated->is_const == 0u && associated->is_async == 0u
            && associated->is_variadic == 0u);
    }
    assert(declaration_string_is(metadata.associated_items[0].name,
            "allocate")
        && metadata.associated_items[0].has_default_body == 0u
        && declaration_string_is(metadata.associated_items[1].name,
            "allocate_zeroed")
        && metadata.associated_items[1].has_default_body == 1u
        && metadata.associated_items[2].safety
            == CM_HIR_DECL_SAFETY_UNSAFE
        && declaration_string_is(metadata.associated_items[6].name,
            "by_ref")
        && metadata.associated_items[6].predicate_count == 1u
        && metadata.predicate_count == 2u
        && metadata.predicates[1].owner_kind
            == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
        && metadata.predicates[1].owner_value == 0u
        && metadata.predicates[1].owner_associated == 7u
        && metadata.predicates[1].argument_count == 0u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.associated_count == 7u
        && result.projected_semantic_attribute_count == 10u);
    input = capture_input(&plain);
    result = cm_hir_declaration_metadata_capture(&input, &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.associated_count == 7u
        && result.projected_semantic_attribute_count == 9u);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    cm_byte_buf_init(&plain_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && bytes.len == plain_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0
        && memcmp(bytes.data, plain_bytes.data, bytes.len) == 0);
    {
        static const char *const hints[] = {
            "  #[inline]\n", "  #[inline(never)]\n"
        };
        size_t hint_index;
        for (hint_index = 0u;
                hint_index < sizeof(hints) / sizeof(hints[0]); ++hint_index) {
            CaptureFixture variant;
            CmHirDeclarationMetadata variant_metadata;
            CmByteBuf variant_bytes;
            allocator_like_fixture_init_hints(&variant, "",
                hints[hint_index]);
            cm_hir_declaration_metadata_init(&variant_metadata);
            input = capture_input(&variant);
            result = cm_hir_declaration_metadata_capture(&input,
                &variant_metadata);
            assert(result.status == CM_HIR_DECL_CAPTURE_OK
                && result.projected_semantic_attribute_count == 10u);
            cm_byte_buf_init(&variant_bytes);
            assert(cm_hir_declaration_metadata_encode(&variant_metadata,
                        &variant_bytes) == CM_HIR_DECL_METADATA_OK
                && variant_bytes.len == bytes.len
                && memcmp(variant_bytes.data, bytes.data, bytes.len) == 0);
            cm_byte_buf_destroy(&variant_bytes);
            cm_hir_declaration_metadata_destroy(&variant_metadata);
            fixture_destroy(&variant);
        }
    }
    cm_byte_buf_destroy(&plain_bytes);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);

    saved_associated = metadata.associated_items;
    saved_traits = metadata.traits;
    saved_namespace = metadata.namespace_entries;
    trait_item = (CmHirItem *)find_item(&first, "AllocatorLike", &trait_id);
    method = (CmHirItem *)find_item(&first, "allocate_zeroed", &method_id);
    by_ref_method = (CmHirItem *)find_item(&first, "by_ref", &method_id);
    assert(trait_item != NULL && method != NULL && by_ref_method != NULL
        && method->attribute_count == 1u && method->attributes != NULL
        && by_ref_method->attribute_count == 2u
        && by_ref_method->attributes != NULL);
#define ASSERT_ATOMIC_TRAIT_FAILURE(reason_) do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR \
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS \
        && result.failure_reason == (reason_) \
        && metadata.associated_items == saved_associated \
        && metadata.traits == saved_traits \
        && metadata.namespace_entries == saved_namespace); \
} while (0)
    input = capture_input(&first);
    saved_safety = trait_item->data.trait_item.safety;
    trait_item->data.trait_item.safety = CM_HIR_SAFE;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    trait_item->data.trait_item.safety = saved_safety;

    saved_parent = method->parent_definition;
    method->parent_definition = cm_hir_def_id_none();
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    method->parent_definition = saved_parent;

    saved_safety = method->data.function_item.signature.safety;
    method->data.function_item.signature.safety = CM_HIR_UNSAFE;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    method->data.function_item.signature.safety = saved_safety;

    saved_default = method->data.function_item.has_default_body;
    method->data.function_item.has_default_body = !saved_default;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    method->data.function_item.has_default_body = saved_default;

    saved_body = method->data.function_item.body;
    assert(saved_body != CM_HIR_BODY_NONE);
    method->data.function_item.body = CM_HIR_BODY_NONE;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    method->data.function_item.body = saved_body;

    saved_metadata = method->attributes[0].metadata;
    method->attributes[0].metadata = cm_hir_intern(&first.hir,
        "doc(notable_trait)");
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    method->attributes[0].metadata = saved_metadata;

    saved_receiver = method->data.function_item.signature.receiver;
    method->data.function_item.signature.receiver =
        CM_HIR_RECEIVER_REF_MUTABLE;
    ASSERT_ATOMIC_TRAIT_FAILURE(
        CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED);
    method->data.function_item.signature.receiver = saved_receiver;

    saved_metadata = by_ref_method->attributes[1].metadata;
    by_ref_method->attributes[1].metadata = cm_hir_intern(&first.hir,
        "inline(never)");
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    by_ref_method->attributes[1].metadata = saved_metadata;

    saved_expansion_depth = by_ref_method->attributes[1].expansion_depth;
    by_ref_method->attributes[1].expansion_depth = 1u;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    by_ref_method->attributes[1].expansion_depth = saved_expansion_depth;

    saved_source_attribute = by_ref_method->attributes[1].source_attribute;
    assert(saved_source_attribute != UINT32_MAX);
    by_ref_method->attributes[1].source_attribute =
        saved_source_attribute + 1u;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    by_ref_method->attributes[1].source_attribute = saved_source_attribute;

    saved_attribute_span = by_ref_method->attributes[1].span;
    by_ref_method->attributes[1].span.start += 1u;
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    by_ref_method->attributes[1].span = saved_attribute_span;

    allocator_like_fixture_init_hints(&rejected, "",
        "  #[inline(sometimes)]\n");
    input = capture_input(&rejected);
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    fixture_destroy(&rejected);

    allocator_like_fixture_init_hints(&rejected, "",
        "  #[inline]\n  #[inline(never)]\n");
    input = capture_input(&rejected);
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    fixture_destroy(&rejected);

    allocator_like_fixture_init_hints(&rejected, "#[inline]\n", "");
    input = capture_input(&rejected);
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    fixture_destroy(&rejected);

    allocator_like_fixture_init_hints(&rejected, "",
        "  #[cfg_attr(all(), inline(always))]\n");
    input = capture_input(&rejected);
    ASSERT_ATOMIC_TRAIT_FAILURE(CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID);
    fixture_destroy(&rejected);

    input = capture_input(&first);
#undef ASSERT_ATOMIC_TRAIT_FAILURE

    fixture_init_source(&rejected, 0, "inline-free-function.rs",
        inline_free_function_fixture_source,
        sizeof(inline_free_function_fixture_source) - 1u);
    input = capture_input(&rejected);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID
        && metadata.associated_items == saved_associated
        && metadata.traits == saved_traits
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&rejected);

    saved_hir_item_count = first.hir.items.len;
    first.hir.items.len = CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS + 1u;
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED
        && metadata.associated_items == saved_associated
        && metadata.traits == saved_traits
        && metadata.namespace_entries == saved_namespace);
    first.hir.items.len = saved_hir_item_count;

    fixture_init_source(&rejected, 0, "associated-inferred-return.rs",
        inferred_associated_return_fixture_source,
        sizeof(inferred_associated_return_fixture_source) - 1u);
    method = (CmHirItem *)find_item(&rejected, "by_ref", &method_id);
    assert(method != NULL);
    inferred_return = (CmHirType *)cm_hir_get_type(&rejected.hir,
        method->data.function_item.signature.return_type);
    assert(inferred_return != NULL
        && inferred_return->kind == CM_HIR_TYPE_REFERENCE_KIND);
    inferred_return->data.reference_type.region.kind = CM_HIR_REGION_INFER;
    input = capture_input(&rejected);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID
        && metadata.associated_items == saved_associated
        && metadata.traits == saved_traits
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&rejected);

    fixture_init_source(&rejected, 0, "associated-safe-trait.rs",
        safe_associated_trait_fixture_source,
        sizeof(safe_associated_trait_fixture_source) - 1u);
    input = capture_input(&rejected);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED
        && metadata.associated_items == saved_associated
        && metadata.traits == saved_traits
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&rejected);

    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&plain);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_any_like_safe_static_trait_capture(void)
{
    static const unsigned char missing_static_source[] =
        "#[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
        "#[rustc_diagnostic_item = \"AnyLike\"]\n"
        "pub trait AnyLike {\n"
        "  #[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
        "  fn type_id(&self) -> TypeIdLike;\n"
        "}\n"
        "pub struct TypeIdLike;\n"
        "pub trait Marker {}\n"
        "pub fn needs<X: Marker>() {}\n";
    static const unsigned char duplicate_diagnostic_source[] =
        "#[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
        "#[rustc_diagnostic_item = \"AnyLike\"]\n"
        "#[rustc_diagnostic_item = \"OtherAnyLike\"]\n"
        "pub trait AnyLike: 'static {\n"
        "  #[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
        "  fn type_id(&self) -> TypeIdLike;\n"
        "}\n"
        "pub struct TypeIdLike;\n"
        "pub trait Marker {}\n"
        "pub fn needs<X: Marker>() {}\n";
    static const unsigned char unstable_trait_source[] =
        "#[unstable(feature = \"type_id_like\", issue = \"none\")]\n"
        "#[rustc_diagnostic_item = \"AnyLike\"]\n"
        "pub trait AnyLike: 'static {\n"
        "  #[stable(feature = \"type_id_like\", since = \"1.0.0\")]\n"
        "  fn type_id(&self) -> TypeIdLike;\n"
        "}\n"
        "pub struct TypeIdLike;\n"
        "pub trait Marker {}\n"
        "pub fn needs<X: Marker>() {}\n";
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture rejected;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;
    const CmHirDeclarationTrait *any_wire = NULL;
    const CmHirDeclarationAssociatedItem *method_wire = NULL;
    const CmHirDeclarationOutlivesPredicate *outlives;
    const CmHirDeclarationType *subject;
    const CmHirDeclarationType *return_type;
    CmHirDeclarationTrait *saved_traits;
    CmHirDeclarationAssociatedItem *saved_associated;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItem *trait_item;
    CmHirItem *method_item;
    const CmHirItem *other_item;
    CmHirItemId trait_id;
    CmHirItemId method_id;
    CmHirItemId other_id;
    CmHirOutlivesPredicate *hir_outlives;
    CmHirFunctionSignature *signature;
    CmHirType *receiver;
    CmHirType *method_return;
    CmHirDefId saved_definition;
    CmInternId saved_metadata;
    CmSpan saved_span;
    uint32_t saved_source_attribute;
    CmHirRegion saved_bound;
    CmHirTypeId saved_subject;
    CmHirSafety saved_safety;
    CmHirMutability saved_mutability;
    size_t index;
    any_like_fixture_init(&first, 0);
    any_like_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.trait_count == 2u && result.associated_count == 1u
        && result.item_count == 2u && result.value_count == 1u
        && result.projected_semantic_attribute_count == 2u
        && metadata.outlives_predicate_count == 1u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    for (index = 0u; index < metadata.trait_count; ++index)
        if (declaration_string_is(metadata.traits[index].name, "AnyLike"))
            any_wire = &metadata.traits[index];
    for (index = 0u; index < metadata.associated_count; ++index)
        if (declaration_string_is(metadata.associated_items[index].name,
                "type_id")) method_wire = &metadata.associated_items[index];
    assert(any_wire != NULL && method_wire != NULL
        && any_wire->safety == CM_HIR_DECL_SAFETY_SAFE
        && any_wire->associated_count == 1u
        && any_wire->outlives_count == 1u
        && any_wire->predicate_scope_count == 0u
        && any_wire->predicate_count == 0u
        && any_wire->flags == CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM
        && declaration_string_is(any_wire->diagnostic_item, "AnyLike")
        && method_wire->parent_local
            == (uint32_t)(any_wire - metadata.traits + 1)
        && method_wire->receiver == CM_HIR_DECL_RECEIVER_REF_SHARED
        && method_wire->parameter_count == 1u
        && method_wire->safety == CM_HIR_DECL_SAFETY_SAFE
        && method_wire->has_default_body == 0u
        && method_wire->predicate_count == 0u);
    outlives = &metadata.outlives_predicates[0];
    subject = &metadata.types[outlives->subject_type - 1u];
    return_type = &metadata.types[method_wire->return_type - 1u];
    assert(outlives->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
        && outlives->owner_local == method_wire->parent_local
        && outlives->ordinal == 0u && outlives->scope == 0u
        && outlives->bound.kind == CM_HIR_DECL_REGION_STATIC
        && subject->kind == CM_HIR_DECL_TYPE_SELF
        && subject->self_trait_local == method_wire->parent_local
        && return_type->kind == CM_HIR_DECL_TYPE_NAMED_ADT
        && declaration_string_is(metadata.items[
            return_type->item_local - 1u].name, "TypeIdLike")
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);

    saved_traits = metadata.traits;
    saved_associated = metadata.associated_items;
    saved_namespace = metadata.namespace_entries;
    trait_item = (CmHirItem *)find_item(&first, "AnyLike", &trait_id);
    method_item = (CmHirItem *)find_item(&first, "type_id", &method_id);
    assert(trait_item != NULL && method_item != NULL
        && trait_item->outlives_predicate_count == 1u
        && trait_item->outlives_predicates != NULL
        && trait_item->attribute_count == 2u
        && method_item->attribute_count == 1u);
    hir_outlives = &trait_item->outlives_predicates[0];
    signature = &method_item->data.function_item.signature;
    receiver = (CmHirType *)cm_hir_get_type(&first.hir,
        signature->parameters[0].type);
    method_return = (CmHirType *)cm_hir_get_type(&first.hir,
        signature->return_type);
    other_item = find_item(&first, "OtherId", &other_id);
    assert(receiver != NULL && receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        && method_return != NULL && method_return->kind == CM_HIR_TYPE_ADT_KIND
        && other_item != NULL);
#define ASSERT_ANY_ATOMIC_FAILURE() do { \
    input = capture_input(&first); \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.traits == saved_traits \
        && metadata.associated_items == saved_associated \
        && metadata.namespace_entries == saved_namespace); \
} while (0)
    saved_metadata = trait_item->attributes[1].metadata;
    trait_item->attributes[1].metadata = cm_hir_intern(&first.hir,
        "rustc_diagnostic_item = \"ForgedAnyLike\"");
    ASSERT_ANY_ATOMIC_FAILURE();
    trait_item->attributes[1].metadata = saved_metadata;

    saved_span = trait_item->attributes[1].span;
    trait_item->attributes[1].span.start += 1u;
    ASSERT_ANY_ATOMIC_FAILURE();
    trait_item->attributes[1].span = saved_span;

    saved_source_attribute = trait_item->attributes[1].source_attribute;
    trait_item->attributes[1].source_attribute += 1u;
    ASSERT_ANY_ATOMIC_FAILURE();
    trait_item->attributes[1].source_attribute = saved_source_attribute;

    trait_item->attributes[1].expansion_depth = 1u;
    ASSERT_ANY_ATOMIC_FAILURE();
    trait_item->attributes[1].expansion_depth = 0u;

    saved_bound = hir_outlives->bound;
    hir_outlives->bound.kind = CM_HIR_REGION_ERASED;
    ASSERT_ANY_ATOMIC_FAILURE();
    hir_outlives->bound = saved_bound;

    saved_subject = hir_outlives->subject.type;
    hir_outlives->subject.type = signature->return_type;
    ASSERT_ANY_ATOMIC_FAILURE();
    hir_outlives->subject.type = saved_subject;

    saved_span = hir_outlives->span;
    hir_outlives->span.start += 1u;
    ASSERT_ANY_ATOMIC_FAILURE();
    hir_outlives->span = saved_span;

    saved_safety = trait_item->data.trait_item.safety;
    trait_item->data.trait_item.safety = CM_HIR_UNSAFE;
    ASSERT_ANY_ATOMIC_FAILURE();
    trait_item->data.trait_item.safety = saved_safety;

    saved_mutability = receiver->data.reference_type.mutability;
    receiver->data.reference_type.mutability = CM_HIR_MUTABLE;
    ASSERT_ANY_ATOMIC_FAILURE();
    receiver->data.reference_type.mutability = saved_mutability;

    saved_definition = method_return->data.named_type.definition;
    method_return->data.named_type.definition = other_item->definition;
    ASSERT_ANY_ATOMIC_FAILURE();
    method_return->data.named_type.definition = saved_definition;
#undef ASSERT_ANY_ATOMIC_FAILURE

    fixture_init_source(&rejected, 0, "any-like-missing-static.rs",
        missing_static_source, sizeof(missing_static_source) - 1u);
    input = capture_input(&rejected);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && metadata.traits == saved_traits
        && metadata.associated_items == saved_associated
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&rejected);

    fixture_init_source(&rejected, 0, "any-like-duplicate-diagnostic.rs",
        duplicate_diagnostic_source,
        sizeof(duplicate_diagnostic_source) - 1u);
    input = capture_input(&rejected);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && metadata.traits == saved_traits
        && metadata.associated_items == saved_associated
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&rejected);

    fixture_init_source(&rejected, 0, "any-like-unstable-trait.rs",
        unstable_trait_source, sizeof(unstable_trait_source) - 1u);
    input = capture_input(&rejected);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && metadata.traits == saved_traits
        && metadata.associated_items == saved_associated
        && metadata.namespace_entries == saved_namespace);
    fixture_destroy(&rejected);

    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_associated_composite_type_capture(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationAssociatedItem *outcome_wire = NULL;
    CmHirDeclarationAssociatedItem *raw_mut_wire = NULL;
    CmHirDeclarationAssociatedItem *saved_associated;
    CmHirDeclarationType *saved_types;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId outcome_method_id;
    CmHirItemId raw_mut_method_id;
    CmHirItemId outcome_item_id;
    CmHirItem *outcome_method;
    CmHirItem *raw_mut_method;
    CmHirItem *outcome_item;
    CmHirItem *shadow_outcome = NULL;
    const CmHirModule *shadow_module;
    CmHirType *outer_application;
    CmHirType *wrap_application;
    CmHirType *slice;
    CmHirType *const_pointer;
    CmHirType *mut_pointer;
    CmHirDefId saved_definition;
    uint32_t saved_argument_count;
    CmHirTypeId saved_type_id;
    CmHirMutability saved_mutability;
    uint32_t saved_span_start;
    CmByteBuf bytes;
    CmByteBuf noisy_bytes;
    size_t index;
    size_t slice_count = 0u;
    size_t pointer_count = 0u;
    size_t application_count = 0u;
    int saw_const_pointer = 0;
    int saw_mut_pointer = 0;
    composite_associated_fixture_init(&first, 0);
    composite_associated_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.associated_count == 2u
        && metadata.associated_count == 2u);
    for (index = 0u; index < metadata.type_count; ++index) {
        const CmHirDeclarationType *type = &metadata.types[index];
        if (type->kind == CM_HIR_DECL_TYPE_SLICE) slice_count += 1u;
        else if (type->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
            pointer_count += 1u;
            if (type->mutability == CM_HIR_DECL_IMMUTABLE)
                saw_const_pointer = 1;
            if (type->mutability == CM_HIR_DECL_MUTABLE)
                saw_mut_pointer = 1;
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            application_count += 1u;
        }
    }
    assert(slice_count == 1u && pointer_count == 2u
        && application_count == 2u && saw_const_pointer && saw_mut_pointer
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    for (index = 0u; index < metadata.associated_count; ++index) {
        if (declaration_string_is(metadata.associated_items[index].name,
                "outcome")) outcome_wire = &metadata.associated_items[index];
        if (declaration_string_is(metadata.associated_items[index].name,
                "raw_mut")) raw_mut_wire = &metadata.associated_items[index];
    }
    assert(outcome_wire != NULL && raw_mut_wire != NULL
        && outcome_wire->parameter_count == 2u
        && metadata.types[outcome_wire->parameter_types[1] - 1u].kind
            == CM_HIR_DECL_TYPE_RAW_POINTER
        && metadata.types[outcome_wire->parameter_types[1] - 1u].mutability
            == CM_HIR_DECL_IMMUTABLE
        && metadata.types[raw_mut_wire->return_type - 1u].kind
            == CM_HIR_DECL_TYPE_RAW_POINTER
        && metadata.types[raw_mut_wire->return_type - 1u].mutability
            == CM_HIR_DECL_MUTABLE
        && metadata.types[outcome_wire->return_type - 1u].kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
        && metadata.types[outcome_wire->return_type - 1u].argument_count
            == 2u);
    {
        const CmHirDeclarationType *outer =
            &metadata.types[outcome_wire->return_type - 1u];
        const CmHirDeclarationType *wrap =
            &metadata.types[outer->argument_types[0] - 1u];
        const CmHirDeclarationType *error =
            &metadata.types[outer->argument_types[1] - 1u];
        const CmHirDeclarationType *slice_type;
        const CmHirDeclarationType *const_type =
            &metadata.types[outcome_wire->parameter_types[1] - 1u];
        const CmHirDeclarationType *mut_type =
            &metadata.types[raw_mut_wire->return_type - 1u];
        assert(wrap->kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION
            && wrap->argument_count == 1u
            && error->kind == CM_HIR_DECL_TYPE_NAMED_ADT
            && wrap->argument_types[0] < outer->argument_types[0]
            && outer->argument_types[0] < outcome_wire->return_type
            && outer->argument_types[1] < outcome_wire->return_type);
        slice_type = &metadata.types[wrap->argument_types[0] - 1u];
        assert(slice_type->kind == CM_HIR_DECL_TYPE_SLICE
            && slice_type->child_type < wrap->argument_types[0]
            && const_type->child_type == slice_type->child_type
            && mut_type->child_type == slice_type->child_type
            && metadata.types[slice_type->child_type - 1u].kind
                == CM_HIR_DECL_TYPE_PRIMITIVE
            && metadata.types[slice_type->child_type - 1u].primitive
                == CM_HIR_DECL_PRIMITIVE_U8
            && declaration_string_is(metadata.items[outer->item_local - 1u]
                    .name, "Outcome")
            && declaration_string_is(metadata.items[wrap->item_local - 1u]
                    .name, "Wrap")
            && declaration_string_is(metadata.items[error->item_local - 1u]
                    .name, "Error"));
    }
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.associated_count == 2u);
    cm_byte_buf_init(&bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&metadata, &bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && bytes.len == noisy_bytes.len
        && memcmp(bytes.data, noisy_bytes.data, bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&bytes);

    saved_associated = metadata.associated_items;
    saved_types = metadata.types;
    saved_namespace = metadata.namespace_entries;
    outcome_method = (CmHirItem *)find_item(&first, "outcome",
        &outcome_method_id);
    raw_mut_method = (CmHirItem *)find_item(&first, "raw_mut",
        &raw_mut_method_id);
    outcome_item = (CmHirItem *)find_item(&first, "Outcome",
        &outcome_item_id);
    shadow_module = find_module(&first, "shadow");
    for (index = 0u; index < first.hir.items.len; ++index) {
        CmHirItem *candidate = (CmHirItem *)cm_vec_at(&first.hir.items,
            index);
        const CmInternedString *name = candidate == NULL ? NULL
            : cm_interner_get(&first.hir.strings, candidate->name);
        if (candidate != NULL && shadow_module != NULL
            && cm_hir_get_module(&first.hir, candidate->owner_module)
                == shadow_module
            && name != NULL && name->len == 7u
            && memcmp(name->bytes, "Outcome", 7u) == 0)
            shadow_outcome = candidate;
    }
    assert(outcome_method != NULL && raw_mut_method != NULL
        && outcome_item != NULL && shadow_outcome != NULL
        && outcome_method->data.function_item.signature.parameter_count == 2u
        && outcome_item->data.enum_item.variant_count == 2u);
    outer_application = (CmHirType *)cm_hir_get_type(&first.hir,
        outcome_method->data.function_item.signature.return_type);
    const_pointer = (CmHirType *)cm_hir_get_type(&first.hir,
        outcome_method->data.function_item.signature.parameters[1].type);
    mut_pointer = (CmHirType *)cm_hir_get_type(&first.hir,
        raw_mut_method->data.function_item.signature.return_type);
    assert(outer_application != NULL
        && outer_application->kind == CM_HIR_TYPE_ADT_KIND
        && outer_application->data.named_type.argument_count == 2u
        && const_pointer != NULL
        && const_pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && mut_pointer != NULL
        && mut_pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND);
    wrap_application = (CmHirType *)cm_hir_get_type(&first.hir,
        outer_application->data.named_type.arguments[0].data.type);
    assert(wrap_application != NULL
        && wrap_application->kind == CM_HIR_TYPE_ADT_KIND
        && wrap_application->data.named_type.argument_count == 1u);
    slice = (CmHirType *)cm_hir_get_type(&first.hir,
        wrap_application->data.named_type.arguments[0].data.type);
    assert(slice != NULL && slice->kind == CM_HIR_TYPE_SLICE_KIND);
    input = capture_input(&first);
#define ASSERT_COMPOSITE_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.associated_items == saved_associated \
        && metadata.types == saved_types \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_definition = outer_application->data.named_type.definition;
    outer_application->data.named_type.definition =
        wrap_application->data.named_type.definition;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    outer_application->data.named_type.definition = saved_definition;

    outer_application->data.named_type.definition =
        shadow_outcome->definition;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    outer_application->data.named_type.definition = saved_definition;

    saved_argument_count = outer_application->data.named_type.argument_count;
    outer_application->data.named_type.argument_count = 1u;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    outer_application->data.named_type.argument_count = saved_argument_count;

    saved_type_id = slice->data.slice_type.element;
    slice->data.slice_type.element =
        outer_application->data.named_type.arguments[1].data.type;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    slice->data.slice_type.element = saved_type_id;

    slice->data.slice_type.element =
        wrap_application->data.named_type.arguments[0].data.type;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    slice->data.slice_type.element = saved_type_id;

    saved_type_id = const_pointer->data.raw_pointer_type.pointee;
    const_pointer->data.raw_pointer_type.pointee =
        (CmHirTypeId)(first.hir.types.len + 1u);
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    const_pointer->data.raw_pointer_type.pointee = saved_type_id;

    saved_mutability = const_pointer->data.raw_pointer_type.mutability;
    const_pointer->data.raw_pointer_type.mutability = CM_HIR_MUTABLE;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    const_pointer->data.raw_pointer_type.mutability = saved_mutability;

    saved_mutability = mut_pointer->data.raw_pointer_type.mutability;
    mut_pointer->data.raw_pointer_type.mutability = CM_HIR_IMMUTABLE;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    mut_pointer->data.raw_pointer_type.mutability = saved_mutability;

    saved_type_id = wrap_application->data.named_type.arguments[0].data.type;
    wrap_application->data.named_type.arguments[0].data.type =
        outcome_item->data.enum_item.variants[0].fields[0].type;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    wrap_application->data.named_type.arguments[0].data.type = saved_type_id;

    saved_span_start = outer_application->span.start;
    outer_application->span.start += 1u;
    ASSERT_COMPOSITE_ATOMIC_FAILURE();
    outer_application->span.start = saved_span_start;

    saved_argument_count = outer_application->data.named_type.argument_count;
    outer_application->data.named_type.argument_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + UINT32_C(1);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata.associated_items == saved_associated
        && metadata.types == saved_types
        && metadata.namespace_entries == saved_namespace);
    outer_application->data.named_type.argument_count = saved_argument_count;
#undef ASSERT_COMPOSITE_ATOMIC_FAILURE

    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_reachable_canonical_type_caps(void)
{
    CaptureFixture private_fixture;
    CaptureFixture repeated_fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId item_id;
    CmHirItem *item;
    CmHirType *type;
    uint32_t saved_count;
    CmModuleId root_module;
    CmResolveEffectiveItem effective;
    const CmAst *borrowed_ast;
    CmAstItem *ast_item;
    CmAstType *ast_array;
    CmAstType *ast_outer;
    const CmAstType *ast_inner;
    CmAstTypeId ast_inner_id;
    CmAstTypeId ast_primitive_id;
    CmAstTypeId *saved_ast_elements;
    CmAstTypeId *ast_elements;
    CmHirType *hir_array;
    CmHirType *hir_outer;
    const CmHirType *hir_inner;
    CmHirTypeId hir_outer_id;
    CmHirTypeId hir_inner_id;
    CmHirTypeId hir_primitive_id;
    CmHirType duplicate_template;
    CmHirTypeId *saved_hir_elements;
    CmHirTypeId *hir_elements;
    CmHirDeclarationType *saved_types;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationAssociatedItem *saved_associated;
    size_t duplicate_count =
        CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES / 2u + 1u;
    size_t index;
    size_t tuple_count;
    size_t slice_count;
    size_t raw_pointer_count;
    size_t reference_count;

    fixture_init_source(&private_fixture, 0, "unreachable-private.rs",
        unreachable_private_type_fixture_source,
        sizeof(unreachable_private_type_fixture_source) - 1u);
    item = (CmHirItem *)find_item(&private_fixture, "hidden", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && item->data.function_item.signature.parameter_count == 1u);
    type = (CmHirType *)cm_hir_get_type(&private_fixture.hir,
        item->data.function_item.signature.parameters[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count == 2u);
    saved_count = type->data.tuple_type.element_count;
    type->data.tuple_type.element_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES + UINT32_C(1);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&private_fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    type->data.tuple_type.element_count = saved_count;
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&private_fixture);

    fixture_init_source(&repeated_fixture, 0, "repeated-composites.rs",
        repeated_composite_fixture_source,
        sizeof(repeated_composite_fixture_source) - 1u);
    item = (CmHirItem *)find_item(&repeated_fixture, "MANY", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_STATIC);
    hir_array = (CmHirType *)cm_hir_get_type(&repeated_fixture.hir,
        item->data.value_item.type);
    assert(hir_array != NULL && hir_array->kind == CM_HIR_TYPE_ARRAY_KIND);
    hir_outer_id = hir_array->data.array_type.element;
    hir_outer = (CmHirType *)cm_hir_get_type(&repeated_fixture.hir,
        hir_outer_id);
    assert(hir_outer != NULL && hir_outer->kind == CM_HIR_TYPE_TUPLE_KIND
        && hir_outer->data.tuple_type.element_count == 2u);
    hir_inner_id = hir_outer->data.tuple_type.elements[0];
    hir_inner = cm_hir_get_type(&repeated_fixture.hir, hir_inner_id);
    assert(hir_inner != NULL && hir_inner->kind == CM_HIR_TYPE_TUPLE_KIND
        && hir_inner->data.tuple_type.element_count == 1u);
    hir_primitive_id = hir_inner->data.tuple_type.elements[0];
    duplicate_template = *hir_inner;
    assert(cm_module_graph_get_root(&repeated_fixture.graph, &root_module)
        && cm_module_graph_get_effective_item(&repeated_fixture.graph,
            repeated_fixture.graph_result.revision, root_module, 0u,
            &effective) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_borrow_ast(&repeated_fixture.graph, root_module,
            &borrowed_ast));
    ast_item = (CmAstItem *)(void *)cm_ast_get_item(borrowed_ast,
        effective.declaration.item);
    assert(ast_item != NULL && ast_item->kind == CM_AST_ITEM_STATIC);
    ast_array = (CmAstType *)(void *)cm_ast_get_type(borrowed_ast,
        ast_item->data.value_item.type);
    assert(ast_array != NULL && ast_array->kind == CM_AST_TYPE_ARRAY);
    ast_outer = (CmAstType *)(void *)cm_ast_get_type(borrowed_ast,
        ast_array->child);
    assert(ast_outer != NULL && ast_outer->kind == CM_AST_TYPE_TUPLE
        && ast_outer->element_count == 2u);
    ast_inner_id = ast_outer->elements[0];
    ast_inner = cm_ast_get_type(borrowed_ast, ast_inner_id);
    assert(ast_inner != NULL && ast_inner->kind == CM_AST_TYPE_TUPLE
        && ast_inner->element_count == 1u);
    ast_primitive_id = ast_inner->elements[0];

    hir_elements = (CmHirTypeId *)cm_alloc_zeroed(duplicate_count,
        sizeof(*hir_elements));
    ast_elements = (CmAstTypeId *)cm_alloc_zeroed(duplicate_count,
        sizeof(*ast_elements));
    for (index = 0u; index < duplicate_count; ++index) {
        assert(cm_hir_add_type(&repeated_fixture.hir, &duplicate_template,
                &hir_elements[index]) == CM_HIR_OK);
        ast_elements[index] = ast_inner_id;
    }
    hir_outer = (CmHirType *)cm_hir_get_type(&repeated_fixture.hir,
        hir_outer_id);
    saved_hir_elements = hir_outer->data.tuple_type.elements;
    saved_ast_elements = ast_outer->elements;
    saved_count = hir_outer->data.tuple_type.element_count;
    hir_outer->data.tuple_type.elements = hir_elements;
    hir_outer->data.tuple_type.element_count = (uint32_t)duplicate_count;
    ast_outer->elements = ast_elements;
    ast_outer->element_count = (uint32_t)duplicate_count;
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&repeated_fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    tuple_count = 0u;
    for (index = 0u; index < metadata.type_count; ++index)
        if (metadata.types[index].kind == CM_HIR_DECL_TYPE_TUPLE)
            tuple_count += 1u;
    assert(tuple_count == 2u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    saved_types = metadata.types;
    saved_values = metadata.values;

    hir_outer->data.tuple_type.elements = saved_hir_elements;
    hir_outer->data.tuple_type.element_count = saved_count;
    ast_outer->elements = saved_ast_elements;
    ast_outer->element_count = saved_count;
    cm_free(hir_elements);
    cm_free(ast_elements);

    hir_elements = (CmHirTypeId *)cm_alloc_zeroed(
        CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES, sizeof(*hir_elements));
    ast_elements = (CmAstTypeId *)cm_alloc_zeroed(
        CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES, sizeof(*ast_elements));
    for (index = 0u; index < CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES; ++index) {
        hir_elements[index] = hir_primitive_id;
        ast_elements[index] = ast_primitive_id;
    }
    hir_outer->data.tuple_type.elements = hir_elements;
    hir_outer->data.tuple_type.element_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES - UINT32_C(2);
    ast_outer->elements = ast_elements;
    ast_outer->element_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES - UINT32_C(2);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    slice_count = 0u;
    raw_pointer_count = 0u;
    reference_count = 0u;
    for (index = 0u; index < metadata.type_count; ++index) {
        if (metadata.types[index].kind == CM_HIR_DECL_TYPE_SLICE)
            slice_count += 1u;
        else if (metadata.types[index].kind == CM_HIR_DECL_TYPE_RAW_POINTER)
            raw_pointer_count += 1u;
        else if (metadata.types[index].kind == CM_HIR_DECL_TYPE_REFERENCE)
            reference_count += 1u;
    }
    assert(slice_count == 1u && raw_pointer_count == 2u
        && reference_count == 1u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    saved_types = metadata.types;
    saved_values = metadata.values;
    saved_associated = metadata.associated_items;
    hir_outer->data.tuple_type.element_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES - UINT32_C(1);
    ast_outer->element_count =
        (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES - UINT32_C(1);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_TYPE_METADATA_INVALID
        && metadata.types == saved_types && metadata.values == saved_values
        && metadata.associated_items == saved_associated);
    hir_outer->data.tuple_type.elements = saved_hir_elements;
    hir_outer->data.tuple_type.element_count = saved_count;
    ast_outer->elements = saved_ast_elements;
    ast_outer->element_count = saved_count;
    cm_free(hir_elements);
    cm_free(ast_elements);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&repeated_fixture);
}

static void test_many_unique_type_canonicalization_is_order_independent(void)
{
    enum { ARRAY_COUNT = 4096 };
    CaptureFixture ascending;
    CaptureFixture descending;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata ascending_metadata;
    CmHirDeclarationMetadata descending_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf ascending_bytes;
    CmByteBuf descending_bytes;
    size_t index;
    size_t array_index = 0u;
    many_unique_array_fixture_init(&ascending, 0, 0, ARRAY_COUNT);
    many_unique_array_fixture_init(&descending, 1, 1, ARRAY_COUNT);
    cm_hir_declaration_metadata_init(&ascending_metadata);
    cm_hir_declaration_metadata_init(&descending_metadata);
    input = capture_input(&ascending);
    result = cm_hir_declaration_metadata_capture(&input, &ascending_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && cm_hir_declaration_metadata_validate(&ascending_metadata)
            == CM_HIR_DECL_METADATA_OK);
    input = capture_input(&descending);
    result = cm_hir_declaration_metadata_capture(&input, &descending_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && cm_hir_declaration_metadata_validate(&descending_metadata)
            == CM_HIR_DECL_METADATA_OK);
    for (index = 0u; index < ascending_metadata.type_count; ++index) {
        const CmHirDeclarationType *type = &ascending_metadata.types[index];
        if (type->kind != CM_HIR_DECL_TYPE_ARRAY) continue;
        assert(array_index < ARRAY_COUNT
            && type->array_length_low_bits == array_index
            && type->array_length_high_bits == 0u
            && type->child_type < index + 1u
            && type->array_length_type < index + 1u);
        array_index += 1u;
    }
    assert(array_index == ARRAY_COUNT);
    cm_byte_buf_init(&ascending_bytes);
    cm_byte_buf_init(&descending_bytes);
    assert(cm_hir_declaration_metadata_encode(&ascending_metadata,
                &ascending_bytes) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&descending_metadata,
                &descending_bytes) == CM_HIR_DECL_METADATA_OK
        && ascending_bytes.len == descending_bytes.len
        && memcmp(ascending_bytes.data, descending_bytes.data,
            ascending_bytes.len) == 0);
    cm_byte_buf_destroy(&descending_bytes);
    cm_byte_buf_destroy(&ascending_bytes);
    cm_hir_declaration_metadata_destroy(&descending_metadata);
    cm_hir_declaration_metadata_destroy(&ascending_metadata);
    fixture_destroy(&descending);
    fixture_destroy(&ascending);
}

static void test_many_self_traits_use_indexed_trait_locals(void)
{
    enum { TRAIT_COUNT = 1024 };
    CaptureFixture fixture;
    CaptureFixture dependency;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationMetadata dependency_metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult materialize_result;
    CmHirLibraryArtifact dependency_artifact;
    const CmHirCrate *provider_crate;
    size_t self_count = 0u;
    size_t reference_count = 0u;
    size_t index;
    fixture_init(&dependency, 0);
    cm_hir_declaration_metadata_init(&dependency_metadata);
    input = capture_input(&dependency);
    result = cm_hir_declaration_metadata_capture(&input,
        &dependency_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    many_self_trait_fixture_init(&fixture, TRAIT_COUNT);
    memset(&expectation, 0, sizeof(expectation));
    expectation.crate_name = dependency_metadata.crate_name;
    expectation.crate_disambiguator =
        dependency_metadata.crate_disambiguator;
    expectation.edition = dependency_metadata.edition;
    expectation.target_triple = dependency_metadata.target_triple;
    expectation.data_layout = dependency_metadata.data_layout;
    expectation.panic_strategy = dependency_metadata.panic_strategy;
    expectation.cfgs = dependency_metadata.cfgs;
    expectation.cfg_count = dependency_metadata.cfg_count;
    provider_crate = cm_hir_get_crate(&fixture.hir,
        fixture.lower_result.crate_id);
    assert(provider_crate != NULL && fixture.hir.crates.len == 1u);
    cm_hir_library_artifact_init(&dependency_artifact);
    materialize_result = cm_hir_declaration_metadata_materialize(
        &fixture.hir, &dependency_artifact, &dependency_metadata,
        &expectation, "indexed_trait_dependency", provider_crate->span.source);
    assert(materialize_result.status == CM_HIR_DECL_MATERIALIZE_OK
        && fixture.hir.crates.len == 2u
        && materialize_result.crate_id != fixture.lower_result.crate_id);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.trait_count == TRAIT_COUNT + 1u
        && result.associated_count == TRAIT_COUNT
        && metadata.associated_count == TRAIT_COUNT);
    for (index = 0u; index < metadata.type_count; ++index) {
        if (metadata.types[index].kind == CM_HIR_DECL_TYPE_SELF)
            self_count += 1u;
        else if (metadata.types[index].kind == CM_HIR_DECL_TYPE_REFERENCE)
            reference_count += 1u;
    }
    assert(self_count == TRAIT_COUNT && reference_count == TRAIT_COUNT
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    cm_hir_library_artifact_destroy(&dependency_artifact);
    fixture_destroy(&fixture);
    cm_hir_declaration_metadata_destroy(&dependency_metadata);
    fixture_destroy(&dependency);
}

static void test_unit_function_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CaptureFixture stable;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata stable_metadata;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationValue *value;
    const CmHirDeclarationType *return_type;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    CmByteBuf stable_bytes;
    CmByteBuf decoded_bytes;
    unit_function_fixture_init(&first, 0);
    unit_function_fixture_init(&noisy, 1);
    fixture_init_source(&stable, 0, "unit-function-stable.rs",
        unit_function_stable_fixture_source,
        sizeof(unit_function_stable_fixture_source) - 1u);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    cm_hir_declaration_metadata_init(&stable_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.value_count == 1u && result.trait_count == 1u
        && result.projected_semantic_attribute_count == 2u
        && first_metadata.generic_count == 0u
        && first_metadata.predicate_count == 0u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    input = capture_input(&stable);
    result = cm_hir_declaration_metadata_capture(&input, &stable_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    value = find_declaration_value(&first_metadata, "breakpoint_like", NULL);
    assert(value != NULL && value->kind == CM_HIR_DECL_VALUE_FUNCTION
        && value->generic_start == 0u && value->generic_count == 0u
        && value->predicate_start == 0u && value->predicate_count == 0u
        && value->parameter_count == 0u && value->parameter_types == NULL
        && value->has_body == 1u && value->is_const == 0u);
    return_type = &first_metadata.types[value->return_type - 1u];
    assert(return_type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && return_type->primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && cm_hir_declaration_metadata_validate(&first_metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    cm_byte_buf_init(&stable_bytes);
    cm_byte_buf_init(&decoded_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&stable_metadata, &stable_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && first_bytes.len == stable_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0
        && memcmp(first_bytes.data, stable_bytes.data, first_bytes.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(first_bytes.data,
                first_bytes.len, &decoded) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&decoded, &decoded_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == decoded_bytes.len
        && memcmp(first_bytes.data, decoded_bytes.data,
            first_bytes.len) == 0);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&decoded_bytes);
    cm_byte_buf_destroy(&stable_bytes);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&stable_metadata);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&stable);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void assert_unit_function_source_fails(
    const unsigned char *source, size_t source_length)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata sentinel;
    CmHirDeclarationCaptureResult result;
    fixture_init_source(&fixture, 0, "unit-function-negative.rs", source,
        source_length);
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.root_module = UINT32_C(91);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &sentinel);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && sentinel.root_module == UINT32_C(91));
    fixture_destroy(&fixture);
}

static void test_unit_function_unsupported_sources_fail_closed(void)
{
    static const unsigned char no_inline[] =
        "pub trait Marker {}\n"
        "#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "pub fn breakpoint_like() {}\n";
    static const unsigned char extra_attribute[] =
        "pub trait Marker {}\n"
        "#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "#[inline(always)]\n#[deprecated(since=\"1.0.0\")]\n"
        "pub fn breakpoint_like() {}\n";
    static const unsigned char parameter[] =
        "pub trait Marker {}\n"
        "#[stable(feature=\"x\",since=\"1.0.0\")]\n#[inline]\n"
        "pub fn breakpoint_like(_value: u8) {}\n";
    static const unsigned char explicit_unit[] =
        "pub trait Marker {}\n"
        "#[stable(feature=\"x\",since=\"1.0.0\")]\n#[inline]\n"
        "pub fn breakpoint_like() -> () {}\n";
    assert_unit_function_source_fails(no_inline, sizeof(no_inline) - 1u);
    assert_unit_function_source_fails(extra_attribute,
        sizeof(extra_attribute) - 1u);
    assert_unit_function_source_fails(parameter, sizeof(parameter) - 1u);
    assert_unit_function_source_fails(explicit_unit,
        sizeof(explicit_unit) - 1u);
}

static void test_unit_function_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationType *saved_types;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItem *item;
    CmHirItemId item_id;
    CmHirBody *body;
    CmHirType *return_type;
    CmInternId saved_metadata;
    uint32_t saved_source_attribute;
    CmSpan saved_span;
    CmHirDefId saved_definition;
    uint32_t saved_source_expression;
    CmModuleId root_module;
    const CmAst *borrowed_ast;
    CmAstItem *ast_item = NULL;
    CmResolveEffectiveItem effective;
    uint32_t ordinal;
    unit_function_fixture_init(&fixture, 1);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_types = metadata.types;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)(void *)find_item(&fixture, "breakpoint_like",
        &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && item->attribute_count == 2u
        && item->data.function_item.signature.parameter_count == 0u);
    body = (CmHirBody *)(void *)cm_hir_get_body(&fixture.hir,
        item->data.function_item.body);
    return_type = (CmHirType *)(void *)cm_hir_get_type(&fixture.hir,
        item->data.function_item.signature.return_type);
    assert(body != NULL && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_UNIT_KIND);

#define ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values && metadata.types == saved_types \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_metadata = item->attributes[1].metadata;
    item->attributes[1].metadata = cm_hir_intern(&fixture.hir,
        "inline(sometimes)");
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attributes[1].metadata = saved_metadata;
    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = item->attributes[1].metadata;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attributes[0].metadata = saved_metadata;
    item->attributes[1].metadata = cm_hir_intern(&fixture.hir,
        "stable(feature = \"x\", since = \"1.0.0\")");
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attributes[1].metadata = cm_hir_intern(&fixture.hir,
        "inline(always)");
    item->attribute_count = 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attribute_count = 2u;
    saved_source_attribute = item->attributes[0].source_attribute;
    item->attributes[0].source_attribute += 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attributes[0].source_attribute = saved_source_attribute;
    saved_span = item->attributes[1].span;
    item->attributes[1].span.start += 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attributes[1].span = saved_span;
    item->attributes[1].expansion_depth = 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->attributes[1].expansion_depth = 0u;

    item->data.function_item.signature.safety = CM_HIR_UNSAFE;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->data.function_item.signature.safety = CM_HIR_SAFE;
    item->data.function_item.signature.is_const = 1;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->data.function_item.signature.is_const = 0;
    saved_metadata = item->data.function_item.signature.abi;
    item->data.function_item.signature.abi = cm_hir_intern(&fixture.hir,
        "C");
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->data.function_item.signature.abi = saved_metadata;
    item->data.function_item.signature.parameter_count = 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->data.function_item.signature.parameter_count = 0u;
    return_type->kind = CM_HIR_TYPE_BOOL_KIND;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    return_type->kind = CM_HIR_TYPE_UNIT_KIND;
    item->generic_parameter_count = 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->generic_parameter_count = 0u;

    saved_source_expression = body->source_expression_id;
    body->source_expression_id = UINT32_MAX;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    body->source_expression_id = saved_source_expression;
    body->local_count = 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    body->local_count = 0u;
    body->state = CM_HIR_BODY_ERROR;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    body->state = CM_HIR_BODY_UNLOWERED;
    body->root_expression = 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    body->root_expression = CM_HIR_EXPR_NONE;
    saved_span = body->span;
    body->span.start += 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    body->span = saved_span;

    saved_definition = item->definition;
    item->definition.index += 1u;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->definition = saved_definition;
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    item->visibility.kind = CM_HIR_VIS_PUBLIC;

    assert(cm_module_graph_get_root(&fixture.graph, &root_module)
        && cm_module_graph_borrow_ast(&fixture.graph, root_module,
            &borrowed_ast));
    for (ordinal = 0u; ordinal < 2u; ++ordinal) {
        const CmAstItem *candidate;
        const CmInternedString *name;
        assert(cm_module_graph_get_effective_item(&fixture.graph,
            fixture.graph_result.revision, root_module, ordinal,
            &effective) == CM_RESOLVE_VIEW_OK);
        candidate = cm_ast_get_item(borrowed_ast, effective.declaration.item);
        name = candidate == NULL ? NULL
            : cm_ast_get_string(borrowed_ast, candidate->name);
        if (name != NULL && name->len == strlen("breakpoint_like")
            && memcmp(name->bytes, "breakpoint_like", name->len) == 0) {
            ast_item = (CmAstItem *)(void *)candidate;
            break;
        }
    }
    assert(ast_item != NULL && ast_item->kind == CM_AST_ITEM_FUNCTION);
    ast_item->data.function_item.is_unsafe = 1;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();
    ast_item->data.function_item.is_unsafe = 0;
    ast_item->data.function_item.body = CM_AST_EXPR_NONE;
    ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE();

    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
#undef ASSERT_UNIT_FUNCTION_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_type_name_const_functions_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationValue *type_name;
    const CmHirDeclarationType *return_type;
    const CmHirDeclarationType *child;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    CmByteBuf decoded_bytes;
    type_name_fixture_init(&first, 0);
    type_name_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.value_count == 1u && result.trait_count == 1u
        && result.item_count == 0u
        && result.projected_semantic_attribute_count == 3u
        && first_metadata.generic_count == 1u
        && first_metadata.predicate_count == 0u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 3u);
    type_name = find_declaration_value(&first_metadata, "type_name_like",
        NULL);
    assert(type_name != NULL
        && type_name->kind == CM_HIR_DECL_VALUE_FUNCTION
        && type_name->is_const == 1u && type_name->has_body == 1u
        && type_name->generic_count == 1u
        && type_name->predicate_start == 0u
        && type_name->predicate_count == 0u
        && type_name->parameter_count == 0u
        && type_name->parameter_types == NULL);
    return_type = &first_metadata.types[type_name->return_type - 1u];
    assert(return_type->kind == CM_HIR_DECL_TYPE_REFERENCE
        && return_type->mutability == CM_HIR_DECL_IMMUTABLE
        && return_type->region.kind == CM_HIR_DECL_REGION_STATIC);
    child = &first_metadata.types[return_type->child_type - 1u];
    assert(child->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && child->primitive == CM_HIR_DECL_PRIMITIVE_STR);
    assert(first_metadata.generics[0].owner_kind
            == CM_HIR_DECL_GENERIC_VALUE
        && first_metadata.generics[0].owner_local == 1u
        && first_metadata.generics[0].is_relaxed_sized == 1u);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    cm_byte_buf_init(&decoded_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(first_bytes.data,
                first_bytes.len, &decoded) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&decoded, &decoded_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == decoded_bytes.len
        && memcmp(first_bytes.data, decoded_bytes.data,
            first_bytes.len) == 0);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&decoded_bytes);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_type_name_const_function_hostiles_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationType *saved_types;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItem *type_name;
    CmHirItemId ignored_id;
    CmHirGenericParam *generic;
    CmHirBody *body;
    CmHirType *return_type;
    CmHirTypeId saved_child;
    CmHirRegionKind saved_region;
    CmSpan saved_span;
    CmInternId saved_metadata;
    uint32_t saved_source_attribute;
    CmHirDefId saved_owner;
    uint32_t saved_source_expression;
    CmModuleId root_module;
    const CmAst *borrowed_ast;
    CmResolveEffectiveItem effective;
    CmAstItem *ast_item;
    const CmInternedString *effective_name;
    uint32_t ordinal;
    type_name_fixture_init(&fixture, 1);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_types = metadata.types;
    saved_namespace = metadata.namespace_entries;
    type_name = (CmHirItem *)(void *)find_item(&fixture, "type_name_like",
        &ignored_id);
    assert(type_name != NULL && type_name->attribute_count == 3u);
    generic = (CmHirGenericParam *)(void *)cm_hir_get_generic_param(
        &fixture.hir, type_name->generic_parameter_start);
    body = (CmHirBody *)(void *)cm_hir_get_body(&fixture.hir,
        type_name->data.function_item.body);
    return_type = (CmHirType *)(void *)cm_hir_get_type(&fixture.hir,
        type_name->data.function_item.signature.return_type);
    assert(generic != NULL && body != NULL && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_REFERENCE_KIND);

#define ASSERT_TYPE_NAME_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values && metadata.types == saved_types \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    generic->is_relaxed_sized = 0;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    generic->is_relaxed_sized = 1;

    type_name->data.function_item.signature.is_const = 0;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->data.function_item.signature.is_const = 1;

    type_name->data.function_item.signature.safety = CM_HIR_UNSAFE;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->data.function_item.signature.safety = CM_HIR_SAFE;

    saved_region = return_type->data.reference_type.region.kind;
    return_type->data.reference_type.region.kind = CM_HIR_REGION_INFER;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    return_type->data.reference_type.region.kind = saved_region;

    saved_child = return_type->data.reference_type.pointee;
    return_type->data.reference_type.pointee =
        type_name->data.function_item.signature.return_type;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    return_type->data.reference_type.pointee = saved_child;

    return_type->data.reference_type.mutability = CM_HIR_MUTABLE;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    return_type->data.reference_type.mutability = CM_HIR_IMMUTABLE;

    saved_metadata = type_name->attributes[0].metadata;
    type_name->attributes[0].metadata = cm_hir_intern(&fixture.hir,
        "must_use(because)");
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->attributes[0].metadata = saved_metadata;

    saved_metadata = type_name->attributes[2].metadata;
    type_name->attributes[2].metadata = type_name->attributes[1].metadata;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->attributes[2].metadata = saved_metadata;

    saved_source_attribute = type_name->attributes[1].source_attribute;
    type_name->attributes[1].source_attribute += 1u;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->attributes[1].source_attribute = saved_source_attribute;

    saved_span = type_name->attributes[1].span;
    type_name->attributes[1].span.start += 1u;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->attributes[1].span = saved_span;

    type_name->attributes[2].expansion_depth = 1u;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    type_name->attributes[2].expansion_depth = 0u;

    saved_source_expression = body->source_expression_id;
    body->source_expression_id = UINT32_MAX;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    body->source_expression_id = saved_source_expression;

    body->expected_type = saved_child;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    body->expected_type = type_name->data.function_item.signature.return_type;

    saved_owner = body->owner;
    body->owner.index += 1u;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    body->owner = saved_owner;

    assert(cm_module_graph_get_root(&fixture.graph, &root_module)
        && cm_module_graph_borrow_ast(&fixture.graph, root_module,
            &borrowed_ast));
    ast_item = NULL;
    for (ordinal = 0u; ordinal < 2u; ++ordinal) {
        assert(cm_module_graph_get_effective_item(&fixture.graph,
            fixture.graph_result.revision, root_module, ordinal,
            &effective) == CM_RESOLVE_VIEW_OK);
        effective_name = cm_ast_get_string(borrowed_ast,
            cm_ast_get_item(borrowed_ast, effective.declaration.item)->name);
        if (effective_name != NULL
            && effective_name->len == strlen("type_name_like")
            && memcmp(effective_name->bytes, "type_name_like",
                effective_name->len) == 0) {
            ast_item = (CmAstItem *)(void *)cm_ast_get_item(borrowed_ast,
                effective.declaration.item);
            break;
        }
    }
    assert(ast_item != NULL && ast_item->kind == CM_AST_ITEM_FUNCTION);
    ast_item->data.function_item.is_const = 0;
    ASSERT_TYPE_NAME_ATOMIC_FAILURE();
    ast_item->data.function_item.is_const = 1;
    {
        CmAstType *ast_return = (CmAstType *)(void *)cm_ast_get_type(
            borrowed_ast, ast_item->data.function_item.return_type);
        CmInternId saved_lifetime;
        assert(ast_return != NULL && ast_return->kind == CM_AST_TYPE_REFERENCE);
        saved_lifetime = ast_return->lifetime;
        ast_return->lifetime = CM_INTERN_ID_NONE;
        ASSERT_TYPE_NAME_ATOMIC_FAILURE();
        ast_return->lifetime = saved_lifetime;
    }

    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
#undef ASSERT_TYPE_NAME_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_type_name_of_val_const_function_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationValue *value;
    const CmHirDeclarationType *parameter;
    const CmHirDeclarationType *parameter_child;
    const CmHirDeclarationType *return_type;
    const CmHirDeclarationType *return_child;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    CmByteBuf decoded_bytes;
    type_name_of_val_fixture_init(&first, 0);
    type_name_of_val_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.value_count == 1u && result.trait_count == 1u
        && result.projected_semantic_attribute_count == 3u
        && first_metadata.generic_count == 1u
        && first_metadata.predicate_count == 0u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 3u);
    value = find_declaration_value(&first_metadata,
        "type_name_of_val_like", NULL);
    assert(value != NULL && value->kind == CM_HIR_DECL_VALUE_FUNCTION
        && value->is_const == 1u && value->has_body == 1u
        && value->generic_count == 1u && value->predicate_start == 0u
        && value->predicate_count == 0u && value->parameter_count == 1u
        && value->parameter_types != NULL);
    parameter = &first_metadata.types[value->parameter_types[0] - 1u];
    assert(parameter->kind == CM_HIR_DECL_TYPE_REFERENCE
        && parameter->mutability == CM_HIR_DECL_IMMUTABLE
        && parameter->region.kind == CM_HIR_DECL_REGION_ERASED);
    parameter_child = &first_metadata.types[parameter->child_type - 1u];
    assert(parameter_child->kind == CM_HIR_DECL_TYPE_GENERIC
        && parameter_child->generic_local == value->generic_start);
    return_type = &first_metadata.types[value->return_type - 1u];
    assert(return_type->kind == CM_HIR_DECL_TYPE_REFERENCE
        && return_type->mutability == CM_HIR_DECL_IMMUTABLE
        && return_type->region.kind == CM_HIR_DECL_REGION_STATIC);
    return_child = &first_metadata.types[return_type->child_type - 1u];
    assert(return_child->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && return_child->primitive == CM_HIR_DECL_PRIMITIVE_STR);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    cm_byte_buf_init(&decoded_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(first_bytes.data,
                first_bytes.len, &decoded) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&decoded, &decoded_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == decoded_bytes.len
        && memcmp(first_bytes.data, decoded_bytes.data,
            first_bytes.len) == 0);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&decoded_bytes);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void assert_type_name_of_val_source_fails(
    const unsigned char *source, size_t source_length)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata sentinel;
    CmHirDeclarationCaptureResult result;
    CmHirItemId item_id;
    assert(source != NULL && source_length != 0u);
    fixture_init_source(&fixture, 0, "type-name-of-val-negative.rs",
        source, source_length);
    assert(find_item(&fixture, "type_name_of_val_like", &item_id) != NULL);
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.root_module = UINT32_C(77);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &sentinel);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID
        && result.rejected_item == item_id
        && sentinel.root_module == UINT32_C(77));
    fixture_destroy(&fixture);
}

static void test_type_name_of_val_unsupported_lifetimes_fail_closed(void)
{
    static const unsigned char explicit_placeholder[] =
        "pub trait Marker {}\n"
        "#[must_use]\n#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "#[rustc_const_unstable(feature=\"x\",issue=\"none\")]\n"
        "pub const fn type_name_of_val_like<T:?Sized>(_val: &'_ T) "
        "-> &'static str { \"\" }\n";
    static const unsigned char explicit_named[] =
        "pub trait Marker {}\n"
        "#[must_use]\n#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "#[rustc_const_unstable(feature=\"x\",issue=\"none\")]\n"
        "pub const fn type_name_of_val_like<'a,T:?Sized>(_val: &'a T) "
        "-> &'static str { \"\" }\n";
    static const unsigned char explicit_static[] =
        "pub trait Marker {}\n"
        "#[must_use]\n#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "#[rustc_const_unstable(feature=\"x\",issue=\"none\")]\n"
        "pub const fn type_name_of_val_like<T:?Sized>(_val: &'static T) "
        "-> &'static str { \"\" }\n";
    static const unsigned char second_input[] =
        "pub trait Marker {}\n"
        "#[must_use]\n#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "#[rustc_const_unstable(feature=\"x\",issue=\"none\")]\n"
        "pub const fn type_name_of_val_like<T:?Sized>(_val: &T, _b: &T) "
        "-> &'static str { \"\" }\n";
    static const unsigned char omitted_output[] =
        "pub trait Marker {}\n"
        "#[must_use]\n#[stable(feature=\"x\",since=\"1.0.0\")]\n"
        "#[rustc_const_unstable(feature=\"x\",issue=\"none\")]\n"
        "pub const fn type_name_of_val_like<T:?Sized>(_val: &T) "
        "-> &str { \"\" }\n";
    assert_type_name_of_val_source_fails(explicit_placeholder,
        sizeof(explicit_placeholder) - 1u);
    assert_type_name_of_val_source_fails(explicit_named,
        sizeof(explicit_named) - 1u);
    assert_type_name_of_val_source_fails(explicit_static,
        sizeof(explicit_static) - 1u);
    assert_type_name_of_val_source_fails(second_input,
        sizeof(second_input) - 1u);
    assert_type_name_of_val_source_fails(omitted_output,
        sizeof(omitted_output) - 1u);
}

static void test_type_name_of_val_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationType *saved_types;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItem *item;
    CmHirItemId ignored_id;
    CmHirFunctionParameter *parameter;
    CmHirType *parameter_type;
    CmHirTypeId saved_pointee;
    CmHirRegion saved_region;
    CmHirMutability saved_mutability;
    CmHirBindingKind saved_binding_kind;
    CmInternId saved_name;
    CmSpan saved_span;
    CmHirParameterBindingMode saved_binding_mode;
    CmHirBody *body;
    CmHirTypeId saved_local_type;
    uint32_t saved_parameter_index;
    CmModuleId root_module;
    const CmAst *borrowed_ast;
    CmAstItem *ast_item = NULL;
    CmAstType *ast_parameter_type;
    CmAstPattern *ast_pattern;
    CmResolveEffectiveItem effective;
    uint32_t ordinal;
    type_name_of_val_fixture_init(&fixture, 1);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_types = metadata.types;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)(void *)find_item(&fixture,
        "type_name_of_val_like", &ignored_id);
    assert(item != NULL
        && item->data.function_item.signature.parameter_count == 1u);
    parameter = &item->data.function_item.signature.parameters[0];
    parameter_type = (CmHirType *)(void *)cm_hir_get_type(&fixture.hir,
        parameter->type);
    body = (CmHirBody *)(void *)cm_hir_get_body(&fixture.hir,
        item->data.function_item.body);
    assert(parameter_type != NULL
        && parameter_type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && parameter_type->data.reference_type.region.kind
            == CM_HIR_REGION_ERASED
        && body != NULL && body->local_count == 1u && body->locals != NULL);

#define ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values && metadata.types == saved_types \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_region = parameter_type->data.reference_type.region;
    parameter_type->data.reference_type.region.kind = CM_HIR_REGION_INFER;
    parameter_type->data.reference_type.region.data.inference_variable = 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter_type->data.reference_type.region = saved_region;
    parameter_type->data.reference_type.region.data.inference_variable = 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter_type->data.reference_type.region = saved_region;

    saved_pointee = parameter_type->data.reference_type.pointee;
    parameter_type->data.reference_type.pointee =
        item->data.function_item.signature.return_type;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter_type->data.reference_type.pointee = saved_pointee;
    saved_mutability = parameter_type->data.reference_type.mutability;
    parameter_type->data.reference_type.mutability = CM_HIR_MUTABLE;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter_type->data.reference_type.mutability = saved_mutability;
    parameter_type->span.start += 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter_type->span.start -= 1u;

    saved_name = parameter->name;
    parameter->name = CM_INTERN_ID_NONE;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter->name = saved_name;
    saved_span = parameter->span;
    parameter->span.start += 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter->span = saved_span;
    saved_binding_kind = parameter->binding_kind;
    parameter->binding_kind = CM_HIR_BINDING_DISCARD;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter->binding_kind = saved_binding_kind;
    saved_binding_mode = parameter->binding_mode;
    parameter->binding_mode = CM_HIR_PARAMETER_BINDING_REF_SHARED;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    parameter->binding_mode = saved_binding_mode;

    saved_local_type = body->locals[0].type;
    body->locals[0].type = item->data.function_item.signature.return_type;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->locals[0].type = saved_local_type;
    body->locals[0].name = CM_INTERN_ID_NONE;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->locals[0].name = parameter->name;
    body->locals[0].mutability = CM_HIR_MUTABLE;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->locals[0].mutability = CM_HIR_IMMUTABLE;
    saved_parameter_index = body->locals[0].parameter_index;
    body->locals[0].parameter_index = 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->locals[0].parameter_index = saved_parameter_index;
    body->locals[0].span.start += 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->locals[0].span = parameter->span;
    body->locals[0].parameter_binding_index = 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->locals[0].parameter_binding_index = 0u;
    body->parameter_count = 0u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    body->parameter_count = 1u;

    assert(cm_module_graph_get_root(&fixture.graph, &root_module)
        && cm_module_graph_borrow_ast(&fixture.graph, root_module,
            &borrowed_ast));
    for (ordinal = 0u; ordinal < 2u; ++ordinal) {
        const CmAstItem *candidate;
        const CmInternedString *name;
        assert(cm_module_graph_get_effective_item(&fixture.graph,
            fixture.graph_result.revision, root_module, ordinal,
            &effective) == CM_RESOLVE_VIEW_OK);
        candidate = cm_ast_get_item(borrowed_ast,
            effective.declaration.item);
        name = candidate == NULL ? NULL
            : cm_ast_get_string(borrowed_ast, candidate->name);
        if (name != NULL && name->len == strlen("type_name_of_val_like")
            && memcmp(name->bytes, "type_name_of_val_like", name->len) == 0) {
            ast_item = (CmAstItem *)(void *)candidate;
            break;
        }
    }
    assert(ast_item != NULL
        && ast_item->data.function_item.parameter_count == 1u);
    ast_parameter_type = (CmAstType *)(void *)cm_ast_get_type(borrowed_ast,
        ast_item->data.function_item.parameters[0].type);
    ast_pattern = (CmAstPattern *)(void *)cm_ast_get_pattern(borrowed_ast,
        ast_item->data.function_item.parameters[0].pattern);
    assert(ast_parameter_type != NULL
        && ast_parameter_type->kind == CM_AST_TYPE_REFERENCE
        && ast_pattern != NULL && ast_pattern->kind == CM_AST_PATTERN_BINDING);
    ast_parameter_type->lifetime = ast_item->name;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    ast_parameter_type->lifetime = CM_INTERN_ID_NONE;
    ast_parameter_type->is_mutable = 1;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    ast_parameter_type->is_mutable = 0;
    ast_parameter_type->span.start += 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    ast_parameter_type->span.start -= 1u;
    ast_pattern->data.binding.is_mutable = 1;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    ast_pattern->data.binding.is_mutable = 0;
    ast_pattern->span.start += 1u;
    ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE();
    ast_pattern->span.start -= 1u;

    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
#undef ASSERT_TYPE_NAME_OF_VAL_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_unit_function_capture_and_determinism();
    test_unit_function_unsupported_sources_fail_closed();
    test_unit_function_hostile_mutations_are_atomic();
    test_type_name_const_functions_and_determinism();
    test_type_name_const_function_hostiles_are_atomic();
    test_type_name_of_val_const_function_and_determinism();
    test_type_name_of_val_unsupported_lifetimes_fail_closed();
    test_type_name_of_val_hostile_mutations_are_atomic();
    test_primitive_reexports_and_determinism();
    test_primitive_reexport_hostile_mutations_are_atomic();
    test_allocator_like_associated_method_capture();
    test_any_like_safe_static_trait_capture();
    test_associated_composite_type_capture();
    test_reachable_canonical_type_caps();
    test_many_unique_type_canonicalization_is_order_independent();
    test_many_self_traits_use_indexed_trait_locals();
    test_generic_enum_projection_glob_and_field_boundary();
    test_generic_enum_hostile_mutations_are_atomic();
    test_static_capture_and_determinism();
    test_static_hostile_mutations_are_atomic();
    test_static_type_dag_is_structurally_deduplicated();
    test_named_aggregate_capture_and_determinism();
    test_named_aggregate_hostile_mutations_are_atomic();
    test_rust_tuple_struct_capture_and_atomic_boundaries();
    test_into_iter_private_closure_and_determinism();
    test_into_iter_hostile_shapes_are_atomic();
    test_from_fn_callable_closure_and_determinism();
    test_from_fn_hostile_mutations_are_atomic();
    test_from_mut_elision_profile_and_determinism();
    test_from_mut_hostile_mutations_are_atomic();
    test_from_ref_elision_profile_and_negatives();
    test_type_id_like_target_capture_and_determinism();
    test_type_id_like_hostile_mutations_are_atomic();
    test_layout_private_dependency_closure_and_determinism();
    test_layout_private_dependency_hostiles_are_atomic();
    test_default_enum_variant_capture_and_determinism();
    test_default_enum_hostile_mutations_are_atomic();
    test_char_const_capture_and_determinism();
    test_char_const_hostile_mutations_are_atomic();
    test_char_const_attributes_fail_closed_atomically();
    test_fixture_and_determinism();
    test_failure_is_atomic();
    test_zero_item_gate_path();
    test_plain_unit_struct_has_exact_empty_attribute_profile();
    test_module_attribute_projection_and_provenance();
    test_item_shape_diagnostic();
    test_non_exhaustive_authorizes_missing_constructor_mate();
    test_char_shaped_reexport_projection();
    test_ascii_char_enum_projection_and_determinism();
    test_ascii_char_128_variant_projection();
    test_enum_cfg_source_ordinal_and_atomic_negatives();
    test_reexport_alias_spelling_and_duplicate_negatives();
    test_reexport_provenance_and_generated_negatives();
    test_rustfmt_skip_reexport_projection_and_negatives();
    test_doc_inline_reexport_projection_and_negatives();
    test_doc_hidden_reexport_projection_and_negatives();
    test_alias_and_reexport_attributes_fail_closed_atomically();
    test_constructor_omission_authority_is_not_forgeable();
    test_many_private_bindings_do_not_consume_public_cap();
    return 0;
}
