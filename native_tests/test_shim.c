#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../app/src/main/cpp/shim/desktop_shim.h"

int main(void) {
    setenv("ROOTFS", "/data/rootfs", 1);

    char out[1024];

    // Case 1: Standard absolute Linux path
    desktop_shim_rewrite_path("/usr/bin/bash", out, sizeof(out));
    printf("Rewritten /usr/bin/bash -> %s\n", out);
    assert(strcmp(out, "/data/rootfs/usr/bin/bash") == 0);

    // Case 2: Kernel pseudo-filesystem pass-through
    desktop_shim_rewrite_path("/dev/null", out, sizeof(out));
    printf("Rewritten /dev/null -> %s\n", out);
    assert(strcmp(out, "/dev/null") == 0);

    desktop_shim_rewrite_path("/proc/cpuinfo", out, sizeof(out));
    printf("Rewritten /proc/cpuinfo -> %s\n", out);
    assert(strcmp(out, "/proc/cpuinfo") == 0);

    // Case 3: Already in rootfs
    desktop_shim_rewrite_path("/data/rootfs/lib/libc.so", out, sizeof(out));
    printf("Rewritten existing rootfs -> %s\n", out);
    assert(strcmp(out, "/data/rootfs/lib/libc.so") == 0);

    // Case 4: Relative path
    desktop_shim_rewrite_path("local/file.txt", out, sizeof(out));
    printf("Rewritten relative -> %s\n", out);
    assert(strcmp(out, "local/file.txt") == 0);

    printf("ALL SHIM TESTS PASSED!\n");
    return 0;
}

