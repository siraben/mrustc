# cmhir-meta-v3: downstream-safe declaration, semantic, and executable metadata

This document is the implementation contract and staged roadmap for the first
`cmhir` metadata family that may materialize complete trait declarations and
positive impl facts in a fresh HIR context. It supersedes neither the exact
v1.0/v1.1 readers nor the exact v2.3 through v2.6 readers.

The words **must**, **must not**, **required**, and **reject** are normative.
An encoder rejection is `CM_HIR_METADATA_ARTIFACT_UNSUPPORTED_HIR` when the
live HIR is valid but outside the selected profile. A malformed wire artifact
is `CM_HIR_METADATA_ARTIFACT_INVALID_FORMAT`; an aggregate or traversal limit
failure is `CM_HIR_METADATA_ARTIFACT_LIMIT_EXCEEDED`.

## Goals

Version 3 has five immediate goals:

1. Give every transported module, nominal declaration, associated item,
   ordinary item, value, generic parameter, type, and impl one deterministic
   file-local identity. Runtime `CmHirDefId`, `CmHirItemId`, `CmHirTypeId`, and
   arena addresses never cross the boundary.
2. Materialize complete public traits, trait aliases, and associated
   declarations without ever representing a reference-only header as an
   empty, complete `CmHirItem`.
3. Represent projection and function-pointer types needed by Rust 1.90 core,
   including the exact declaring owner of an inherited associated item. For
   example, a callable equality naming `Output` must retain the identity of
   `FnOnce::Output`, even when the surface path names `Fn`.
4. Add complete active-configuration impl facts in v3.1 while retaining an
   open global impl universe: transported presence may prove a goal, but
   absence must not prove that no implementation exists.
5. Add one deliberately narrow executable cross-crate slice in v3.2. It binds
   a structurally validated generic body recipe and native object member to
   the same crate, source, target, and cfg identity as its declarations and
   impl facts. It is a vertical bootstrap gate, not a claim of general Rust
   body or link coverage.

The v3.0 acceptance boundary is fresh-context declaration lowering and name
resolution. The v3.1 acceptance boundary additionally permits positive trait
selection and associated-type normalization from authenticated facts. The
v3.2 acceptance boundary is a fresh-process compile, consumer-side generic
instantiation, native link, and execution of the exact `EXECUTABLE_SLICE`
profile defined below.

## Non-goals

V3.0 and v3.1 do not transport macro definitions, expanded token streams,
generic bodies, MIR, evaluated const bodies, native objects, dependency
archives, or link directives. They are not complete `.rmeta` or `.rlib`
formats and are not by themselves sufficient to build `alloc`.

V3.2 does not change that statement for v3.0 or v3.1. Its body and link
capabilities cover only the structural subset named by `EXECUTABLE_SLICE`.
It does not transport arbitrary expressions, MIR, const evaluation, statics,
drop glue, vtables, macros, build-script directives, arbitrary native
libraries, or general object-backed Rust functions. It is not sufficient to
build `core` or `alloc`.

In particular, omitting impls, traits, aliases, associated declarations, or
namespace bindings and retaining only RESERVED nominal headers is a useful
capture/census checkpoint, but it is not an alloc-consumable artifact. Such an
artifact must advertise `REFERENCE_ONLY` or `ABSENT` for the affected
families. A consumer requiring declaration lowering or semantic selection
must reject it before changing its HIR context.

External dependency identities are also outside v3.0 and v3.1. A producer for
those profiles encountering a declaration edge to another crate must either
resolve it through a future authenticated dependency section/version or
reject. It must not create a local definition that merely resembles the
dependency item. V3.2 is itself such an authenticated dependency format, but
only for the single-crate executable slice below; its producer likewise
rejects edges to any third crate.

## Envelope and exact version dispatch

V3 reuses the existing uncompressed envelope, explicit little-endian integer
encoding, section frames, payload limit, and CRC-32 integrity check. Flags are
zero. The major/minor pairs are exact:

- `3.0` is the `LOWER_SAFE` declaration profile.
- `3.1` is the `SEMANTIC_POSITIVE` profile.
- `3.2` is the `EXECUTABLE_SLICE` profile.

The v3 decoder first reads the envelope once, then dispatches on the exact
pair. Unknown v3 minors return unsupported version. A CRC failure, malformed
section, invalid manifest, or semantic failure after matching `3.x` must not
fall through to another v3 minor or any v2/v1 reader. Legacy readers and bytes
remain unchanged.

V3.0 and v3.1 contain exactly these sections in this order:

```text
CRAT MANF MODS NOMD AITM GPAR TYPE ITEM VALU PRED IMPL NSPC
```

V3.2 contains exactly these sections in this order:

```text
CRAT MANF MODS NOMD AITM GPAR TYPE ITEM VALU PRED IMPL NSPC BODY LINK
```

There are no optional, duplicate, reordered, or unknown sections. A family
which is absent for a profile still has its mandatory section with a zero
record count. Each section payload starts with a `u32` record count unless a
more specific count tuple is described below. All integers are little-endian;
all arrays are `u32 count` followed by exactly that many records. A local
handle is a nonzero `u32`; zero is permitted only in a field explicitly named
`optional` or `none`. Strings are `u32 byte_length` followed by exact bytes
and must satisfy the interner's name rules. Spans are not serialized; the
decoder assigns normalized spans from its `metadata_source` argument.

Stable wire tags are declared separately from C enum values. An unknown tag,
nonzero reserved field, trailing byte, or noncanonical alternate spelling is
invalid format.

## Manifest

`MANF` is checked before any HIR allocation. V3.0 and v3.1 retain manifest
schema 1 and its byte layout exactly:

```text
u8  manifest_schema                 # exactly 1 in v3.0/v3.1
u8  profile                         # LOWER_SAFE or SEMANTIC_POSITIVE
u8  impl_universe                   # OPEN
u8  reserved                        # zero
string crate_name
u32 crate_disambiguator_length
bytes crate_disambiguator
u8  edition
string target_triple
string data_layout
u8  panic_strategy
u32 cfg_count
[string cfg_entry]                  # strictly bytewise increasing
u32 family_count                    # exactly the table length below
[family_descriptor]
```

V3.2 uses manifest schema 2. Its common fields have the same encodings, but
the identity fields are inserted before `family_count`; `edition`, target,
and panic use the artifact-identity API's representations:

```text
u8  manifest_schema                 # exactly 2 in v3.2
u8  profile                         # EXECUTABLE_SLICE
u8  impl_universe                   # OPEN
u8  reserved                        # zero
string crate_name
u32 crate_disambiguator_length
bytes crate_disambiguator
u32 edition
string target_descriptor
string panic_strategy
u32 cfg_count
[string cfg_entry]                  # strictly bytewise increasing
u8  source_digest_algorithm         # SHA256
u8  reserved[3]                     # zero
bytes[32] source_digest
bytes[32] link_manifest_digest
u32 dependency_identity_count       # exactly zero in v3.2
[bytes[32] dependency_identity]
bytes[32] artifact_identity
u32 family_count                    # exactly the table length below
[family_descriptor]
```

The stable manifest wire constants are:

```text
profile:       LOWER_SAFE = 1, SEMANTIC_POSITIVE = 2,
               EXECUTABLE_SLICE = 3
impl_universe: OPEN = 0
source_digest_algorithm: SHA256 = 1
state:         ABSENT = 0, REFERENCE_ONLY = 1,
               COMPLETE_ACTIVE_CFG = 2
family_tag:    MODULE_NAMESPACE = 1, TYPE_ITEMS = 2, VALUES = 3,
               TRAIT_DECLARATIONS = 4, TRAIT_ALIASES = 5,
               ASSOCIATED_DECLARATIONS = 6, PROJECTION_TYPES = 7,
               FUNCTION_POINTER_TYPES = 8, TRAIT_IMPLS = 9,
               INHERENT_IMPLS = 10, MACROS = 11,
               SEMANTIC_ATTRIBUTES = 12, BODIES_CONST_IR = 13,
               LINK_INPUTS = 14
```

These values are wire constants, not casts of internal enums. All other
values in their storage width reject.

Each family descriptor is:

```text
u8  family_tag
u8  state
u16 reserved                        # zero
u32 logical_record_count
u32 canonical_crc32
```

Family tags appear exactly once in the fixed order below. Each family has a
fixed primary-record stream: modules plus namespace entries; ordinary items;
values; ordinary-trait `NOMD` records; trait-alias `NOMD` records;
nominal-owned `AITM` records; projection `TYPE` records; function-pointer
`TYPE` records; trait `IMPL` records plus their impl-owned `AITM` records; or
inherent `IMPL` records plus their impl-owned `AITM` records. The logical count
and CRC cover exactly that stream as `u32 count` followed by its canonical
records. In v3.2, `BODIES_CONST_IR` instead uses the canonical tuple
`u32 execution_binding_count`, the execution bindings in `value_local` order,
`u32 recipe_count`, and the `BODY` records; its logical count is the sum of
those counts. `LINK_INPUTS` uses the exact `LINK` payload and its logical count
is object count plus symbol count plus native-library count. An absent family
uses the canonical empty stream. Shared supporting `GPAR`, `TYPE`, and `PRED`
records are protected by the envelope CRC and are validated through owner
ranges and reachability; they are not ambiguously charged to several family
CRCs. The encoder computes family descriptors after constructing canonical
section payloads. CRCs provide corruption detection, not a trust signature.

The closed family-state enum is:

- `ABSENT`: the producer asserts that no facts from the family are present.
  Its logical count is zero and its canonical CRC is the CRC of the canonical
  zero-record representation.
- `REFERENCE_ONLY`: identities and schemas may be present, but no record may
  be materialized as a complete HIR declaration or used as solver evidence.
- `COMPLETE_ACTIVE_CFG`: every active source-HIR fact in the profile's capture
  domain is present. This is completeness for this crate, target, and exact
  cfg set, not completeness of the global crate graph.

Unknown states reject. A consumer must not reinterpret `REFERENCE_ONLY` as
`COMPLETE_ACTIVE_CFG` merely because a record happens to contain every field
it currently understands.

The fixed family table and required states are:

| Family | Sections | v3.0 | v3.1 | v3.2 |
| --- | --- | --- | --- | --- |
| `MODULE_NAMESPACE` | `MODS`, type/value `NSPC` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TYPE_ITEMS` | `ITEM`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `VALUES` | `VALU`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TRAIT_DECLARATIONS` | `NOMD`, `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TRAIT_ALIASES` | `NOMD`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `ABSENT` |
| `ASSOCIATED_DECLARATIONS` | `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `ABSENT` |
| `PROJECTION_TYPES` | `TYPE`, `NOMD`, `AITM` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `ABSENT` |
| `FUNCTION_POINTER_TYPES` | `TYPE` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` | `ABSENT` |
| `TRAIT_IMPLS` | `IMPL`, impl-owned `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `ABSENT` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `INHERENT_IMPLS` | `IMPL`, impl-owned `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `ABSENT` | `COMPLETE_ACTIVE_CFG` | `ABSENT` |
| `MACROS` | none in v3.0/v3.1/v3.2 | `ABSENT` | `ABSENT` | `ABSENT` |
| `SEMANTIC_ATTRIBUTES` | none in v3.0/v3.1/v3.2 | `ABSENT` | `ABSENT` | `ABSENT` |
| `BODIES_CONST_IR` | `BODY` in v3.2 | `ABSENT` | `ABSENT` | `COMPLETE_ACTIVE_CFG` |
| `LINK_INPUTS` | `LINK` in v3.2 | `ABSENT` | `ABSENT` | `COMPLETE_ACTIVE_CFG` |

`LOWER_SAFE` consumers require the first eight families to be
`COMPLETE_ACTIVE_CFG`. `SEMANTIC_POSITIVE` consumers additionally require both
impl families to be complete and `profile == SEMANTIC_POSITIVE`. An alloc
source consumer must additionally require complete macro and semantic-
attribute families in a later exact version. A codegen/link consumer must also
require bodies/const IR and link inputs. These checks occur before reserving a
crate or interning a wire string.

An `EXECUTABLE_SLICE` consumer requires the exact v3.2 column, schema 2, both
body/link sections, and the structural capture restrictions below. A v3.1
artifact with nonzero trailing bytes is not a v3.2 artifact, and a v3.2
artifact with `BODY` and `LINK` removed is not a v3.1 artifact. Exact dispatch
does not upgrade, infer, or compose capabilities across minor versions.

`COMPLETE_ACTIVE_CFG` requires exact census agreement between the manifest,
section counts, and the producer's library-capture manifest. An encoder may
not obtain the state by filtering unsupported records. Encountering one
unsupported active record rejects the complete encode transaction.

For v3.0/v3.1/v3.2, `MODULE_NAMESPACE` completeness is deliberately scoped to the
module tree and effective `TYPE` and `VALUE` namespaces. It makes no claim
about the macro namespace. Macro definitions and macro namespace bindings are
both represented by the separate `MACROS == ABSENT` family; an `NSPC` macro
record is invalid in these profiles.

## V3.2 artifact identity and exact capture domain

`EXECUTABLE_SLICE` is an exact, structurally selected profile. It never
recognizes a crate, module, trait, function, parameter, or symbol by spelling.
There are no fixture-name exceptions. Counts may be zero or greater subject to
the ordinary limits, although the G3 acceptance artifact must actually contain
a public generic recipe function, a trait, a matching positive impl, and an
object-backed public function.

Source closure identity is exactly the result of
`cm_hir_artifact_source_closure_digest` from
`cm/hir/artifact_identity.h`; v3.2 must not implement a second metadata-local
hashing convention. That API uses SHA-256 and frames every field as its
one-byte tag, a big-endian `u64` payload length, and the payload. Its stream is:

```text
field 'D', bytes "cmrustc-g3-source-closure-v1"  # no terminating NUL
field 'N', big-endian u64 source_count
[field 'P', logical_path; field 'B', exact source bytes]
```

Records are strictly ordered by logical-path bytes with no duplicate. A
logical path uses `/`, is relative, contains no empty, `.` or `..` component,
contains neither NUL nor backslash, and is independent of the producer's
working directory. The closure contains every source byte read to produce the
represented crate and every native or generated source byte used to produce a
declared object member, each under a stable noncolliding logical path. Physical
roots, mtimes, ownership, and other filesystem metadata are absent.
Environment or flags that can affect cfg, ABI, panic behavior, declaration
semantics, recipe selection, or object bytes must either already be
represented by the explicit identity fields or cause this profile to reject.
Macros and build scripts are absent in v3.2, so silently hashing only their
post-expansion output is not permitted.

`link_manifest_digest` is SHA-256 over the exact canonical `LINK` section
payload defined below, including its counts and the required zero native-
library count. It commits to every object member name, length, digest, owning
value, and external symbol without creating a digest cycle.

`artifact_identity` is exactly the result of
`cm_hir_artifact_identity_compute` from the same API. Its required
`link_manifest` input is the value stored as `MANF.link_manifest_digest`. It
uses SHA-256, the same tag/length/payload framing, the no-NUL domain
`cmrustc-g3-artifact-identity-v1`, and these fields in API structure order:

```text
'D' domain
'A' schema_major u32                # big-endian integer payload
'a' schema_minor u32
'P' profile u32
'C' crate_name
'I' crate_disambiguator
'E' edition u32
'T' target_descriptor
'R' panic_strategy
'F' cfg_count u64
['f' cfg_entry]
'S' source_digest
'L' link_manifest_digest
'N' dependency_identity_count u64
['n' dependency_identity]
```

Integer fields use fixed-width big-endian payloads within the common field
framing. Cfg strings and dependency identities are each strictly bytewise
ordered. The target descriptor is the driver's canonical target description,
including every target and data-layout fact that affects ABI or object
compatibility; a raw target nickname alone is insufficient. Although the
identity API can encode dependencies for later profiles, v3.2 requires
`dependency_identity_count == 0` and rejects any edge to a third crate.
V3.2 encodes edition as its calendar value (`2015`, `2018`, `2021`, or `2024`)
and panic strategy as the canonical ASCII name; the initial executable slice
requires `abort`.

The encoder recomputes the source, link-manifest, and artifact digests; it
never accepts caller-supplied digest bytes without comparing them to the
canonical inputs. A dependency request supplies the expected crate name,
disambiguator, source digest, and artifact identity from the build manifest.
The consumer recomputes the link-manifest digest and artifact identity and
requires every expected field to match. A missing expectation is unsupported
for this profile. It separately requires exact edition, target descriptor,
panic strategy, and cfg equality with the consuming compilation. Thus stale,
wrong-source, wrong-crate, and wrong-target artifacts reject before HIR
allocation. SHA-256 and the envelope CRC bind bytes and identity; they are not
a signature and do not establish who produced an artifact.

The complete active v3.2 capture domain is intentionally narrow:

- Modules and their effective public type/value namespace are complete. This
  initial executable slice requires exactly one defining namespace entry for
  every transported module, trait, and value; any reexport rejects v3.2.
  Ordinary `ITEM` records are unsupported, so `TYPE_ITEMS` is complete with a
  canonical empty primary stream. Primitive types are structural `TYPE`
  records, not fake items.
- Ordinary traits are complete only when they are safe marker traits with no
  generics, predicates, supertraits, associated declarations, attributes, or
  default bodies. Trait aliases are unsupported.
- Trait impls are complete only for positive, final, non-const impls of such a
  marker trait for an exact supported concrete primitive type, with no
  generics, predicates, attributes, or members. The producer must census all
  active trait impls in the crate and reject if any lies outside this shape;
  `impl_universe` nevertheless remains `OPEN` globally.
- A recipe-backed value is a public, safe, non-variadic Rust-ABI free function
  with at least one generic parameter, representable parameter/return types,
  and only scope-free `REQUIRED` predicates naming complete marker traits in
  this artifact. Its sole body is the `RETURN_ARGUMENT` recipe below.
- An object-backed value is a public, non-generic, non-variadic `extern "C"`
  free function with exactly the `no_mangle` export attribute and whose
  parameter and return types are supported C-ABI scalar primitives. Its live
  Rust body must be admitted by the existing bounded
  all-local monomorphic C-emission subset; generic or otherwise deferred body
  obligations reject. It is bound to exactly one exported symbol and object
  member by `LINK`. Constants, statics, methods, inherent impls, and any other
  value or body shape are unsupported.
- Associated declarations, projections, function-pointer types, macros,
  general semantic attributes, evaluated consts, and native libraries are
  unsupported and retain the exact `ABSENT` states in the v3.2 column. The
  sole `no_mangle` export fact is normalized into the bidirectional
  `VALU`/`LINK` symbol binding and is not transported as a general attribute.

Completeness is not obtained by dropping an unsupported active record. The
encoder first censuses each active family, applies the structural profile to
every record, and emits nothing if any record cannot be represented. A
consumer validates the same restrictions; it never broadens a record merely
because the current HIR could represent more.

## Identity domains and canonical order

V3 has one authority for each kind of definition:

- `module_local` indexes `MODS`.
- `nominal_local` indexes `NOMD` and names a trait or trait alias.
- `associated_local` indexes `AITM` and names an associated declaration or an
  impl member.
- `generic_local` indexes `GPAR`.
- `type_local` indexes `TYPE`.
- `item_local` indexes ordinary type declarations in `ITEM`.
- `value_local` indexes free functions, constants, and statics in `VALU`.
- `impl_local` indexes `IMPL`.

V3.2 additionally defines `body_local` as the one-based index of a `BODY`
record, `link_object_local` as the one-based index of a `LINK` object record,
and `link_symbol_local` as the one-based index of a `LINK` symbol record.
These locals do not exist in v3.0/v3.1.

V3 does not retain a second `NREF` identity table. Predicate targets,
supertraits, alias bounds, projection trait owners, associated owners, impl
traits, and trait namespace bindings all use `nominal_local`. Associated
equalities and projection item identities use `associated_local` and must
name the exact declaration that owns the item. An impl member points back to
that declaration through `implemented_associated_local`.

The encoder first forms the complete capture closure, then assigns locals.
Modules are ordered by canonical module path. Named child definitions are
ordered by owner local, namespace, name bytes, kind tag, and authenticated
source ordinal. Unnamed impls are ordered by owner module and authenticated
source ordinal, with structural fields used only as a checked tie-breaker.
Associated items are ordered by parent identity and source ordinal. Generic
parameters retain declared index order. Runtime DefId allocation order or
pointer order must never be a sorting key.

Global set-like arrays use their referenced locals as canonical keys and must
be strictly increasing with no duplicates. Semantically ordered arrays retain
their order: generic parameters, supertraits, trait-alias `+` bounds,
associated children, function parameters, and predicate scopes. Associated
equalities within one bound are set-like and are strictly increasing by
`associated_local`. The decoder verifies order rather than repairing it.

## Section records

This section defines the required logical field order. Each variant starts
with a closed `u8` tag followed by zero reserved padding where needed. Names,
regions, constants, generic arguments, visibility, named trait references,
function signatures, and predicates are shared encodings and must have one
implementation on both producer and consumer paths.

### `CRAT` and `MODS`

`CRAT` contains the canonical crate name, edition, and root `module_local`.
These values must equal `MANF`. `MODS` retains the existing v2 module shape:
parent local or none, name, and canonical path-derived order. Exactly one
module has no parent and it is the `CRAT` root. Parent locals precede children.
For v3.2, equality maps `CRAT`'s existing closed edition tag to the required
calendar value in schema-2 `MANF`; it does not change the v3.0/v3.1 `CRAT`
encoding.

### `NOMD`: traits and trait aliases

Each record contains:

```text
u8  kind                            # TRAIT or TRAIT_ALIAS
u8  declaration_state               # COMPLETE_DECLARATION in v3.0/v3.1/v3.2
u16 reserved
module_local owner_module
string name
visibility
u32 source_ordinal
generic_local generic_start
u32 generic_count
u32 predicate_scope_start/count
u32 trait_predicate_start/count
u32 outlives_predicate_start/count
u32 associated_start/count
```

The stable nominal declaration states are `REFERENCE_ONLY = 1` and
`COMPLETE_DECLARATION = 2`; zero and other values reject. Exact v3.0/v3.1/v3.2
profiles accept only `COMPLETE_DECLARATION`.

`TRAIT` then contains safety, authenticated `is_auto`, authenticated
`is_const_trait`, and the ordered supertrait array. Each supertrait has
modifier, a complete named trait reference, associated equalities, and its
bound-position lifetime binder. `is_const_trait` is the exact lowered
`#[const_trait]` fact and belongs to the complete trait declaration rather
than the otherwise absent general semantic-attribute family. It is never
inferred from a predicate modifier. Const-dependent solving may still defer
when the selected profile lacks any other required const semantic fact.

`TRAIT_ALIAS` contains its ordered bound array. A bound is either a trait
bound with the same payload as a supertrait or an exact lifetime region.
Aliases have zero associated children. Their bound bodies are semantic
declarations in v3; unlike v2.6 opaque alias references, they may be expanded
only after the complete record and all referenced declarations are bound.

A `REFERENCE_ONLY` nominal schema is not accepted by the v3.0/v3.1/v3.2 profiles.
If a later dependency version permits one, it remains a RESERVED definition,
must not receive generic parameters or associated children in the global HIR,
must not appear in `NSPC`, and must not be passed to resolution, alias
expansion, projection, or the trait solver.

### `AITM`: associated declarations and impl members

The common prefix is:

```text
u8  kind                            # TYPE, CONST, METHOD
u8  parent_kind                     # NOMINAL or IMPL
u16 reserved
u32 parent_local
associated_local implemented_associated_local_or_none
string name
visibility
u32 source_ordinal
u8  is_specializable
generic_local generic_start
u32 generic_count
u32 predicate_scope_start/count
u32 trait_predicate_start/count
u32 outlives_predicate_start/count
```

For a nominal parent, `implemented_associated_local` is none. The parent must
be an ordinary trait; aliases cannot own associated items. For an impl parent,
the field is required for a trait impl and names a direct or inherited
declaration available through the implemented trait; it is none for an
inherent impl. Kind and name must agree with the named declaration.

Variant payloads are:

- `TYPE`: ordered associated-type bounds, optional default/value type, and a
  bit distinguishing a targetless trait declaration from an impl value. GAT
  parameters and their predicates are owned by this `associated_local`.
- `CONST`: declared type plus `has_default_body`. V3.0/v3.1/v3.2 do not carry the
  body; a true bit is a promise that a later body artifact may satisfy, not an
  evaluated value.
- `METHOD`: receiver kind, ordered parameter types, return type, ABI, safety,
  constness, asyncness, variadicness, and `has_default_body`. Parameter names
  and destructuring are not needed for a declaration-only consumer, but the
  ABI slot types and receiver are exact.

Associated-type bounds use exact trait references, equalities, REQUIRED or
RELAXED modifier, and lifetime binders. A projection or equality always names
the declaring `associated_local`; name lookup is never used to reconstruct
ownership.

### `GPAR`: generic parameters

Every record contains owner tag/local, declared index, kind, name, relaxed-
Sized bit, optional default argument, and the declared type required by const
parameters. Owner tags cover `NOMINAL`, `ASSOCIATED`, `ITEM`, `VALUE`, and
`IMPL`. Parameters for one owner are contiguous, start at index zero, and
appear in index order. Lifetime/type parameters have no declared type; const
parameters have one. Defaults may reference only earlier parameters and the
authenticated enclosing `Self` where the HIR permits it.

Every early-bound region and parameter type/const carries a `generic_local`
whose owner is in the lexical owner chain of the record using it. Late-bound
regions carry a binder index and must be beneath the predicate, bound, or
function-pointer binder that authenticates that index. Orphan, cross-owner,
or future-parameter references reject.

### `TYPE`: structural types

The v3.0 type tags include every v2.6 structural type plus these required
forms:

- `FN_POINTER`: binder arity, ordered parameter type locals, return type,
  ABI, safety, and variadicness.
- `SELF`: exact enclosing nominal or impl owner.
- `PROJECTION`: self type, named ordinary-trait reference, exact associated
  declaration, and ordered associated/GAT generic arguments.
- `DYN_TRAIT`: optional principal trait, its arguments and equalities,
  strictly ordered authenticated auto traits, and object lifetime.
- `FN_DEFINITION`: exact defining value/associated identity and generic
  arguments, accepted only when that definition family is complete.

Named ADT, alias, foreign, and function-definition types identify a definition
plus an exact lifetime/type/const argument list matching its generic schema.
An `OPAQUE` type requires its own identity, bounds, capture set, and hidden-
type disclosure policy; v3.0/v3.1/v3.2 have no such declaration record and reject
it rather than treating it as an argument-only named type. Constants initially
include exact scalar bits, an authenticated
generic parameter, or an authenticated unevaluated const definition with its
substitutions. Inference and error regions, inference/error constants,
inference/error types, and closure identities reject this cross-crate profile.
`ERASED` is a closed `LOWER_SAFE` transport marker, not a source lifetime
identity or binder; its generic-local and binder-index payloads are zero. It
is admitted only when every declaration root reaching it is an authenticated
profile boundary: (1) the shared or mutable reference receiver of a supported
associated `METHOD` and a receiver-related return type, never another method
input; or
(2) the sole immutable direct `&T` parameter of a body-bearing const free
`FUNCTION` with exactly one owner-local relaxed-Sized type generic, zero
predicates, and exact immutable `&'static str` return; or (3) both roots of the
exact body-bearing const free `fn<T>(&mut T) -> &mut [T; 1]` profile, after
lowering has authenticated that both source lifetimes are omitted and the
unary input therefore governs the output. The third profile's paired root
shape is the relation authority; structural reuse of either `ERASED` type is
not. Aggregate, variant, and alias fields, `CONST`/`STATIC` types, predicate
and outlives roots, arbitrary free parameters or returns, mixed-sharing roots,
explicit `'_`, named, or static input lifetimes, and remaining inference
regions reject. `ERASED` does not by itself encode equality, outlives evidence,
or a late-bound binder; artifacts that require lifetime-sensitive selection
outside an authenticated closed profile need a later exact representation.

Type records form a canonical DAG. Structural child type locals precede their
parents. Nominal recursive references occur through definition handles rather
than cyclic type-local edges. Projection validation checks that the associated
item is a TYPE declaration available from the named trait and that its GAT
arguments match the associated schema.

The bounded array-declaration profile also admits a const-parameter length.
Such an `ARRAY` names an exact owner-local `CONST` generic whose declared type
is primitive `usize`; scalar length bits and the const-parameter local are
mutually exclusive. The generic has no default. This is parameter identity,
not evaluation or unevaluated-constant transport.

### `ITEM` and `VALU`

`ITEM` retains extern types, structs, unions, enums, and free type aliases.
Records contain owner module, name, visibility, source ordinal, generic and
predicate ranges, and their existing structural payload. Fields and variants
retain semantic source order; enum variant identities are local to the item
and stable by source ordinal. Alias targets are exact `type_local` values.

Public roots may close over private, crate-visible, or exactly restricted
aggregate and trait declarations. Every such non-public record must be
transitively reachable through retained field, type, generic, predicate, or
associated-signature edges, must retain its exact visibility restriction, and
must have no `NSPC` entry. Orphan private records and namespace leakage reject.
Associated methods of a non-public trait remain structural HIR children; they
are not published as module values or public-library associated authority.

The current `IntoIter` profile retains a const `N: usize`, a
const-parameter-length array, the item-owned `DATA: PartialDrop` predicate,
the diagnostic and insignificant-destructor identities, and the reachable
non-public `PolymorphicIter`, `IndexRange`, and `PartialDrop` declarations.
Capture authenticates the source alias `InnerSized<T, N>` and records its
already-lowered normalized field type; it does not fabricate an alias item.
Impls, drop glue, iterator behavior, method bodies, and executable authority
remain absent and must fail closed in consumers that require them.

`VALU` retains free functions, constants, and statics. Its common prefix is
the same owner/name/visibility/source/generic/predicate information. Functions
carry the exact declaration signature used by `AITM` methods. Constants and
statics carry type and mutability; body/default presence is recorded but no
body ID crosses the boundary.

V3.0 must not silently skip any active public value merely because it has a
body. It transports the declaration and records body presence. A consumer
requiring execution checks the `BODIES_CONST_IR` family and rejects.

The current `from_fn` declaration profile retains the ordered value-owned
generics `T`, `const N: usize`, and `F`, the parameter `F`, the return type
`[T; N]`, and the exact predicate
`F: FnMut<(usize,), Output = T>`. Its reachable nominal closure contains the
complete `Tuple`, `FnOnce`, and `FnMut` declarations, the ordered
`FnMut: FnOnce` supertrait edge, targetless `FnOnce::Output`, the `call_once`
and `call_mut` rust-call method declarations, the inherited associated-type
availability relation, and the projection types used by those method
signatures. Associated equalities retain the declaring associated DefId across
crate boundaries; they are never reconstructed by name or forced to the
consumer crate. Capture authenticates the stable/inline source attributes and
the source-owned body but projects them from declaration bytes. Materialization
binds the function with `BODY_NONE`: importing and inspecting the declaration
is supported, while MIR, monomorphization, execution, and code generation fail
closed until an executable family transports that authority.

The current `from_mut` profile similarly transports only its declaration. It
retains ordinary `T`, one mutable erased reference parameter to `T`, and one
mutable erased reference return to the scalar-length array `[T; 1]`. Capture
requires the exact source-owned stable const function and its
`rustc_const_stable` provenance, while lowering admits the paired erased roots
only after authenticating the sole omitted input/output lifetime relation.
The body remains `BODY_NONE` after materialization.

V3.2 appends this execution binding to every `VALU` function record after the
unchanged declaration payload:

```text
u8  execution_kind                  # BODY_RECIPE or NATIVE_OBJECT
u8  reserved[3]                     # zero
u32 execution_local                 # body_local or link_symbol_local
```

The stable tags are `BODY_RECIPE = 1` and `NATIVE_OBJECT = 2`; all other tags
reject. `BODY_RECIPE` and `NATIVE_OBJECT` records form disjoint sets and the
pointed-to record must point back to exactly this `value_local`. Every v3.2
value has exactly one execution binding. This appended payload is absent, not
zero-filled, in v3.0/v3.1, preserving their byte layout.

### `PRED`: scopes, trait predicates, and outlives predicates

`PRED` starts with three counts followed by the three arrays:

```text
u32 scope_count
u32 trait_predicate_count
u32 outlives_predicate_count
[scope]
[trait_predicate]
[outlives_predicate]
```

Every record carries an owner tag/local and owner-relative ordinal. A scope
contains subject kind/value, binder arity, and exact ranges of the atomic
trait and outlives predicates expanded from it. A trait predicate contains
subject type, named trait-or-alias reference, strictly ordered associated
equalities, scope local or none, bound-position binder arity, and the closed
modifier `REQUIRED`, `CONST_IF_CONST`, or `CONST`. Alias predicates may not
carry direct associated equalities; their complete alias definition supplies
the expansion. An outlives predicate contains type-or-lifetime subject, bound
region, and scope local or none.

Predicate modifiers are declaration facts. They do not prove const-trait
eligibility unless a later semantic capability transports and authenticates
that trait fact. Scoped and bound-position binders are mutually exclusive in
the same way as the live HIR. Counts/ranges must partition each owner's
records exactly; no predicate can be orphaned or shared between owners.

### `IMPL`: v3.1/v3.2 impl facts

The v3.0 section is the canonical zero-record payload. Each v3.1/v3.2 record has:

```text
module_local owner_module
u32 source_ordinal
u8  kind                            # TRAIT or INHERENT
u8  safety
u8  polarity                        # POSITIVE, NEGATIVE, or RESERVATION
u8  defaultness                     # FINAL or DEFAULT
u8  constness                       # NON_CONST or CONST
u8  reserved[3]                     # zero
type_local self_type
nominal_local trait_or_none
[generic_arg] trait_arguments
generic_local generic_start
u32 generic_count
u32 predicate_scope_start/count
u32 trait_predicate_start/count
u32 outlives_predicate_start/count
associated_local member_start
u32 member_count
```

Inherent impls have no trait and must be positive. Trait impls name an ordinary
trait, never an alias, and carry its complete arguments. Negative impls have
no members. Direct members form one contiguous `AITM` range and agree with
their parent and declaration mappings. Per-member specialization is retained
by `AITM.is_specializable`; containing `default impl` semantics use the impl
defaultness field. If the current HIR has not authenticated const-impl or
specialization state required by an active core impl, the producer rejects
instead of writing a default value.

V3.1 `COMPLETE_ACTIVE_CFG` includes every active trait impl in the crate,
because even a syntactically private module's impl participates in selection.
It also includes every active inherent impl and direct member in the crate;
filtering private inherent impls is not compatible with
`COMPLETE_ACTIVE_CFG`. The manifest may prove this crate's census complete, but
`impl_universe` remains `OPEN`: a missing match returns
`CM_TRAIT_SOLVER_DEFERRED_METADATA`, never `NO_SOLUTION`. Exact transported
negative impls may refute only where the existing solver's authenticated
negative-proof rules permit it. A reservation record is a declaration and
coherence fact only: it permits overlap but is neither positive selection
evidence nor a negative proof. Legacy formats and semantic consumers that
cannot preserve that distinction reject the record rather than remapping its
polarity. Reservation-owned `AITM` records are structural declarations only:
they are never selection or projection evidence and cannot be instantiated,
lowered to MIR, or code-generated.

V3.2 uses this same record layout but accepts only the positive marker-impl
shape in its exact capture domain. It still performs the complete active impl
census before rejecting negative, reservation, default, const, generic,
predicated, associated-member, or otherwise unsupported records.

### `NSPC`: effective public namespace

Each record contains owner module, namespace (`TYPE` or `VALUE`), name, target
tag/local, visibility, and canonical source/export ordinal. A `MACRO`
namespace tag is invalid in v3.0/v3.1/v3.2 because `MACROS` is `ABSENT`.
Targets are `MODULE`, `ITEM`, `VALUE`, `NOMINAL`, `PRIMITIVE`, or the bounded
`ENUM_VARIANT` target. Ordinary traits and trait aliases use `NOMINAL`; there
is no fake type item for them. `ENUM_VARIANT` uses the canonical flattened
variant local obtained by walking ENUM ITEMs in ITEM-local order and their
variants in retained source order. Its namespace record retains whether the
alias inhabits TYPE or VALUE; constructible unit variants require exact paired
TYPE/VALUE aliases, and no free ITEM or VALU is synthesized.
In v3.0/v3.1, reexports point to the same canonical target handle as the
defining entry. V3.2 applies its narrower exact capture rule above and rejects
all reexports.

Records are ordered by module, namespace, name bytes, and export ordinal.
Conflicting effective public bindings reject. Every nonprimitive target must
be a complete bound declaration in this artifact. Namespace binding occurs
last during decode so resolution can never observe a half-bound target.

### `BODY`: v3.2 generic execution recipes

V3.0/v3.1 have no `BODY` section. Every v3.2 record is:

```text
u8  recipe_tag                      # RETURN_ARGUMENT
u8  reserved[3]                     # zero
value_local owner_value
u32 parameter_index                 # zero-based source parameter index
type_local parameter_type
type_local return_type
```

The sole stable recipe tag is `RETURN_ARGUMENT = 1`. Records are strictly
ordered by `owner_value`, which is unique. `parameter_type` must equal both
the selected parameter's declared type local and the function's return type
local. The owner must satisfy the recipe-backed value restrictions above and
must have at least one type generic parameter used by that common type; a
monomorphic identity function is not this cross-crate instantiation proof.

The producer admits `RETURN_ARGUMENT` only when the normalized local body is
exactly one block with zero statements and a tail expression that is a direct
read of the selected argument local. No explicit return node, binding,
coercion, cast, borrow, dereference, call, control flow, unsafe operation,
drop, adjustment, or hidden temporary is permitted. The body must belong to
`owner_value` in the live HIR; a declaration synthesized from a name or
signature cannot acquire a recipe. The artifact's source digest and the
bidirectional `VALU`/`BODY` references bind that structural proof to the
transported definition.

On instantiation the consumer substitutes the imported function's generic
arguments into the authenticated parameter/return type, proves every imported
predicate using ordinary trait selection, and only then lowers the recipe to
its own MIR/C. It must select the transported impl fact for the acceptance
instance; directly returning the argument while skipping predicate checking
does not satisfy v3.2. Recipe bodies have metadata provenance consisting of
the dependency artifact identity, owner `value_local`, and `body_local`.
They must not be relabeled as a local source body or given a fabricated source
span.

### `LINK`: v3.2 object and symbol binding

V3.0/v3.1 have no `LINK` section. V3.2 uses this payload:

```text
u32 object_count
[object_record]
u32 symbol_count
[symbol_record]
u32 native_library_count            # exactly zero
```

An object record is:

```text
string archive_member_name
u64 byte_length
u8  digest_algorithm                # SHA256
u8  reserved[3]                     # zero
bytes[32] object_digest
link_symbol_local symbol_start
u32 symbol_count
```

A symbol record is:

```text
value_local owner_value
link_object_local object
string external_symbol
u8  linkage                         # EXACT_EXTERNAL
u8  reserved[3]                     # zero
```

The stable link constants are `SHA256 = 1` and `EXACT_EXTERNAL = 1`.
Object records are strictly ordered by member-name bytes; symbol records are
strictly ordered by `object`, symbol bytes, and then `owner_value`. Object
symbol ranges partition the symbol table exactly and agree with each symbol's
object local.
Each owner is unique, is an object-backed v3.2 value, and points back through
its `VALU.execution_local`. External symbols are nonempty portable C external
identifiers and unique across the artifact. `LINK` records the exact external
symbol required by the value's sole `no_mangle` attribute. Other export-name
or semantic attributes are unsupported because `SEMANTIC_ATTRIBUTES` is
`ABSENT`; the exact export fact is consumed into `LINK`, not reconstructed as
a downstream HIR attribute. Object records with no referenced symbol reject.

The executable `.rlib` is a deterministic short-name System V archive. It
contains exactly one member named `cmrustc.rmeta`, whose bytes are this v3.2
metadata envelope, plus exactly the object members named by `LINK`. Member
names are 1 through 15 ASCII bytes from `[A-Za-z0-9_.-]`, are bytewise sorted,
and must not otherwise equal `cmrustc.rmeta`. Extended-name tables, symbol
index members, duplicate members, undeclared members, and trailing archive
bytes reject. Each member uses canonical zero timestamp/uid/gid, mode `0644`,
decimal size without leading zeroes, and the required newline pad for odd
length. A consumer extracts verified object bytes to private staging files and
passes those exact files explicitly to the target linker; it does not depend
on an archive symbol index.

Before HIR allocation the consumer requires every declared archive member to
exist exactly once with the declared length and SHA-256 digest, and requires
that no other member exist. A member from another artifact, a renamed member,
or a metadata/object mix-and-match therefore rejects. The object format and
machine must agree with the manifest target when the target object reader can
preflight them; the target linker remains an additional fail-closed check, not
a substitute for identity and digest validation.

## Encoder collection rules

Collection is by family, not by a catch-all item loop:

1. Collect modules and the complete effective public type/value namespace;
   reject macro entries in these profiles.
2. Collect every declaration directly exported by that namespace.
3. Close transitively over field/signature types, generic defaults,
   predicates, supertraits, alias bounds, associated declarations,
   projection owners, and associated availability.
4. In v3.1 and v3.2, collect all active trait impls and the required inherent impl
   domain, then close over their self types, traits, generics, predicates, and
   direct members.
5. Compare every family census with library capture before encoding and reject
   unsupported or unclassified HIR.

For v3.2, steps 2 through 4 use only its exact capture domain. The producer
then validates each recipe against the owning live body, compiles each
object-backed value for the manifest target, computes the canonical source,
object, and artifact digests, and constructs `BODY`, `LINK`, metadata, and the
archive in scratch storage. It publishes no `.rmeta`, object, archive, or
partial replacement if any census, compiler, digest, or archive step fails.
The native compiler command is an accounted build input and must be invoked
with deterministic-output options appropriate to the target; unsupported
nondeterministic object formats reject this profile.

Top-level impls are not `ITEM` records. Traits and aliases are not omitted
from `NOMD` merely because no exported function predicate mentions them.
Trait namespace entries are collected from the same nominal table used by
predicates and projections. This prevents the v2 structural failure mode in
which impls reach an unsupported item switch and public traits have no
namespace target domain.

## Atomic decode and bind order

Decode has a read-only preflight followed by one rollback-protected bind:

1. Validate envelope, exact version, flags, payload CRC, fixed section tags,
   lengths, counts, and absence of trailing bytes.
2. Parse `MANF`, enforce the caller's capability requirements, and validate
   target/cfg compatibility. No context allocation has occurred yet.
3. Decode every section into scratch wire records using checked additions and
   multiplications. Zero counts produce null pointers and never rely on
   `malloc(0)`.
4. Validate canonical order, owner ranges, local references, generic
   provenance, binder provenance, type reachability, projection ownership,
   family states/counts/CRCs, impl/member mappings, namespace targets, and all
   aggregate limits. No runtime IDs exist yet.
5. Mark every mutable HIR/interner/library arena and preserve the caller's
   artifact/output sentinels.
6. Reserve the crate, modules, and all definition identities in canonical
   local order: ordinary items/values, nominals, associated items, and impls.
   A reserved record retains its promised item kind but is not visible through
   namespace lookup.
7. Reserve generic-parameter runtime IDs with their authenticated owner, kind,
   and index, but do not publish defaults or const declared types. Materialize
   types in verified DAG order using those reserved IDs, then finalize every
   generic payload. This explicit two-phase step breaks the real dependency
   cycle in which types refer to parameters while const parameter declarations
   and generic defaults refer to types.
8. Bind ordinary items/values, complete nominal declarations, associated
   items, and impls. Build alias/projection availability and semantic indexes
   only after all participating parents and children are bound.
9. Bind `NSPC`, validate the resulting library artifact and family census,
   then swap it into the caller's artifact and commit all marks.

Any failure after step 5 restores every context/interner/library mark, frees
all partial scratch arrays exactly once, and leaves the caller's artifact and
output unchanged. No global trait index, generic parameter, fake namespace
entry, interned wire name, or RESERVED definition may survive rollback.

For v3.2, archive and identity preflight precedes step 1: validate the complete
archive framing, locate the sole metadata member, validate the v3.2 envelope
and manifest identity, and validate every declared object member and symbol
binding. `BODY` and `LINK` scratch records join steps 3 and 4. Step 8 binds
metadata-origin recipes and imported impl evidence only after their owners are
complete; link inputs remain immutable scratch views until the HIR transaction
commits. Namespace publication still occurs last.

Consumer C emission, object extraction, linking, and executable publication
are a second transaction. All files are created under a private staging name;
the requested output path is replaced only after successful target linking.
Any stale/source/target mismatch, corrupt or incomplete member, unsupported
instantiation, missing trait proof, C compiler error, or linker error removes
the staging outputs and leaves every requested output sentinel unchanged.

## Resource and canonicality requirements

V3 initially retains the audited v2 limits unless a measured core census
requires an explicit versioned increase:

- payload: 64 MiB;
- modules: 4,096;
- ordinary items: 65,536;
- generic parameters: 131,072;
- types: 262,144;
- namespace entries: 131,072;
- nominal declarations: 65,536;
- impls: 131,072;
- values, associated items, predicates, and graph edges: 131,072 each;
- one string: 1 MiB;
- structural nesting or one nominal-expansion traversal: 1,024 nodes.

V3.2 additionally limits recipes and link symbols to 131,072 each, object
members to 15, one object member to 128 MiB, and the complete archive to
256 MiB. Together with the mandatory metadata member this preserves the
archive-wide limit of 16 members. The metadata payload itself remains subject
to the 64 MiB envelope limit. Checked `u64` archive arithmetic must narrow to
`size_t` only after proving representability.

The decoder checks raw counts before allocation and aggregate sums before
materializing child arrays. Total generic arguments, binder slots,
equalities, predicate edges, fields, variants, parameters, associated bounds,
supertrait/alias edges, impl members, and namespace records each have explicit
caps; per-record caps do not replace aggregate caps.

All recursive validation uses memoized tri-state traversal or an explicit
stack. A doubled-child DAG is visited once per local, not exponentially.
Supertrait/trait-alias expansion uses a separate active-stack bit to reject an
expansion cycle; ordinary references from signatures or associated values do
not become expansion edges and may refer back to their declaring trait.
Encoder collection obeys the same 1,024-node expansion bound and cycle
semantics as decoder validation.

Canonical set construction may sort once in `O(E log E)` and deduplicate with
an adjacent pass. Decoder validation is linear over already sorted records.
Repeated linear searches inside equality, nominal-closure, or impl loops are
forbidden; use side indexes or binary search whose storage remains alive for
the complete operation. All side indexes are scratch-owned and are released
on success and failure.

## Acceptance tests

No v3 milestone is complete until all applicable tests below pass in fresh
producer and consumer processes.

### Common codec and transaction tests

- Two isolated producers lower the same fixture with perturbed discovery
  order and emit byte-identical artifacts.
- Decode then re-encode is byte-identical with newly allocated runtime IDs.
- Every section truncation, duplicate/reordered/unknown tag, unknown wire enum,
  nonzero reserved field, bad CRC, trailing byte, zero required local,
  out-of-range local, noncanonical order, count overflow, aggregate overflow,
  1,025-node traversal, and expansion cycle rejects.
- Wide shared DAGs complete within the linear/log-linear complexity contract;
  they neither recurse exponentially nor exceed the bound by double-counting
  shared children.
- Failure at every allocation/bind step leaves context counts, interner,
  solver indexes, artifact sentinel, and output sentinel unchanged under
  ASan/UBSan/LSan.
- Exact v2.3-v2.6 and v1.0/v1.1 fixtures retain their prior decode and
  re-encode behavior. Retagging legacy bytes as v3 rejects.

### V3.0 `LOWER_SAFE`

- A dependent fixture resolves a public ordinary trait, a trait alias, every
  associated kind, a reexport, and a generic public value from only decoded
  metadata.
- `Fn`, `FnMut`, `FnOnce`, and `FnOnce::Output` retain distinct exact
  identities; a parenthesized `Fn(..) -> bool` equality points to
  `FnOnce::Output`.
- Projection fixtures cover `<T as Pointee>::Metadata`, inherited associated
  availability, GAT arguments, wrong-owner rejection, and wrong-schema
  rejection.
- Function-pointer fixtures cover safe/unsafe ABI, variadic rejection where
  the target ABI forbids it, late-bound input/output regions, and public
  `RawWakerVTable`-shaped fields.
- An opaque/reference-only trait fixture creates no bound item, generic
  parameter, namespace binding, alias expansion, projection fact, or solver
  evidence and fails the `LOWER_SAFE` capability check.
- The Rust 1.90 core producer emits nonempty bytes with every required v3.0
  family complete; a fresh consumer resolves a fixture using a generic type,
  public value, trait, alias, and associated projection. Unsupported active
  declarations fail with zero replacement bytes.

### V3.1 `SEMANTIC_POSITIVE`

- Fixtures cover generic positive trait impls, inherent methods, associated
  type/const/method mappings, negative impls, member specialization flags,
  and impl predicates/outlives.
- A fresh consumer selects one authenticated positive impl and normalizes its
  associated type. Removing that impl changes the result to
  `DEFERRED_METADATA`, not `NO_SOLUTION`.
- A malformed impl/member mapping, alias target, polarity/defaultness,
  associated owner, or incomplete impl-family census rejects atomically.
- Complete v3.0 bytes do not satisfy a semantic-positive capability request;
  exact v3.1 bytes do not make the global impl universe closed.

### V3.2 `EXECUTABLE_SLICE`

- An isolated producer emits a nonempty deterministic archive containing
  nonempty metadata and at least one nonempty object member. A second producer
  in a different working directory and with perturbed discovery/allocation
  order emits byte-identical metadata, objects, and archive.
- In a fresh process with no producer HIR or sidecar state, a consumer loads
  only that archive, resolves a public generic recipe function and its marker
  trait, selects the transported positive impl for a concrete primitive
  substitution, instantiates `RETURN_ARGUMENT`, and emits C for the call.
- The same consumer also calls an object-backed public function from the
  artifact. Strict GCC and TinyCC builds link the verified member and execute
  the same expected result. The cmrustc paths used for both runs contain no
  invocation of, library from, or hidden declaration synthesized by the C++
  mrustc implementation.
- Clang ASan/UBSan/LSan runs the producer, isolated consumer, all rejection
  cases, and the resulting executable where compatible with the target.
- Corrupt metadata, corrupt object bytes, truncation at every archive/member
  boundary, duplicate/reordered/extra/missing members, wrong length/digest,
  wrong symbol owner, incomplete family census, stale source digest, wrong
  expected crate/disambiguator, and wrong edition/target/layout/panic/cfg all
  reject atomically and publish neither HIR state nor output files.
- Changing declaration names while retaining the same supported structural
  shapes still succeeds, and changing a supported shape rejects or changes
  canonical bytes as specified. This guards against fixture-name special
  cases, unconditional coherence exemptions, and fake imported declarations.
- Exact v3.0 and v3.1 fixtures remain byte-identical and retain their existing
  semantics. Neither can satisfy an executable capability request; exact v3.2
  does not claim macro, general body, const-evaluation, native-library, or
  closed-world impl capability.

### Later alloc and general execution gates

A future exact v3 minor must add complete macro/reexport provenance and the
allowlisted lang-item, intrinsic, and semantic attributes before claiming
alloc-source consumption. It must also generalize v3.2's body/link capability
to the complete required body/MIR, evaluated-const, object, and native-link
closure before a `core` artifact is called generally executable. V3.2's exact
slice is an executable `.rlib` only for its declared structural profile.

The final authority is always the deepest artifact consumed without producer
state. A successful census, zero-error capture, or nonempty capture-only file
does not satisfy these downstream gates.
