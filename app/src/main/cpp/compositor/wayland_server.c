#define _GNU_SOURCE
#include "wayland_server.h"
#include "wayland_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_POOLS 32
#define MAX_BUFFERS 128
#define MAX_SURFACES 32
#define MAX_FRAME_CBS 8
#define MAX_CLIENTS 8
#define MAX_CURSOR_DIM 128

typedef struct {
    uint32_t id;
    int fd;
    size_t size;
    uint8_t *data;
    int ref_count;
    bool active;
} shm_pool_t;

typedef struct {
    uint32_t id;
    int pool_idx;
    int32_t offset;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
    bool active;
} shm_buffer_t;

typedef struct {
    uint32_t id;
    uint32_t xdg_surface_id;
    uint32_t xdg_toplevel_id;
    uint32_t shell_surface_id;
    uint32_t attached_buffer_id;
    uint32_t frame_callbacks[MAX_FRAME_CBS];
    int frame_cb_count;
    bool configured;
    bool active;
} surface_t;

typedef struct {
    int fd;
    bool connected;
    uint32_t registry_id;
    uint32_t compositor_id;
    uint32_t subcompositor_id;
    uint32_t shm_id;
    uint32_t output_id;
    uint32_t output_version;
    uint32_t seat_id;
    uint32_t seat_version;
    uint32_t pointer_id;
    uint32_t keyboard_id;
    uint32_t touch_id;
    uint32_t xdg_wm_base_id;
    uint32_t wl_shell_id;
    uint32_t next_serial;

    uint32_t pointer_entered_surface_id;
    uint32_t keyboard_entered_surface_id;
    uint32_t cursor_surface_id;
    int32_t cursor_hot_x;
    int32_t cursor_hot_y;
    uint32_t cursor_pixels[MAX_CURSOR_DIM * MAX_CURSOR_DIM];
    int32_t cursor_w;
    int32_t cursor_h;
    bool has_cursor_buffer;

    shm_pool_t pools[MAX_POOLS];
    shm_buffer_t buffers[MAX_BUFFERS];
    surface_t surfaces[MAX_SURFACES];
} client_session_t;

struct wayland_server {
    int listen_fd;
    char socket_dir[PATH_MAX];
    char socket_path[PATH_MAX];
    bool running;

    int output_width;
    int output_height;
    int last_rendered_width;
    int last_rendered_height;
    int32_t pointer_x;
    int32_t pointer_y;

    uint8_t *desktop_pixels;
    size_t desktop_cap;
    int desktop_w;
    int desktop_h;
    int desktop_stride;
    uint32_t desktop_format;

    uint8_t *composite_pixels;
    size_t composite_cap;

    client_session_t clients[MAX_CLIENTS];

    wayland_frame_render_cb render_cb;
    void *render_user_data;

    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_started;
};

static uint32_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

static void present_composited_frame(wayland_server_t *server, client_session_t *client) {
    if (!server || !server->render_cb || !server->desktop_pixels ||
        server->desktop_w <= 0 || server->desktop_h <= 0 || server->desktop_stride <= 0) {
        return;
    }

    if (!client || !client->has_cursor_buffer || client->cursor_w <= 0 || client->cursor_h <= 0) {
        server->render_cb(server->desktop_pixels, server->desktop_w, server->desktop_h,
                          server->desktop_stride, server->desktop_format, server->render_user_data);
        return;
    }

    size_t needed = (size_t)server->desktop_stride * (size_t)server->desktop_h;
    if (needed > server->composite_cap) {
        uint8_t *new_buf = (uint8_t *)realloc(server->composite_pixels, needed);
        if (!new_buf) {
            server->render_cb(server->desktop_pixels, server->desktop_w, server->desktop_h,
                              server->desktop_stride, server->desktop_format, server->render_user_data);
            return;
        }
        server->composite_pixels = new_buf;
        server->composite_cap = needed;
    }

    memcpy(server->composite_pixels, server->desktop_pixels, needed);

    int32_t start_x = server->pointer_x - client->cursor_hot_x;
    int32_t start_y = server->pointer_y - client->cursor_hot_y;

    for (int32_t cy = 0; cy < client->cursor_h; ++cy) {
        int32_t dy = start_y + cy;
        if (dy < 0 || dy >= server->desktop_h) continue;

        uint32_t *dst_row = (uint32_t *)(server->composite_pixels + (size_t)dy * (size_t)server->desktop_stride);
        const uint32_t *src_row = &client->cursor_pixels[cy * client->cursor_w];

        for (int32_t cx = 0; cx < client->cursor_w; ++cx) {
            int32_t dx = start_x + cx;
            if (dx < 0 || dx >= server->desktop_w) continue;

            uint32_t sp = src_row[cx];
            uint32_t sa = (sp >> 24) & 0xFFU;
            if (sa == 0) continue;

            if (sa == 255) {
                dst_row[dx] = sp | 0xFF000000U;
            } else {
                uint32_t dp = dst_row[dx];
                uint32_t inv_a = 255U - sa;
                uint32_t sr = (sp >> 16) & 0xFFU;
                uint32_t sg = (sp >> 8) & 0xFFU;
                uint32_t sb = sp & 0xFFU;
                uint32_t dr = (dp >> 16) & 0xFFU;
                uint32_t dg = (dp >> 8) & 0xFFU;
                uint32_t db = dp & 0xFFU;

                // Note: Wayland ARGB8888 cursor buffers use premultiplied alpha
                uint32_t out_r = sr + ((dr * inv_a) / 255U);
                uint32_t out_g = sg + ((dg * inv_a) / 255U);
                uint32_t out_b = sb + ((db * inv_a) / 255U);
                if (out_r > 255U) out_r = 255U;
                if (out_g > 255U) out_g = 255U;
                if (out_b > 255U) out_b = 255U;

                dst_row[dx] = 0xFF000000U | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }

    server->render_cb(server->composite_pixels, server->desktop_w, server->desktop_h,
                      server->desktop_stride, server->desktop_format, server->render_user_data);
}

static void pool_unref(shm_pool_t *pool) {
    if (!pool || !pool->active) return;
    pool->ref_count--;
    if (pool->ref_count <= 0) {
        if (pool->data != MAP_FAILED && pool->data != NULL) {
            munmap(pool->data, pool->size);
            pool->data = NULL;
        }
        if (pool->fd >= 0) {
            close(pool->fd);
            pool->fd = -1;
        }
        pool->id = 0;
        pool->size = 0;
        pool->ref_count = 0;
        pool->active = false;
    }
}

static int send_wire_message(int fd, uint32_t object_id, uint16_t opcode, const void *payload, size_t payload_len) {
    size_t total_len = sizeof(wl_wire_header_t) + payload_len;
    uint8_t stack_buf[512];
    uint8_t *buf = stack_buf;
    if (total_len > sizeof(stack_buf)) {
        buf = (uint8_t *)malloc(total_len);
        if (!buf) return -1;
    }

    wl_wire_header_t *hdr = (wl_wire_header_t *)buf;
    hdr->object_id = object_id;
    hdr->opcode_and_size = wl_header_pack(opcode, (uint16_t)total_len);

    if (payload && payload_len > 0) {
        memcpy(buf + sizeof(wl_wire_header_t), payload, payload_len);
    }

    ssize_t written = send(fd, buf, total_len, MSG_NOSIGNAL);
    if (buf != stack_buf) {
        free(buf);
    }
    return (written == (ssize_t)total_len) ? 0 : -1;
}

static int send_wire_message_with_fd(int sock_fd, uint32_t object_id, uint16_t opcode,
                                     const void *payload, size_t payload_len, int pass_fd) {
    size_t total_len = sizeof(wl_wire_header_t) + payload_len;
    uint8_t buf[256];
    if (total_len > sizeof(buf)) return -1;

    wl_wire_header_t *hdr = (wl_wire_header_t *)buf;
    hdr->object_id = object_id;
    hdr->opcode_and_size = wl_header_pack(opcode, (uint16_t)total_len);
    if (payload && payload_len > 0) {
        memcpy(buf + sizeof(wl_wire_header_t), payload, payload_len);
    }

    struct iovec iov = {
        .iov_base = buf,
        .iov_len = total_len
    };

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));

    ssize_t sent = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
    return (sent == (ssize_t)total_len) ? 0 : -1;
}

static void send_keyboard_keymap(wayland_server_t *server, client_session_t *client) {
    if (!server || !client || client->keyboard_id == 0) return;

    static const char *xkb_str =
        "xkb_keymap {\n"
        "  xkb_keycodes  { include \"evdev+aliases(qwerty)\" };\n"
        "  xkb_types     { include \"complete\" };\n"
        "  xkb_compat    { include \"complete\" };\n"
        "  xkb_symbols   { include \"pc+us+inet(evdev)\" };\n"
        "};\n";

    size_t map_size = strlen(xkb_str) + 1;
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s/wl-keymap-%d", server->socket_dir, client->fd);

    int kfd = open(tmp_path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
    if (kfd < 0) return;
    unlink(tmp_path);

    ssize_t w = write(kfd, xkb_str, map_size);
    (void)w;
    lseek(kfd, 0, SEEK_SET);

    uint32_t payload[2];
    payload[0] = WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1;
    payload[1] = (uint32_t)map_size;
    send_wire_message_with_fd(client->fd, client->keyboard_id, WL_KEYBOARD_KEYMAP,
                              payload, sizeof(payload), kfd);
    close(kfd);
}

static void ensure_keyboard_entered(client_session_t *client, uint32_t target_surf_id) {
    if (!client || client->keyboard_id == 0 || target_surf_id == 0) return;
    if (client->keyboard_entered_surface_id == target_surf_id) return;

    uint32_t serial = ++client->next_serial;
    uint32_t enter_payload[3];
    enter_payload[0] = serial;
    enter_payload[1] = target_surf_id;
    enter_payload[2] = 0; // empty pressed-keys array
    send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_ENTER, enter_payload, sizeof(enter_payload));

    uint32_t mod_serial = ++client->next_serial;
    uint32_t mod_payload[5] = { mod_serial, 0, 0, 0, 0 };
    send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_MODIFIERS, mod_payload, sizeof(mod_payload));

    client->keyboard_entered_surface_id = target_surf_id;
}

static void send_global(int client_fd, uint32_t registry_id, uint32_t name, const char *interface, uint32_t version) {
    uint32_t iface_len = (uint32_t)strlen(interface) + 1;
    uint32_t iface_padded = (iface_len + 3) & ~3U;

    size_t payload_len = sizeof(uint32_t) + sizeof(uint32_t) + iface_padded + sizeof(uint32_t);
    uint8_t payload[128];
    if (payload_len > sizeof(payload)) return;
    memset(payload, 0, payload_len);

    uint32_t *p32 = (uint32_t *)payload;
    p32[0] = name;
    p32[1] = iface_len;
    memcpy(payload + 8, interface, iface_len);

    uint32_t *ver_ptr = (uint32_t *)(payload + 8 + iface_padded);
    ver_ptr[0] = version;

    send_wire_message(client_fd, registry_id, WL_REGISTRY_GLOBAL, payload, payload_len);
}

static void send_output_info(wayland_server_t *server, client_session_t *client) {
    if (client->output_id == 0) return;

    const char *make = "Android";
    const char *model = "NativeDesktop";
    uint32_t make_len = (uint32_t)strlen(make) + 1;
    uint32_t make_pad = (make_len + 3) & ~3U;
    uint32_t model_len = (uint32_t)strlen(model) + 1;
    uint32_t model_pad = (model_len + 3) & ~3U;

    // WL_OUTPUT_GEOMETRY: [x, y, phys_w, phys_h, subpixel, make(str), model(str), transform]
    size_t geom_len = 5 * sizeof(int32_t) + sizeof(uint32_t) + make_pad + sizeof(uint32_t) + model_pad + sizeof(int32_t);
    uint8_t geom_buf[128];
    memset(geom_buf, 0, sizeof(geom_buf));
    int32_t *g32 = (int32_t *)geom_buf;
    g32[0] = 0;   // x
    g32[1] = 0;   // y
    g32[2] = 150; // physical_width (mm)
    g32[3] = 260; // physical_height (mm)
    g32[4] = 0;   // subpixel: WL_OUTPUT_SUBPIXEL_UNKNOWN
    *(uint32_t *)(geom_buf + 20) = make_len;
    memcpy(geom_buf + 24, make, make_len);
    size_t off = 24 + make_pad;
    *(uint32_t *)(geom_buf + off) = model_len;
    memcpy(geom_buf + off + 4, model, model_len);
    off += 4 + model_pad;
    *(int32_t *)(geom_buf + off) = 0; // transform: WL_OUTPUT_TRANSFORM_NORMAL
    send_wire_message(client->fd, client->output_id, WL_OUTPUT_GEOMETRY, geom_buf, geom_len);

    // WL_OUTPUT_MODE: [uint32 flags, int32 width, int32 height, int32 refresh]
    int32_t mode_payload[4];
    mode_payload[0] = (int32_t)(WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED);
    mode_payload[1] = server->output_width > 0 ? server->output_width : 1280;
    mode_payload[2] = server->output_height > 0 ? server->output_height : 720;
    mode_payload[3] = 60000; // 60 Hz in mHz
    send_wire_message(client->fd, client->output_id, WL_OUTPUT_MODE, mode_payload, sizeof(mode_payload));

    if (client->output_version >= 2) {
        int32_t scale = 1;
        send_wire_message(client->fd, client->output_id, WL_OUTPUT_SCALE, &scale, sizeof(scale));
        send_wire_message(client->fd, client->output_id, WL_OUTPUT_DONE, NULL, 0);
    }
}

static void send_seat_info(client_session_t *client) {
    if (client->seat_id == 0) return;

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    send_wire_message(client->fd, client->seat_id, WL_SEAT_CAPABILITIES, &caps, sizeof(caps));

    if (client->seat_version >= 2) {
        const char *seat_name = "default";
        uint32_t len = (uint32_t)strlen(seat_name) + 1;
        uint32_t padded = (len + 3) & ~3U;
        uint8_t buf[32];
        memset(buf, 0, sizeof(buf));
        *(uint32_t *)buf = len;
        memcpy(buf + 4, seat_name, len);
        send_wire_message(client->fd, client->seat_id, WL_SEAT_NAME, buf, 4 + padded);
    }
}

static void send_xdg_configure(wayland_server_t *server, client_session_t *client, surface_t *surf) {
    int32_t w = server->output_width > 0 ? server->output_width : 1280;
    int32_t h = server->output_height > 0 ? server->output_height : 720;

    if (surf->xdg_toplevel_id != 0) {
        // xdg_toplevel.configure: [int32 width, int32 height, array states ([FULLSCREEN=2, ACTIVATED=4])]
        uint32_t top_cfg[5];
        top_cfg[0] = (uint32_t)w;
        top_cfg[1] = (uint32_t)h;
        top_cfg[2] = 2 * sizeof(uint32_t); // 2 states in array (8 bytes)
        top_cfg[3] = XDG_TOPLEVEL_STATE_FULLSCREEN;
        top_cfg[4] = XDG_TOPLEVEL_STATE_ACTIVATED;
        send_wire_message(client->fd, surf->xdg_toplevel_id, XDG_TOPLEVEL_CONFIGURE, top_cfg, sizeof(top_cfg));
    }

    if (surf->xdg_surface_id != 0) {
        uint32_t serial = ++client->next_serial;
        send_wire_message(client->fd, surf->xdg_surface_id, XDG_SURFACE_CONFIGURE, &serial, sizeof(serial));
        surf->configured = true;
        ensure_keyboard_entered(client, surf->id);
    }
}

static void client_close(client_session_t *client) {
    if (!client->connected) return;

    for (int i = 0; i < MAX_BUFFERS; ++i) {
        client->buffers[i].active = false;
    }
    for (int i = 0; i < MAX_POOLS; ++i) {
        if (client->pools[i].active) {
            if (client->pools[i].data != MAP_FAILED && client->pools[i].data != NULL) {
                munmap(client->pools[i].data, client->pools[i].size);
            }
            if (client->pools[i].fd >= 0) {
                close(client->pools[i].fd);
            }
            client->pools[i].active = false;
            client->pools[i].ref_count = 0;
        }
    }
    for (int i = 0; i < MAX_SURFACES; ++i) {
        client->surfaces[i].active = false;
    }

    close(client->fd);
    client->fd = -1;
    client->connected = false;
}

static void handle_client_message(wayland_server_t *server, client_session_t *client,
                                  uint32_t object_id, uint16_t opcode,
                                  const uint8_t *payload, size_t payload_len,
                                  int *received_fds, int *fd_idx, int fd_count) {
    if (object_id == WL_DISPLAY_ID) {
        if (opcode == WL_DISPLAY_SYNC) {
            if (payload_len >= 4) {
                uint32_t callback_id = *(const uint32_t *)payload;
                uint32_t serial = ++client->next_serial;
                send_wire_message(client->fd, callback_id, WL_CALLBACK_DONE, &serial, sizeof(serial));
                send_wire_message(client->fd, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, &callback_id, sizeof(callback_id));
            }
        } else if (opcode == WL_DISPLAY_GET_REGISTRY) {
            if (payload_len >= 4) {
                client->registry_id = *(const uint32_t *)payload;
                send_global(client->fd, client->registry_id, 1, "wl_compositor", 4);
                send_global(client->fd, client->registry_id, 2, "wl_shm", 1);
                send_global(client->fd, client->registry_id, 3, "wl_output", 2);
                send_global(client->fd, client->registry_id, 4, "xdg_wm_base", 2);
                send_global(client->fd, client->registry_id, 5, "wl_seat", 4);
                send_global(client->fd, client->registry_id, 6, "wl_subcompositor", 1);
                send_global(client->fd, client->registry_id, 7, "wl_shell", 1);
            }
        }
        return;
    }

    if (object_id == client->registry_id) {
        if (opcode == WL_REGISTRY_BIND && payload_len >= 12) {
            const uint32_t *p32 = (const uint32_t *)payload;
            uint32_t name = p32[0];
            uint32_t iface_len = p32[1];
            uint32_t iface_padded = (iface_len + 3) & ~3U;
            if (8 + iface_padded + 8 > payload_len) return;
            const char *iface = (const char *)(payload + 8);
            const uint32_t *trailing = (const uint32_t *)(payload + 8 + iface_padded);
            uint32_t version = trailing[0];
            uint32_t id = trailing[1];

            if (name == 1 || strcmp(iface, "wl_compositor") == 0) {
                client->compositor_id = id;
            } else if (name == 2 || strcmp(iface, "wl_shm") == 0) {
                client->shm_id = id;
                uint32_t fmt_argb = WL_SHM_FORMAT_ARGB8888;
                uint32_t fmt_xrgb = WL_SHM_FORMAT_XRGB8888;
                send_wire_message(client->fd, client->shm_id, WL_SHM_FORMAT, &fmt_argb, sizeof(fmt_argb));
                send_wire_message(client->fd, client->shm_id, WL_SHM_FORMAT, &fmt_xrgb, sizeof(fmt_xrgb));
            } else if (name == 3 || strcmp(iface, "wl_output") == 0) {
                client->output_id = id;
                client->output_version = version;
                send_output_info(server, client);
            } else if (name == 4 || strcmp(iface, "xdg_wm_base") == 0) {
                client->xdg_wm_base_id = id;
            } else if (name == 5 || strcmp(iface, "wl_seat") == 0) {
                client->seat_id = id;
                client->seat_version = version;
                send_seat_info(client);
            } else if (name == 6 || strcmp(iface, "wl_subcompositor") == 0) {
                client->subcompositor_id = id;
            } else if (name == 7 || strcmp(iface, "wl_shell") == 0) {
                client->wl_shell_id = id;
            }
        }
        return;
    }

    if (object_id == client->compositor_id) {
        if (opcode == WL_COMPOSITOR_CREATE_SURFACE && payload_len >= 4) {
            uint32_t surface_id = *(const uint32_t *)payload;
            for (int i = 0; i < MAX_SURFACES; ++i) {
                if (!client->surfaces[i].active) {
                    memset(&client->surfaces[i], 0, sizeof(surface_t));
                    client->surfaces[i].id = surface_id;
                    client->surfaces[i].active = true;
                    break;
                }
            }
        }
        return;
    }

    if (object_id == client->seat_id) {
        if (opcode == WL_SEAT_GET_POINTER && payload_len >= 4) {
            client->pointer_id = *(const uint32_t *)payload;
            client->pointer_entered_surface_id = 0;
        } else if (opcode == WL_SEAT_GET_KEYBOARD && payload_len >= 4) {
            client->keyboard_id = *(const uint32_t *)payload;
            client->keyboard_entered_surface_id = 0;
            send_keyboard_keymap(server, client);
            if (client->seat_version >= 4) {
                int32_t repeat[2] = { 40, 400 };
                send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_REPEAT_INFO, repeat, sizeof(repeat));
            }
            for (int s = 0; s < MAX_SURFACES; ++s) {
                if (client->surfaces[s].active && client->surfaces[s].xdg_surface_id != 0) {
                    ensure_keyboard_entered(client, client->surfaces[s].id);
                    break;
                }
            }
        } else if (opcode == WL_SEAT_GET_TOUCH && payload_len >= 4) {
            client->touch_id = *(const uint32_t *)payload;
        }
        return;
    }

    if (object_id == client->keyboard_id && client->keyboard_id != 0) {
        if (opcode == WL_KEYBOARD_RELEASE) {
            send_wire_message(client->fd, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, &object_id, sizeof(object_id));
            client->keyboard_id = 0;
            client->keyboard_entered_surface_id = 0;
        }
        return;
    }

    if (object_id == client->pointer_id && client->pointer_id != 0) {
        if (opcode == WL_POINTER_SET_CURSOR && payload_len >= 16) {
            const uint32_t *p32 = (const uint32_t *)payload;
            uint32_t surf_id = p32[1];
            int32_t hot_x = (int32_t)p32[2];
            int32_t hot_y = (int32_t)p32[3];
            client->cursor_surface_id = surf_id;
            client->cursor_hot_x = hot_x;
            client->cursor_hot_y = hot_y;
            if (surf_id == 0) {
                client->has_cursor_buffer = false;
                present_composited_frame(server, client);
            }
        } else if (opcode == WL_POINTER_RELEASE) {
            send_wire_message(client->fd, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, &object_id, sizeof(object_id));
            client->pointer_id = 0;
            client->pointer_entered_surface_id = 0;
        }
        return;
    }

    if (object_id == client->xdg_wm_base_id) {
        if (opcode == XDG_WM_BASE_GET_XDG_SURFACE && payload_len >= 8) {
            const uint32_t *p32 = (const uint32_t *)payload;
            uint32_t xdg_surf_id = p32[0];
            uint32_t wl_surf_id = p32[1];
            for (int i = 0; i < MAX_SURFACES; ++i) {
                if (client->surfaces[i].active && client->surfaces[i].id == wl_surf_id) {
                    client->surfaces[i].xdg_surface_id = xdg_surf_id;
                    break;
                }
            }
        }
        return;
    }

    if (object_id == client->wl_shell_id) {
        if (opcode == WL_SHELL_GET_SHELL_SURFACE && payload_len >= 8) {
            const uint32_t *p32 = (const uint32_t *)payload;
            uint32_t shell_surf_id = p32[0];
            uint32_t wl_surf_id = p32[1];
            for (int i = 0; i < MAX_SURFACES; ++i) {
                if (client->surfaces[i].active && client->surfaces[i].id == wl_surf_id) {
                    client->surfaces[i].shell_surface_id = shell_surf_id;
                    int32_t cfg[3] = {
                        0,
                        server->output_width > 0 ? server->output_width : 800,
                        server->output_height > 0 ? server->output_height : 1280
                    };
                    send_wire_message(client->fd, shell_surf_id, WL_SHELL_SURFACE_CONFIGURE, cfg, sizeof(cfg));
                    break;
                }
            }
        }
        return;
    }

    if (object_id == client->shm_id) {
        if (opcode == WL_SHM_CREATE_POOL && payload_len >= 8 && *fd_idx < fd_count) {
            int pool_fd = received_fds[(*fd_idx)++];
            const uint32_t *p32 = (const uint32_t *)payload;
            uint32_t pool_id = p32[0];
            int32_t pool_size = (int32_t)p32[1];

            for (int i = 0; i < MAX_POOLS; ++i) {
                if (!client->pools[i].active) {
                    client->pools[i].id = pool_id;
                    client->pools[i].fd = pool_fd;
                    client->pools[i].size = (size_t)pool_size;
                    client->pools[i].data = (uint8_t *)mmap(NULL, (size_t)pool_size, PROT_READ, MAP_SHARED, pool_fd, 0);
                    client->pools[i].ref_count = 1;
                    client->pools[i].active = true;
                    pool_fd = -1;
                    break;
                }
            }
            if (pool_fd >= 0) close(pool_fd);
        }
        return;
    }

    // Check if object is a shm_pool
    for (int i = 0; i < MAX_POOLS; ++i) {
        if (client->pools[i].active && client->pools[i].id == object_id && object_id != 0) {
            if (opcode == WL_SHM_POOL_CREATE_BUFFER && payload_len >= 24) {
                const uint32_t *p32 = (const uint32_t *)payload;
                uint32_t buf_id = p32[0];
                int32_t offset = (int32_t)p32[1];
                int32_t width = (int32_t)p32[2];
                int32_t height = (int32_t)p32[3];
                int32_t stride = (int32_t)p32[4];
                uint32_t format = p32[5];

                for (int b = 0; b < MAX_BUFFERS; ++b) {
                    if (!client->buffers[b].active || client->buffers[b].id == buf_id) {
                        if (client->buffers[b].active && client->buffers[b].pool_idx >= 0 && client->buffers[b].pool_idx < MAX_POOLS) {
                            pool_unref(&client->pools[client->buffers[b].pool_idx]);
                        }
                        client->pools[i].ref_count++;
                        client->buffers[b].id = buf_id;
                        client->buffers[b].pool_idx = i;
                        client->buffers[b].offset = offset;
                        client->buffers[b].width = width;
                        client->buffers[b].height = height;
                        client->buffers[b].stride = stride;
                        client->buffers[b].format = format;
                        client->buffers[b].active = true;
                        break;
                    }
                }
            } else if (opcode == WL_SHM_POOL_DESTROY) {
                // In Wayland, destroying wl_shm_pool releases the protocol ID immediately,
                // while the underlying mmap stays valid until all wl_buffers from this pool are destroyed.
                client->pools[i].id = 0;
                if (client->pools[i].fd >= 0) {
                    close(client->pools[i].fd);
                    client->pools[i].fd = -1;
                }
                pool_unref(&client->pools[i]);
                send_wire_message(client->fd, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, &object_id, sizeof(object_id));
            } else if (opcode == WL_SHM_POOL_RESIZE && payload_len >= 4) {
                int32_t new_size = *(const int32_t *)payload;
                if (new_size > 0 && client->pools[i].fd >= 0) {
                    if (client->pools[i].data != MAP_FAILED && client->pools[i].data != NULL) {
                        munmap(client->pools[i].data, client->pools[i].size);
                    }
                    client->pools[i].size = (size_t)new_size;
                    client->pools[i].data = (uint8_t *)mmap(NULL, (size_t)new_size, PROT_READ, MAP_SHARED, client->pools[i].fd, 0);
                }
            }
            return;
        }
    }

    // Check if object is a wl_buffer
    for (int b = 0; b < MAX_BUFFERS; ++b) {
        if (client->buffers[b].active && client->buffers[b].id == object_id) {
            if (opcode == WL_BUFFER_DESTROY) {
                int p_idx = client->buffers[b].pool_idx;
                client->buffers[b].active = false;
                if (p_idx >= 0 && p_idx < MAX_POOLS) {
                    pool_unref(&client->pools[p_idx]);
                }
                send_wire_message(client->fd, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, &object_id, sizeof(object_id));
            }
            return;
        }
    }

    // Check if object is an xdg_surface or xdg_toplevel
    for (int i = 0; i < MAX_SURFACES; ++i) {
        if (!client->surfaces[i].active) continue;

        if (client->surfaces[i].xdg_surface_id == object_id && object_id != 0) {
            if (opcode == XDG_SURFACE_DESTROY) {
                client->surfaces[i].xdg_surface_id = 0;
            } else if (opcode == XDG_SURFACE_GET_TOPLEVEL && payload_len >= 4) {
                client->surfaces[i].xdg_toplevel_id = *(const uint32_t *)payload;
                send_xdg_configure(server, client, &client->surfaces[i]);
            }
            return;
        }

        if (client->surfaces[i].xdg_toplevel_id == object_id && object_id != 0) {
            if (opcode == XDG_TOPLEVEL_DESTROY) {
                client->surfaces[i].xdg_toplevel_id = 0;
            } else if (opcode == XDG_TOPLEVEL_SET_FULLSCREEN) {
                send_xdg_configure(server, client, &client->surfaces[i]);
            }
            return;
        }
    }

    // Check if object is a wl_surface
    for (int i = 0; i < MAX_SURFACES; ++i) {
        if (client->surfaces[i].active && client->surfaces[i].id == object_id) {
            surface_t *surf = &client->surfaces[i];
            if (opcode == WL_SURFACE_DESTROY) {
                if (client->cursor_surface_id == surf->id) {
                    client->cursor_surface_id = 0;
                    client->has_cursor_buffer = false;
                }
                if (client->pointer_entered_surface_id == surf->id) {
                    client->pointer_entered_surface_id = 0;
                }
                surf->active = false;
            } else if (opcode == WL_SURFACE_ATTACH) {
                if (payload_len >= 4) {
                    surf->attached_buffer_id = *(const uint32_t *)payload;
                }
            } else if (opcode == WL_SURFACE_FRAME) {
                if (payload_len >= 4) {
                    uint32_t cb_id = *(const uint32_t *)payload;
                    if (surf->frame_cb_count < MAX_FRAME_CBS) {
                        surf->frame_callbacks[surf->frame_cb_count++] = cb_id;
                    }
                }
            } else if (opcode == WL_SURFACE_COMMIT) {
                bool is_cursor = (surf->id == client->cursor_surface_id && client->cursor_surface_id != 0);

                if (!is_cursor && surf->attached_buffer_id == 0 && surf->xdg_surface_id != 0) {
                    send_xdg_configure(server, client, surf);
                }

                uint32_t buf_id = surf->attached_buffer_id;
                if (is_cursor) {
                    if (buf_id == 0) {
                        client->has_cursor_buffer = false;
                    } else {
                        for (int b = 0; b < MAX_BUFFERS; ++b) {
                            if (client->buffers[b].active && client->buffers[b].id == buf_id) {
                                shm_buffer_t *buf = &client->buffers[b];
                                int p = buf->pool_idx;
                                if (p >= 0 && p < MAX_POOLS &&
                                    client->pools[p].active &&
                                    client->pools[p].data != MAP_FAILED && client->pools[p].data != NULL &&
                                    buf->width > 0 && buf->height > 0) {
                                    size_t end_off = (size_t)buf->offset + (size_t)buf->stride * (size_t)buf->height;
                                    if (end_off <= client->pools[p].size) {
                                        int32_t cw = buf->width < MAX_CURSOR_DIM ? buf->width : MAX_CURSOR_DIM;
                                        int32_t ch = buf->height < MAX_CURSOR_DIM ? buf->height : MAX_CURSOR_DIM;
                                        const uint8_t *src_base = client->pools[p].data + buf->offset;
                                        for (int32_t cy = 0; cy < ch; ++cy) {
                                            const uint32_t *src_row = (const uint32_t *)(src_base + (size_t)cy * (size_t)buf->stride);
                                            memcpy(&client->cursor_pixels[cy * cw], src_row, (size_t)cw * sizeof(uint32_t));
                                        }
                                        client->cursor_w = cw;
                                        client->cursor_h = ch;
                                        client->has_cursor_buffer = true;
                                    }
                                }
                                send_wire_message(client->fd, buf_id, WL_BUFFER_RELEASE, NULL, 0);
                                break;
                            }
                        }
                    }
                    present_composited_frame(server, client);
                } else if (buf_id != 0) {
                    for (int b = 0; b < MAX_BUFFERS; ++b) {
                        if (client->buffers[b].active && client->buffers[b].id == buf_id) {
                            shm_buffer_t *buf = &client->buffers[b];
                            int p = buf->pool_idx;
                            if (p >= 0 && p < MAX_POOLS &&
                                client->pools[p].active &&
                                client->pools[p].data != MAP_FAILED && client->pools[p].data != NULL &&
                                buf->width > 0 && buf->height > 0 && buf->stride > 0) {
                                size_t end_off = (size_t)buf->offset + (size_t)buf->stride * (size_t)buf->height;
                                if (end_off <= client->pools[p].size) {
                                    uint8_t *pixels = client->pools[p].data + buf->offset;
                                    size_t frame_bytes = (size_t)buf->stride * (size_t)buf->height;
                                    if (frame_bytes > server->desktop_cap) {
                                        uint8_t *new_desk = (uint8_t *)realloc(server->desktop_pixels, frame_bytes);
                                        if (new_desk) {
                                            server->desktop_pixels = new_desk;
                                            server->desktop_cap = frame_bytes;
                                        }
                                    }
                                    if (server->desktop_pixels && server->desktop_cap >= frame_bytes) {
                                        memcpy(server->desktop_pixels, pixels, frame_bytes);
                                        server->desktop_w = buf->width;
                                        server->desktop_h = buf->height;
                                        server->desktop_stride = buf->stride;
                                        server->desktop_format = buf->format;
                                        server->last_rendered_width = buf->width;
                                        server->last_rendered_height = buf->height;
                                        present_composited_frame(server, client);
                                    } else if (server->render_cb) {
                                        server->render_cb(pixels, buf->width, buf->height,
                                                         buf->stride, buf->format, server->render_user_data);
                                    }
                                }
                            }
                            send_wire_message(client->fd, buf_id, WL_BUFFER_RELEASE, NULL, 0);
                            break;
                        }
                    }
                }

                // Always fire queued frame callbacks so Weston/client repaint loops continue
                if (surf->frame_cb_count > 0) {
                    uint32_t time_ms = get_current_time_ms();
                    for (int c = 0; c < surf->frame_cb_count; ++c) {
                        uint32_t cb_id = surf->frame_callbacks[c];
                        send_wire_message(client->fd, cb_id, WL_CALLBACK_DONE, &time_ms, sizeof(time_ms));
                        send_wire_message(client->fd, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, &cb_id, sizeof(cb_id));
                    }
                    surf->frame_cb_count = 0;
                }
            }
            return;
        }
    }
}

static void read_client_data(wayland_server_t *server, client_session_t *client) {
    uint8_t buffer[16384];
    struct msghdr msg = {0};
    struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(buffer) };
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 16)];

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t bytes_read = recvmsg(client->fd, &msg, 0);
    if (bytes_read <= 0) {
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        client_close(client);
        return;
    }

    int received_fds[16];
    int fd_count = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
            int n = (int)(data_len / sizeof(int));
            const int *fds = (const int *)CMSG_DATA(cmsg);
            for (int i = 0; i < n && fd_count < 16; ++i) {
                received_fds[fd_count++] = fds[i];
            }
        }
    }

    int fd_idx = 0;
    size_t offset = 0;
    while (offset + sizeof(wl_wire_header_t) <= (size_t)bytes_read) {
        const wl_wire_header_t *hdr = (const wl_wire_header_t *)(buffer + offset);
        uint16_t msg_size = wl_header_get_size(hdr->opcode_and_size);
        uint16_t opcode = wl_header_get_opcode(hdr->opcode_and_size);

        if (msg_size < sizeof(wl_wire_header_t) || offset + msg_size > (size_t)bytes_read) {
            break;
        }

        const uint8_t *payload = buffer + offset + sizeof(wl_wire_header_t);
        size_t payload_len = msg_size - sizeof(wl_wire_header_t);

        handle_client_message(server, client, hdr->object_id, opcode, payload, payload_len,
                              received_fds, &fd_idx, fd_count);

        offset += msg_size;
    }

    while (fd_idx < fd_count) {
        close(received_fds[fd_idx++]);
    }
}

int wayland_server_dispatch_step(wayland_server_t *server, int timeout_ms) {
    if (!server || server->listen_fd < 0) return -1;

    struct pollfd fds[1 + MAX_CLIENTS];
    int nfds = 0;

    pthread_mutex_lock(&server->lock);
    fds[0].fd = server->listen_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    nfds = 1;

    int client_map[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (server->clients[i].connected) {
            fds[nfds].fd = server->clients[i].fd;
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            fds[nfds].revents = 0;
            client_map[nfds - 1] = i;
            nfds++;
        }
    }
    pthread_mutex_unlock(&server->lock);

    int ret = poll(fds, (nfds_t)nfds, timeout_ms);
    if (ret <= 0) return ret;

    pthread_mutex_lock(&server->lock);
    if (fds[0].revents & POLLIN) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd >= 0) {
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            bool assigned = false;
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (!server->clients[i].connected) {
                    memset(&server->clients[i], 0, sizeof(client_session_t));
                    server->clients[i].fd = client_fd;
                    server->clients[i].connected = true;
                    assigned = true;
                    break;
                }
            }
            if (!assigned) {
                close(client_fd);
            }
        }
    }

    for (int i = 1; i < nfds; ++i) {
        int c_idx = client_map[i - 1];
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (server->clients[c_idx].connected) {
                read_client_data(server, &server->clients[c_idx]);
            }
        }
    }
    pthread_mutex_unlock(&server->lock);

    return 1;
}

void wayland_server_inject_pointer(wayland_server_t *server, int action, float norm_x, float norm_y) {
    if (!server) return;

    if (norm_x < 0.0f) norm_x = 0.0f;
    if (norm_x > 1.0f) norm_x = 1.0f;
    if (norm_y < 0.0f) norm_y = 0.0f;
    if (norm_y > 1.0f) norm_y = 1.0f;

    pthread_mutex_lock(&server->lock);

    int32_t w = server->last_rendered_width > 0 ? server->last_rendered_width :
                (server->output_width > 0 ? server->output_width : 1280);
    int32_t h = server->last_rendered_height > 0 ? server->last_rendered_height :
                (server->output_height > 0 ? server->output_height : 720);

    int32_t px = (int32_t)(norm_x * (float)w);
    int32_t py = (int32_t)(norm_y * (float)h);
    if (px >= w) px = w - 1;
    if (py >= h) py = h - 1;

    server->pointer_x = px;
    server->pointer_y = py;

    int32_t sx_fixed = (int32_t)(norm_x * (float)w * 256.0f);
    int32_t sy_fixed = (int32_t)(norm_y * (float)h * 256.0f);
    uint32_t time_ms = get_current_time_ms();

    client_session_t *active_cursor_client = NULL;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        client_session_t *client = &server->clients[i];
        if (!client->connected || client->pointer_id == 0) continue;

        uint32_t target_surf_id = 0;
        for (int s = 0; s < MAX_SURFACES; ++s) {
            if (!client->surfaces[s].active) continue;
            if (client->surfaces[s].id == client->cursor_surface_id) continue;
            if (client->surfaces[s].xdg_toplevel_id != 0 ||
                client->surfaces[s].xdg_surface_id != 0 ||
                client->surfaces[s].shell_surface_id != 0) {
                target_surf_id = client->surfaces[s].id;
                break;
            }
            if (target_surf_id == 0) {
                target_surf_id = client->surfaces[s].id;
            }
        }

        if (target_surf_id == 0) continue;

        ensure_keyboard_entered(client, target_surf_id);

        if (client->pointer_entered_surface_id != target_surf_id) {
            uint32_t serial = ++client->next_serial;
            uint32_t enter_payload[4];
            enter_payload[0] = serial;
            enter_payload[1] = target_surf_id;
            enter_payload[2] = (uint32_t)sx_fixed;
            enter_payload[3] = (uint32_t)sy_fixed;
            send_wire_message(client->fd, client->pointer_id, WL_POINTER_ENTER, enter_payload, sizeof(enter_payload));
            client->pointer_entered_surface_id = target_surf_id;
            if (client->seat_version >= 5) {
                send_wire_message(client->fd, client->pointer_id, WL_POINTER_FRAME, NULL, 0);
            }
        }

        uint32_t motion_payload[3];
        motion_payload[0] = time_ms;
        motion_payload[1] = (uint32_t)sx_fixed;
        motion_payload[2] = (uint32_t)sy_fixed;
        send_wire_message(client->fd, client->pointer_id, WL_POINTER_MOTION, motion_payload, sizeof(motion_payload));

        // action: 0 = left press, 1 = left release, 2 = motion, 3 = right press, 4 = right release
        if (action == 0 || action == 1 || action == 3 || action == 4) {
            uint32_t serial = ++client->next_serial;
            uint32_t btn_code = (action == 3 || action == 4) ? LINUX_BTN_RIGHT : LINUX_BTN_LEFT;
            uint32_t btn_state = (action == 0 || action == 3)
                ? WL_POINTER_BUTTON_STATE_PRESSED
                : WL_POINTER_BUTTON_STATE_RELEASED;
            uint32_t btn_payload[4];
            btn_payload[0] = serial;
            btn_payload[1] = time_ms;
            btn_payload[2] = btn_code;
            btn_payload[3] = btn_state;
            send_wire_message(client->fd, client->pointer_id, WL_POINTER_BUTTON, btn_payload, sizeof(btn_payload));
        }

        if (client->seat_version >= 5) {
            send_wire_message(client->fd, client->pointer_id, WL_POINTER_FRAME, NULL, 0);
        }

        if (client->has_cursor_buffer) {
            active_cursor_client = client;
        }
    }

    if (active_cursor_client) {
        present_composited_frame(server, active_cursor_client);
    }

    pthread_mutex_unlock(&server->lock);
}

void wayland_server_inject_key(wayland_server_t *server, int linux_keycode, bool pressed, bool shift_mod) {
    if (!server || linux_keycode <= 0) return;

    pthread_mutex_lock(&server->lock);
    uint32_t time_ms = get_current_time_ms();

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        client_session_t *client = &server->clients[i];
        if (!client->connected || client->keyboard_id == 0) continue;

        uint32_t target_surf_id = 0;
        for (int s = 0; s < MAX_SURFACES; ++s) {
            if (!client->surfaces[s].active) continue;
            if (client->surfaces[s].id == client->cursor_surface_id) continue;
            if (client->surfaces[s].xdg_toplevel_id != 0 ||
                client->surfaces[s].xdg_surface_id != 0 ||
                client->surfaces[s].shell_surface_id != 0) {
                target_surf_id = client->surfaces[s].id;
                break;
            }
            if (target_surf_id == 0) {
                target_surf_id = client->surfaces[s].id;
            }
        }

        if (target_surf_id != 0) {
            ensure_keyboard_entered(client, target_surf_id);
        }

        if (shift_mod && pressed) {
            uint32_t s1 = ++client->next_serial;
            uint32_t shift_down[4] = { s1, time_ms, LINUX_KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED };
            send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_KEY, shift_down, sizeof(shift_down));

            uint32_t s2 = ++client->next_serial;
            uint32_t mod_down[5] = { s2, 1U, 0U, 0U, 0U }; // bit 0 = ShiftMask
            send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_MODIFIERS, mod_down, sizeof(mod_down));
        }

        uint32_t k_serial = ++client->next_serial;
        uint32_t key_payload[4] = {
            k_serial,
            time_ms,
            (uint32_t)linux_keycode,
            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
        };
        send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_KEY, key_payload, sizeof(key_payload));

        if (shift_mod && !pressed) {
            uint32_t s1 = ++client->next_serial;
            uint32_t shift_up[4] = { s1, time_ms, LINUX_KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_RELEASED };
            send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_KEY, shift_up, sizeof(shift_up));

            uint32_t s2 = ++client->next_serial;
            uint32_t mod_up[5] = { s2, 0U, 0U, 0U, 0U };
            send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_MODIFIERS, mod_up, sizeof(mod_up));
        }
    }

    pthread_mutex_unlock(&server->lock);
}

static void *server_thread_worker(void *arg) {
    wayland_server_t *server = (wayland_server_t *)arg;
    while (server->running) {
        wayland_server_dispatch_step(server, 16);
    }
    return NULL;
}

wayland_server_t *wayland_server_create(const char *socket_dir, const char *socket_name) {
    wayland_server_t *server = (wayland_server_t *)calloc(1, sizeof(wayland_server_t));
    if (!server) return NULL;

    pthread_mutex_init(&server->lock, NULL);
    server->output_width = 1280;
    server->output_height = 720;
    server->pointer_x = 200;
    server->pointer_y = 200;

    mkdir(socket_dir, 0700);
    chmod(socket_dir, 0700);
    snprintf(server->socket_dir, sizeof(server->socket_dir), "%s", socket_dir);
    snprintf(server->socket_path, sizeof(server->socket_path), "%s/%s", socket_dir, socket_name);
    unlink(server->socket_path);

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        pthread_mutex_destroy(&server->lock);
        free(server);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, server->socket_path, sizeof(addr.sun_path) - 1);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server->listen_fd);
        pthread_mutex_destroy(&server->lock);
        free(server);
        return NULL;
    }

    if (listen(server->listen_fd, 8) < 0) {
        close(server->listen_fd);
        unlink(server->socket_path);
        pthread_mutex_destroy(&server->lock);
        free(server);
        return NULL;
    }

    int flags = fcntl(server->listen_fd, F_GETFL, 0);
    fcntl(server->listen_fd, F_SETFL, flags | O_NONBLOCK);

    return server;
}

void wayland_server_set_render_callback(wayland_server_t *server, wayland_frame_render_cb cb, void *user_data) {
    if (!server) return;
    pthread_mutex_lock(&server->lock);
    server->render_cb = cb;
    server->render_user_data = user_data;
    pthread_mutex_unlock(&server->lock);
}

void wayland_server_set_output_size(wayland_server_t *server, int width, int height) {
    if (!server || width <= 0 || height <= 0) return;
    pthread_mutex_lock(&server->lock);
    // Keep a consistent 1280x720 landscape desktop geometry so fonts/UI remain sharp and readable
    (void)width;
    (void)height;
    server->output_width = 1280;
    server->output_height = 720;
    pthread_mutex_unlock(&server->lock);
}

int wayland_server_start(wayland_server_t *server) {
    if (!server || server->running) return 0;
    server->running = true;
    if (pthread_create(&server->thread, NULL, server_thread_worker, server) != 0) {
        server->running = false;
        return -1;
    }
    server->thread_started = true;
    return 0;
}

void wayland_server_stop(wayland_server_t *server) {
    if (!server || !server->running) return;
    server->running = false;
    if (server->thread_started) {
        pthread_join(server->thread, NULL);
        server->thread_started = false;
    }
}

void wayland_server_destroy(wayland_server_t *server) {
    if (!server) return;
    wayland_server_stop(server);

    pthread_mutex_lock(&server->lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        client_close(&server->clients[i]);
    }

    if (server->desktop_pixels) {
        free(server->desktop_pixels);
        server->desktop_pixels = NULL;
        server->desktop_cap = 0;
    }
    if (server->composite_pixels) {
        free(server->composite_pixels);
        server->composite_pixels = NULL;
        server->composite_cap = 0;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    unlink(server->socket_path);
    pthread_mutex_unlock(&server->lock);
    pthread_mutex_destroy(&server->lock);
    free(server);
}
