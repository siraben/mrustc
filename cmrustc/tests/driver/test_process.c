#define _POSIX_C_SOURCE 200809L

#include "cm/driver.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char directory[] = "/tmp/cmrustc-process-test.XXXXXX";
    char marker_path[128];
    char *present[] = { (char *)"test", (char *)"-f",
        (char *)"marker", NULL };
    char *missing[] = { (char *)"test", (char *)"-f",
        (char *)"missing", NULL };
    CmProcessStatus status;
    FILE *marker;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(marker_path, sizeof(marker_path), "%s/marker", directory)
        > 0);
    marker = fopen(marker_path, "wb");
    assert(marker != NULL);
    assert(fputs("ok\n", marker) >= 0);
    assert(fclose(marker) == 0);

    assert(cm_process_run_in_directory(directory, present, &status));
    assert(status.launched && status.exited && status.exit_code == 0);
    assert(cm_process_run_in_directory(directory, missing, &status));
    assert(status.launched && status.exited && status.exit_code == 1);
    assert(cm_process_run_in_directory("/definitely/not/a/cmrustc/path",
        present, &status));
    assert(status.launched && status.exited && status.exit_code == 126);
    assert(!cm_process_run_in_directory(NULL, present, &status));

    assert(unlink(marker_path) == 0);
    assert(rmdir(directory) == 0);
    return 0;
}
