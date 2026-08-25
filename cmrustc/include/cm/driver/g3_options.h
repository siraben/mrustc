#ifndef CMRUSTC_CM_DRIVER_G3_OPTIONS_H
#define CMRUSTC_CM_DRIVER_G3_OPTIONS_H

#include "cm/config.h"

/*
 * G3 option parsing is deliberately allocation-free.  All strings in a
 * successful result are borrowed from argv and remain valid only as long as
 * the corresponding argv strings remain valid.
 */
#define CM_G3_OPTIONS_MAX_EXTERNS 64u

typedef enum CmG3Action {
    CM_G3_ACTION_NONE = 0,
    CM_G3_ACTION_EMIT_C,
    CM_G3_ACTION_EMIT_CMRLIB
} CmG3Action;

typedef struct CmG3StringView {
    const char *data;
    size_t length;
} CmG3StringView;

typedef struct CmG3ExternCmrlib {
    CmG3StringView name;
    const char *path;
} CmG3ExternCmrlib;

typedef struct CmG3Options {
    CmG3Action action;
    const char *input_path;
    const char *output_path;
    const char *crate_name;
    const char *c_compiler;
    const char *edition;
    const char *target;
    CmG3ExternCmrlib externs[CM_G3_OPTIONS_MAX_EXTERNS];
    size_t extern_count;
} CmG3Options;

typedef enum CmG3OptionsStatus {
    CM_G3_OPTIONS_OK = 0,
    CM_G3_OPTIONS_INVALID_ARGUMENT,
    CM_G3_OPTIONS_UNKNOWN_OPTION,
    CM_G3_OPTIONS_MISSING_VALUE,
    CM_G3_OPTIONS_DUPLICATE_OPTION,
    CM_G3_OPTIONS_CONFLICTING_ACTION,
    CM_G3_OPTIONS_MISSING_ACTION,
    CM_G3_OPTIONS_MISSING_REQUIRED_OPTION,
    CM_G3_OPTIONS_INVALID_IDENTIFIER,
    CM_G3_OPTIONS_INVALID_EDITION,
    CM_G3_OPTIONS_INVALID_EXTERN,
    CM_G3_OPTIONS_DUPLICATE_EXTERN,
    CM_G3_OPTIONS_TOO_MANY_EXTERNS,
    CM_G3_OPTIONS_OPTION_NOT_ALLOWED
} CmG3OptionsStatus;

typedef struct CmG3OptionsResult {
    CmG3OptionsStatus status;
    /* Offending argv index, or argc when a required option is absent. */
    int argument_index;
    /* Associated option spelling (a literal or borrowed from argv), or NULL. */
    const char *option;
} CmG3OptionsResult;

/*
 * Parse a complete process argument vector (argv[0] is skipped).  On error,
 * *options is not modified.  Every option uses a separate following value.
 */
CmG3OptionsResult cm_g3_options_parse(int argc, const char *const argv[],
    CmG3Options *options);

const char *cm_g3_options_status_name(CmG3OptionsStatus status);

#endif
