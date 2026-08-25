# cmhir-meta-v3: downstream-safe declaration and semantic metadata

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

Version 3 has four immediate goals:

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

The v3.0 acceptance boundary is fresh-context declaration lowering and name
resolution. The v3.1 acceptance boundary additionally permits positive trait
selection and associated-type normalization from authenticated facts.

## Non-goals

V3.0 and v3.1 do not transport macro definitions, expanded token streams,
generic bodies, MIR, evaluated const bodies, native objects, dependency
archives, or link directives. They are not complete `.rmeta` or `.rlib`
formats and are not by themselves sufficient to build `alloc`.

In particular, omitting impls, traits, aliases, associated declarations, or
namespace bindings and retaining only RESERVED nominal headers is a useful
capture/census checkpoint, but it is not an alloc-consumable artifact. Such an
artifact must advertise `REFERENCE_ONLY` or `ABSENT` for the affected
families. A consumer requiring declaration lowering or semantic selection
must reject it before changing its HIR context.

External dependency identities are also outside the first v3 boundary. A v3
producer encountering a declaration edge to another crate must either resolve
it through a future authenticated dependency section/version or reject. It
must not create a local definition that merely resembles the dependency item.

## Envelope and exact version dispatch

V3 reuses the existing uncompressed envelope, explicit little-endian integer
encoding, section frames, payload limit, and CRC-32 integrity check. Flags are
zero. The major/minor pairs are exact:

- `3.0` is the `LOWER_SAFE` declaration profile.
- `3.1` is the `SEMANTIC_POSITIVE` profile.

The v3 decoder first reads the envelope once, then dispatches on the exact
pair. Unknown v3 minors return unsupported version. A CRC failure, malformed
section, invalid manifest, or semantic failure after matching `3.x` must not
fall through to another v3 minor or any v2/v1 reader. Legacy readers and bytes
remain unchanged.

Every v3 artifact contains exactly these sections in this order:

```text
CRAT MANF MODS NOMD AITM GPAR TYPE ITEM VALU PRED IMPL NSPC
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

`MANF` is checked before any HIR allocation. Its fixed prefix is:

```text
u8  manifest_schema                 # exactly 1
u8  profile                         # LOWER_SAFE or SEMANTIC_POSITIVE
u8  impl_universe                   # OPEN only in v3.0/v3.1
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

The stable manifest wire constants are:

```text
profile:       LOWER_SAFE = 1, SEMANTIC_POSITIVE = 2
impl_universe: OPEN = 0
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
records. An absent family uses the canonical empty stream. Shared supporting
`GPAR`, `TYPE`, and `PRED` records are protected by the envelope CRC and are
validated through owner ranges and reachability; they are not ambiguously
charged to several family CRCs. The encoder computes family descriptors after
constructing canonical section payloads. CRCs provide corruption detection,
not a trust signature.

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

| Family | Sections | v3.0 | v3.1 |
| --- | --- | --- | --- |
| `MODULE_NAMESPACE` | `MODS`, type/value `NSPC` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TYPE_ITEMS` | `ITEM`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `VALUES` | `VALU`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TRAIT_DECLARATIONS` | `NOMD`, `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TRAIT_ALIASES` | `NOMD`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `ASSOCIATED_DECLARATIONS` | `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `PROJECTION_TYPES` | `TYPE`, `NOMD`, `AITM` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `FUNCTION_POINTER_TYPES` | `TYPE` | `COMPLETE_ACTIVE_CFG` | `COMPLETE_ACTIVE_CFG` |
| `TRAIT_IMPLS` | `IMPL`, impl-owned `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `ABSENT` | `COMPLETE_ACTIVE_CFG` |
| `INHERENT_IMPLS` | `IMPL`, impl-owned `AITM`, relevant `GPAR`, `TYPE`, `PRED` | `ABSENT` | `COMPLETE_ACTIVE_CFG` |
| `MACROS` | none in v3.0/v3.1 | `ABSENT` | `ABSENT` |
| `SEMANTIC_ATTRIBUTES` | none in v3.0/v3.1 | `ABSENT` | `ABSENT` |
| `BODIES_CONST_IR` | none in v3.0/v3.1 | `ABSENT` | `ABSENT` |
| `LINK_INPUTS` | none in v3.0/v3.1 | `ABSENT` | `ABSENT` |

`LOWER_SAFE` consumers require the first eight families to be
`COMPLETE_ACTIVE_CFG`. `SEMANTIC_POSITIVE` consumers additionally require both
impl families to be complete and `profile == SEMANTIC_POSITIVE`. An alloc
source consumer must additionally require complete macro and semantic-
attribute families in a later exact version. A codegen/link consumer must also
require bodies/const IR and link inputs. These checks occur before reserving a
crate or interning a wire string.

`COMPLETE_ACTIVE_CFG` requires exact census agreement between the manifest,
section counts, and the producer's library-capture manifest. An encoder may
not obtain the state by filtering unsupported records. Encountering one
unsupported active record rejects the complete encode transaction.

For v3.0/v3.1, `MODULE_NAMESPACE` completeness is deliberately scoped to the
module tree and effective `TYPE` and `VALUE` namespaces. It makes no claim
about the macro namespace. Macro definitions and macro namespace bindings are
both represented by the separate `MACROS == ABSENT` family; an `NSPC` macro
record is invalid in these profiles.

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

### `NOMD`: traits and trait aliases

Each record contains:

```text
u8  kind                            # TRAIT or TRAIT_ALIAS
u8  declaration_state               # COMPLETE_DECLARATION in v3.0/v3.1
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
`COMPLETE_DECLARATION = 2`; zero and other values reject. Exact v3.0/v3.1
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

A `REFERENCE_ONLY` nominal schema is not accepted by the v3.0/v3.1 profiles.
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
- `CONST`: declared type plus `has_default_body`. V3.0/v3.1 do not carry the
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
type disclosure policy; v3.0/v3.1 have no such declaration record and reject
it rather than treating it as an argument-only named type. Constants initially
include exact scalar bits, an authenticated
generic parameter, or an authenticated unevaluated const definition with its
substitutions. Inference, erased/error regions, inference/error constants,
inference/error types, and closure identities reject this cross-crate profile.

Type records form a canonical DAG. Structural child type locals precede their
parents. Nominal recursive references occur through definition handles rather
than cyclic type-local edges. Projection validation checks that the associated
item is a TYPE declaration available from the named trait and that its GAT
arguments match the associated schema.

### `ITEM` and `VALU`

`ITEM` retains extern types, structs, unions, enums, and free type aliases.
Records contain owner module, name, visibility, source ordinal, generic and
predicate ranges, and their existing structural payload. Fields and variants
retain semantic source order; enum variant identities are local to the item
and stable by source ordinal. Alias targets are exact `type_local` values.

`VALU` retains free functions, constants, and statics. Its common prefix is
the same owner/name/visibility/source/generic/predicate information. Functions
carry the exact declaration signature used by `AITM` methods. Constants and
statics carry type and mutability; body/default presence is recorded but no
body ID crosses the boundary.

V3.0 must not silently skip any active public value merely because it has a
body. It transports the declaration and records body presence. A consumer
requiring execution checks the `BODIES_CONST_IR` family and rejects.

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

### `IMPL`: v3.1 impl facts

The v3.0 section is the canonical zero-record payload. Each v3.1 record has:

```text
module_local owner_module
u32 source_ordinal
u8  kind                            # TRAIT or INHERENT
u8  safety
u8  polarity                        # POSITIVE or NEGATIVE
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
negative-proof rules permit it.

### `NSPC`: effective public namespace

Each record contains owner module, namespace (`TYPE` or `VALUE`), name, target
tag/local, visibility, and canonical source/export ordinal. A `MACRO`
namespace tag is invalid in v3.0/v3.1 because `MACROS` is `ABSENT`.
Targets are `MODULE`, `ITEM`, `VALUE`, `NOMINAL`, or `PRIMITIVE`. Ordinary
traits and trait aliases use `NOMINAL`; there is no fake type item for them.
Reexports point to the same canonical target handle as the defining entry.

Records are ordered by module, namespace, name bytes, and export ordinal.
Conflicting effective public bindings reject. Every nonprimitive target must
be a complete bound declaration in this artifact. Namespace binding occurs
last during decode so resolution can never observe a half-bound target.

## Encoder collection rules

Collection is by family, not by a catch-all item loop:

1. Collect modules and the complete effective public type/value namespace;
   reject macro entries in these profiles.
2. Collect every declaration directly exported by that namespace.
3. Close transitively over field/signature types, generic defaults,
   predicates, supertraits, alias bounds, associated declarations,
   projection owners, and associated availability.
4. In v3.1, collect all active trait impls and the required inherent impl
   domain, then close over their self types, traits, generics, predicates, and
   direct members.
5. Compare every family census with library capture before encoding and reject
   unsupported or unclassified HIR.

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

### Later alloc and execution gates

A future exact v3 minor must add complete macro/reexport provenance and the
allowlisted lang-item, intrinsic, and semantic attributes before claiming
alloc-source consumption. A later body/link profile must carry bodies or an
explicit codegen recipe, evaluated const requirements, object members, and
native link inputs before the artifact is called an executable `.rlib`.

The final authority is always the deepest artifact consumed without producer
state. A successful census, zero-error capture, or nonempty capture-only file
does not satisfy these downstream gates.
