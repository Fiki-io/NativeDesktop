#ifndef DESKTOP_SHIM_H
#define DESKTOP_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void desktop_shim_rewrite_path(const char *in_path, char *out_path, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // DESKTOP_SHIM_H

