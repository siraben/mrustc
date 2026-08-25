#ifndef CMRUSTC_CM_HIR_ARTIFACT_CONFIG_H
#define CMRUSTC_CM_HIR_ARTIFACT_CONFIG_H

#include "cm/buf.h"
#include "cm/driver.h"
#include "cm/hir/artifact_identity.h"
#include "cm/macro/expand.h"
#include "cm/syntax/token.h"

#include <stdint.h>

typedef enum CmHirArtifactPanicStrategy {
    CM_HIR_ARTIFACT_PANIC_ABORT = 0,
    CM_HIR_ARTIFACT_PANIC_UNWIND
} CmHirArtifactPanicStrategy;

typedef enum CmHirArtifactConfigStatus {
    CM_HIR_ARTIFACT_CONFIG_OK = 0,
    CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT,
    CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED,
    CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_EDITION,
    CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_PANIC_STRATEGY,
    CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_ENDIAN,
    CM_HIR_ARTIFACT_CONFIG_DUPLICATE,
    CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG
} CmHirArtifactConfigStatus;

/*
 * Owning canonical configuration material for CmHirArtifactIdentityInput.
 * The byte views borrow this object and remain valid until it is destroyed or
 * replaced by another successful build.  Initialize before first use.
 */
typedef struct CmHirArtifactConfig {
    uint32_t edition;
    CmHirArtifactBytes target_descriptor;
    CmHirArtifactBytes panic_strategy;
    CmHirArtifactBytes *cfgs;
    size_t cfg_count;
    CmByteBuf descriptor_storage;
    CmByteBuf cfg_storage;
} CmHirArtifactConfig;

void cm_hir_artifact_config_init(CmHirArtifactConfig *config);
void cm_hir_artifact_config_destroy(CmHirArtifactConfig *config);

/*
 * Build a locale-independent target descriptor and strictly bytewise-sorted
 * cfg strings.  Target feature and cfg-entry input ordering is canonicalized;
 * duplicates and effective cfg facts which contradict the target are
 * rejected.  `out_config` is changed only on success.
 */
CmHirArtifactConfigStatus cm_hir_artifact_config_build(
    const CmTargetDesc *target, enum cm_edition edition,
    CmHirArtifactPanicStrategy panic_strategy,
    const CmCfgSet *effective_cfg, CmHirArtifactConfig *out_config);

const char *cm_hir_artifact_config_status_name(
    CmHirArtifactConfigStatus status);

#endif
