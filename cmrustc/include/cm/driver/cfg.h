#ifndef CMRUSTC_CM_DRIVER_CFG_H
#define CMRUSTC_CM_DRIVER_CFG_H

#include "cm/driver.h"
#include "cm/macro/expand.h"
#include "cm/syntax/token.h"

/*
 * Populate the cfg facts which are intrinsic to one supported target.
 * Crate features, command-line --cfg values, panic mode,
 * sanitizers, and code-generation options are deliberately not invented
 * here; their eventual owners must extend the set explicitly.
 */
int cm_target_cfg_set(CmCfgSet *cfg, const CmTargetDesc *target);

#endif
