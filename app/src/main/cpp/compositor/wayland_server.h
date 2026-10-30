#ifndef WAYLAND_SERVER_H
#define WAYLAND_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wayland_frame_render_cb)(const uint8_t *pixels, int width, int height, int stride, uint32_t format, void *user_data);

typedef struct wayland_server wayland_server_t;

wayland_server_t *wayland_server_create(const char *socket_dir, const char *socket_name);
void wayland_server_set_render_callback(wayland_server_t *server, wayland_frame_render_cb cb, void *user_data);
void wayland_server_set_output_size(wayland_server_t *server, int width, int height);
void wayland_server_inject_pointer(wayland_server_t *server, int action, float norm_x, float norm_y);
void wayland_server_inject_key(wayland_server_t *server, int linux_keycode, bool pressed, bool shift_mod);
int wayland_server_start(wayland_server_t *server);
void wayland_server_stop(wayland_server_t *server);
void wayland_server_destroy(wayland_server_t *server);

// Non-blocking or timed dispatch step (single iteration)
int wayland_server_dispatch_step(wayland_server_t *server, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // WAYLAND_SERVER_H
