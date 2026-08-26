#ifndef CMRUSTC_CM_HIR_DECLARATION_CAPTURE_H
#define CMRUSTC_CM_HIR_DECLARATION_CAPTURE_H

#include "cm/hir/artifact_config.h"
#include "cm/hir/declaration_metadata.h"
#include "cm/hir/library.h"
#include "cm/hir/module_map.h"

typedef enum CmHirDeclarationCaptureStatus {
    CM_HIR_DECL_CAPTURE_OK = 0,
    CM_HIR_DECL_CAPTURE_INVALID_ARGUMENT,
    CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY,
    CM_HIR_DECL_CAPTURE_LIBRARY_FAILURE,
    CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR,
    CM_HIR_DECL_CAPTURE_METADATA_FAILURE
} CmHirDeclarationCaptureStatus;

typedef enum CmHirDeclarationCaptureStage {
    CM_HIR_DECL_CAPTURE_STAGE_NONE = 0,
    CM_HIR_DECL_CAPTURE_STAGE_INPUT,
    CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY,
    CM_HIR_DECL_CAPTURE_STAGE_LIBRARY,
    CM_HIR_DECL_CAPTURE_STAGE_MODULES,
    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
    CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
    CM_HIR_DECL_CAPTURE_STAGE_IDENTITY,
    CM_HIR_DECL_CAPTURE_STAGE_MODULE_METADATA,
    CM_HIR_DECL_CAPTURE_STAGE_ITEM_METADATA,
    CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA,
    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE_METADATA,
    CM_HIR_DECL_CAPTURE_STAGE_VALIDATE,
    CM_HIR_DECL_CAPTURE_STAGE_FINAL_AUTHORITY
} CmHirDeclarationCaptureStage;

typedef enum CmHirDeclarationCaptureReason {
    CM_HIR_DECL_CAPTURE_REASON_NONE = 0,
    CM_HIR_DECL_CAPTURE_REASON_INVALID_ARGUMENT,
    CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH,
    CM_HIR_DECL_CAPTURE_REASON_CRATE_NOT_FOUND,
    CM_HIR_DECL_CAPTURE_REASON_LIBRARY_REJECTED,
    CM_HIR_DECL_CAPTURE_REASON_OWNED_DATA_MISSING,
    CM_HIR_DECL_CAPTURE_REASON_MODULE_CENSUS_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT,
    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_MODULE_MISSING,
    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_AUTHORITY_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_NAME_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_LIBRARY_MISMATCH,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_INTRODUCTION_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH,
    CM_HIR_DECL_CAPTURE_REASON_BINDING_DUPLICATE,
    CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND,
    CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_REQUIRED_ITEMS_MISSING,
    CM_HIR_DECL_CAPTURE_REASON_IDENTITY_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_MODULE_METADATA_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_PREDICATE_UNSUPPORTED,
    CM_HIR_DECL_CAPTURE_REASON_TYPE_METADATA_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_TARGET_UNMAPPED,
    CM_HIR_DECL_CAPTURE_REASON_METADATA_INVALID,
    CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_CHANGED
} CmHirDeclarationCaptureReason;

typedef enum CmHirDeclarationCaptureSemanticAttributes {
    /* No semantic attribute was omitted from the v3.0 descriptor. */
    CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE = 0,
    /*
     * v3.0 declares SEMANTIC_ATTRIBUTES absent.  The capture explicitly
     * projected authenticated crate/module attributes and, on supported UNIT
     * and named aggregates/bounded unit-variant enums/type aliases/free
     * consts/reexports, only the stricter LOWER_SAFE allowlist documented on
     * the capture entry point.
     * This does not call any projected attribute inert.
     */
    CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION = 1
} CmHirDeclarationCaptureSemanticAttributes;

typedef struct CmHirDeclarationCaptureInput {
    const CmHirContext *hir;
    CmHirCrateId crate_id;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    const CmImportResolver *imports;
    const CmHirModuleMap *modules;
    const CmHirArtifactConfig *configuration;
    CmHirArtifactBytes crate_disambiguator;
    CmHirArtifactBytes target_triple;
    CmHirArtifactBytes data_layout;
} CmHirDeclarationCaptureInput;

typedef struct CmHirDeclarationCaptureResult {
    CmHirDeclarationCaptureStatus status;
    CmHirDeclarationMetadataStatus metadata_status;
    CmHirLibraryStatus library_status;
    CmHirItemId rejected_item;
    CmHirTypeId rejected_type;
    size_t module_count;
    size_t trait_count;
    size_t item_count;
    size_t value_count;
    size_t predicate_count;
    size_t namespace_count;
    CmHirDeclarationCaptureSemanticAttributes semantic_attributes;
    size_t projected_semantic_attribute_count;
    /* Append-only first-failure diagnostic; all fields are borrowed/scalar. */
    CmHirDeclarationCaptureStage failure_stage;
    CmHirDeclarationCaptureReason failure_reason;
    int has_rejected_binding;
    CmHirLibraryBindingKind rejected_binding_kind;
    CmAstItemKind rejected_ast_item_kind;
    CmResolveNamespace rejected_namespace_kind;
    CmHirDefId rejected_definition;
    int has_rejected_target;
    CmResolveItemRef rejected_source_item;
    int has_rejected_span;
    CmSpan rejected_span;
} CmHirDeclarationCaptureResult;

/*
 * Capture the exact first bounded v3.0 LOWER_SAFE declaration slice from one
 * successfully lowered graph revision.  Graph/import/module-map provenance
 * is the completeness authority; unsupported active public facts reject the
 * complete transaction.  No declaration name has special meaning.
 *
 * v3.0's SEMANTIC_ATTRIBUTES family is absent.  Authenticated crate/module
 * attributes are therefore projected from this LOWER_SAFE descriptor and
 * included in `projected_semantic_attribute_count`.  For a supported public
 * top-level UNIT struct, the stricter item allowlist permits one each of
 * direct `stable(...)` or `unstable(...)`, `deprecated(...)`, unexpanded
 * `derive(...)`, and bare `non_exhaustive`.  The latter is the sole authority
 * for an absent public VALUE constructor mate. A supported free alias permits
 * only the stability/deprecation subset and must target a captured UNIT struct.
 * Three source-authenticated named aggregate profiles are also supported,
 * without declaration-name checks: a Rust-repr, zero-generic struct with one
 * retained lang identity and exactly four public BOOL fields; a transparent,
 * pub-transparent one-generic struct whose relaxed-Sized type parameter is its
 * sole private field; and a transparent, pub-transparent one-generic union
 * whose ordinary-Sized parameter appears in one UNIT field and one application
 * of the captured `manually_drop` lang wrapper. Named aggregates are TYPE-only
 * and retain their lang, representation, pub-transparent flag, field order,
 * visibility, types, and ITEM-owned generic shape. Their authenticated
 * stability and derive attributes are projected; layout/lang attributes are
 * normalized into retained ITEM facts. Predicated aggregates remain outside
 * this bounded profile and fail closed.
 * A supported enum is public, top-level, zero-generic/predicate and uses one
 * of two exact profiles. The first is `repr(u8)` with source-ordered UNIT
 * variants and explicit decimal ISIZE scalar discriminants in the u8 range;
 * it has exactly direct `derive(...)`, `unstable(...)`, and normalized
 * `repr(u8)` item attributes and one direct `unstable(...)` per variant. The
 * second has Rust's default repr, only implicit-discriminant UNIT variants,
 * no variant attributes, and exactly one direct `rustc_diagnostic_item =
 * "IDENT"` item attribute. That diagnostic identity is retained structurally
 * rather than counted as projected. Variant attributes and discriminants are
 * authenticated against the graph's source AST because this HIR model does
 * not retain variant attributes. Imported unit variants must have exact paired
 * TYPE and VALUE namespace identities targeting the same flattened ITEM-owned
 * variant local; no function or standalone value is synthesized.
 * A public reexport permits only direct `stable(...)` or `unstable(...)`,
 * `deprecated(...)`, `allow(...)`, and exact
 * `doc(alias("IDENT"))`, where IDENT is a nonempty ASCII Rust identifier.
 * Every other item/reexport spelling, duplicate, generated attribute,
 * repr/lang/layout/ABI attribute, or inconsistent provenance rejects.
 * Attributes on other supported items also reject. Every successful omission
 * is reported through
 * `semantic_attributes` and `projected_semantic_attribute_count`; it is not
 * an unchanged-HIR round-trip claim and is not usable as attribute-complete
 * dependency metadata.
 *
 * A supported free const is public, top-level, immutable, explicitly typed by
 * a v3.0-representable primitive, zero-generic/predicate, and has one
 * authenticated source-owned body. The source primitive spelling and HIR
 * primitive kind must agree exactly.
 * Its direct attributes use only `stable(...)`, `unstable(...)`, and
 * `deprecated(...)`; renamed public reexports use the reexport allowlist above
 * and retain the same VALU identity. Only the declaration and body-presence
 * bit are captured. The initializer, an evaluated value, and CTFE IR are not
 * transported by v3.0, whose BODIES_CONST_IR family remains absent.
 *
 * On success output owns all descriptor storage.  Failure leaves an already
 * initialized output unchanged.
 */
CmHirDeclarationCaptureResult cm_hir_declaration_metadata_capture(
    const CmHirDeclarationCaptureInput *input,
    CmHirDeclarationMetadata *output);

const char *cm_hir_declaration_capture_status_name(
    CmHirDeclarationCaptureStatus status);
const char *cm_hir_declaration_capture_stage_name(
    CmHirDeclarationCaptureStage stage);
const char *cm_hir_declaration_capture_reason_name(
    CmHirDeclarationCaptureReason reason);

#endif
