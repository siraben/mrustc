#ifndef CM_DRIVER_H
#define CM_DRIVER_H

#include "cm/config.h"

struct CmCfgEntry;

typedef enum CmEndian {
    CM_ENDIAN_LITTLE = 0,
    CM_ENDIAN_BIG
} CmEndian;

typedef struct CmTargetDesc {
    const char *triple;
    const char *architecture;
    const char *operating_system;
    const char *environment;
    const char *abi;
    const char *vendor;
    const char *family;
    unsigned int pointer_bits;
    CmEndian endian;
    const char *const *target_features;
    size_t target_feature_count;
    const struct CmCfgEntry *cfg_entries;
    size_t cfg_entry_count;
} CmTargetDesc;

typedef struct CmProcessStatus {
    int launched;
    int exited;
    int exit_code;
    int signal_number;
} CmProcessStatus;

const char *cm_build_identity(void);
const CmTargetDesc *cm_target_find(const char *triple);
const CmTargetDesc *cm_target_default(void);
int cm_process_run(char *const arguments[], CmProcessStatus *status);
int cm_process_run_in_directory(const char *directory,
    char *const arguments[], CmProcessStatus *status);
int cm_driver_main(int argc, char **argv);

#endif
