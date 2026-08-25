#include "cm/driver.h"

#include "cm/macro.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The bootstrap Mes unistd.h implements _exit but omits its declaration. */
extern void _exit(int status);

#ifndef WIFEXITED
# define WIFEXITED(status) (((status) & 0x7f) == 0)
#endif
#ifndef WEXITSTATUS
# define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#endif
#ifndef WIFSIGNALED
# define WIFSIGNALED(status) \
    (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#endif
#ifndef WTERMSIG
# define WTERMSIG(status) ((status) & 0x7f)
#endif

static const char *const cm_i686_target_features[] = {
    "fxsr", "sse", "sse2"
};

static const char *const cm_x86_64_target_features[] = {
    "fxsr", "sse", "sse2"
};

static const CmCfgEntry cm_i386_cfg_entries[] = {
    { "target_thread_local", NULL },
    { "target_has_atomic", "8" },
    { "target_has_atomic", "16" },
    { "target_has_atomic", "32" },
    { "target_has_atomic", "ptr" },
    { "target_has_atomic_equal_alignment", "8" },
    { "target_has_atomic_equal_alignment", "16" },
    { "target_has_atomic_equal_alignment", "32" },
    { "target_has_atomic_equal_alignment", "ptr" },
    { "target_has_atomic_load_store", "8" },
    { "target_has_atomic_load_store", "16" },
    { "target_has_atomic_load_store", "32" },
    { "target_has_atomic_load_store", "ptr" }
};

static const CmCfgEntry cm_i686_cfg_entries[] = {
    { "target_thread_local", NULL },
    { "target_has_atomic", "8" },
    { "target_has_atomic", "16" },
    { "target_has_atomic", "32" },
    { "target_has_atomic", "64" },
    { "target_has_atomic", "ptr" },
    { "target_has_atomic_equal_alignment", "8" },
    { "target_has_atomic_equal_alignment", "16" },
    { "target_has_atomic_equal_alignment", "32" },
    { "target_has_atomic_equal_alignment", "ptr" },
    { "target_has_atomic_load_store", "8" },
    { "target_has_atomic_load_store", "16" },
    { "target_has_atomic_load_store", "32" },
    { "target_has_atomic_load_store", "64" },
    { "target_has_atomic_load_store", "ptr" }
};

static const CmCfgEntry cm_x86_64_cfg_entries[] = {
    { "target_thread_local", NULL },
    { "target_has_atomic", "8" },
    { "target_has_atomic", "16" },
    { "target_has_atomic", "32" },
    { "target_has_atomic", "64" },
    { "target_has_atomic", "ptr" },
    { "target_has_atomic_equal_alignment", "8" },
    { "target_has_atomic_equal_alignment", "16" },
    { "target_has_atomic_equal_alignment", "32" },
    { "target_has_atomic_equal_alignment", "64" },
    { "target_has_atomic_equal_alignment", "ptr" },
    { "target_has_atomic_load_store", "8" },
    { "target_has_atomic_load_store", "16" },
    { "target_has_atomic_load_store", "32" },
    { "target_has_atomic_load_store", "64" },
    { "target_has_atomic_load_store", "ptr" }
};

static const CmTargetDesc cm_targets[] = {
    {
        "i386-unknown-linux-musl", "x86", "linux", "musl", "", "unknown",
        "unix", 32u, CM_ENDIAN_LITTLE, NULL, 0u, cm_i386_cfg_entries,
        CM_ARRAY_LEN(cm_i386_cfg_entries)
    },
    {
        "i686-unknown-linux-musl", "x86", "linux", "musl", "", "unknown",
        "unix", 32u, CM_ENDIAN_LITTLE, cm_i686_target_features,
        CM_ARRAY_LEN(cm_i686_target_features), cm_i686_cfg_entries,
        CM_ARRAY_LEN(cm_i686_cfg_entries)
    },
    {
        "x86_64-unknown-linux-gnu", "x86_64", "linux", "gnu", "", "unknown",
        "unix", 64u, CM_ENDIAN_LITTLE, cm_x86_64_target_features,
        CM_ARRAY_LEN(cm_x86_64_target_features), cm_x86_64_cfg_entries,
        CM_ARRAY_LEN(cm_x86_64_cfg_entries)
    },
    {
        "x86_64-unknown-linux-musl", "x86_64", "linux", "musl", "", "unknown",
        "unix", 64u, CM_ENDIAN_LITTLE, cm_x86_64_target_features,
        CM_ARRAY_LEN(cm_x86_64_target_features), cm_x86_64_cfg_entries,
        CM_ARRAY_LEN(cm_x86_64_cfg_entries)
    }
};

const char *cm_build_identity(void)
{
    return CM_COMPILER_IDENTITY;
}

const CmTargetDesc *cm_target_find(const char *triple)
{
    size_t index;

    if (triple == NULL) {
        return NULL;
    }
    for (index = 0u; index < CM_ARRAY_LEN(cm_targets); ++index) {
        if (strcmp(triple, cm_targets[index].triple) == 0) {
            return &cm_targets[index];
        }
    }
    return NULL;
}

const CmTargetDesc *cm_target_default(void)
{
#if defined(__i386__)
    return cm_target_find("i386-unknown-linux-musl");
#elif defined(__x86_64__)
# if defined(__GLIBC__)
    return cm_target_find("x86_64-unknown-linux-gnu");
# else
    return cm_target_find("x86_64-unknown-linux-musl");
# endif
#else
    return NULL;
#endif
}

static int cm_process_run_internal(const char *directory,
    char *const arguments[], CmProcessStatus *status)
{
    pid_t child;
    pid_t waited;
    int wait_status;

    if (arguments == NULL || arguments[0] == NULL || status == NULL
        || (directory != NULL && directory[0] == '\0')) {
        return 0;
    }
    memset(status, 0, sizeof(*status));
    child = fork();
    if (child < (pid_t)0) {
        return 0;
    }
    if (child == (pid_t)0) {
        if (directory != NULL && chdir(directory) != 0) _exit(126);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    status->launched = 1;
    do {
        waited = waitpid(child, &wait_status, 0);
    } while (waited < (pid_t)0 && errno == EINTR);
    if (waited != child) {
        return 0;
    }
    if (WIFEXITED(wait_status)) {
        status->exited = 1;
        status->exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        status->signal_number = WTERMSIG(wait_status);
    }
    return 1;
}

int cm_process_run(char *const arguments[], CmProcessStatus *status)
{
    return cm_process_run_internal(NULL, arguments, status);
}

int cm_process_run_in_directory(const char *directory,
    char *const arguments[], CmProcessStatus *status)
{
    if (directory == NULL) return 0;
    return cm_process_run_internal(directory, arguments, status);
}
