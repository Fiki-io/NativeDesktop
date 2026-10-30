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

typedef struct {
    uint32_t id;
    int fd;
    size_t size;
    uint8_t *data;
    bool active;
} shm_pool_t;

typedef struct {
    uint32_t id;
    uint32_t pool_id;
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

    shm_pool_t pools[MAX_POOLS];
    shm_buffer_t buffers[MAX_BUFFERS];
    surface_t surfaces[MAX_SURFACES];
} client_session_t;

struct wayland_server {
    int listen_fd;
    char socket_path[PATH_MAX];
    bool running;

    int output_width;
    int output_height;

    client_session_t clients[MAX_CLIENTS];

    wayland_frame_render_cb render_cb;
    void *render_user_data;

    pthread_t thread;
    bool thread_started;
};

static uint32_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
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
    mode_payload[1] = server->output_width > 0 ? server->output_width : 800;
    mode_payload[2] = server->output_height > 0 ? server->output_height : 1280;
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

    // Advertise pointer capability so Weston/clients initialize without blocking on a parent keymap fd
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
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
    int32_t w = server->output_width > 0 ? server->output_width : 800;
    int32_t h = server->output_height > 0 ? server->output_height : 1280;

    if (surf->xdg_toplevel_id != 0) {
        // xdg_toplevel.configure: [int32 width, int32 height, array states ([ACTIVATED=4])]
        uint32_t top_cfg[4];
        top_cfg[0] = (uint32_t)w;
        top_cfg[1] = (uint32_t)h;
        top_cfg[2] = sizeof(uint32_t); // 1 state in array (4 bytes)
        top_cfg[3] = XDG_TOPLEVEL_STATE_ACTIVATED;
        send_wire_message(client->fd, surf->xdg_toplevel_id, XDG_TOPLEVEL_CONFIGURE, top_cfg, sizeof(top_cfg));
    }

    if (surf->xdg_surface_id != 0) {
        uint32_t serial = ++client->next_serial;
        send_wire_message(client->fd, surf->xdg_surface_id, XDG_SURFACE_CONFIGURE, &serial, sizeof(serial));
        surf->configured = true;
    }
}

static void client_close(client_session_t *client) {
    if (!client->connected) return;

    for (int i = 0; i < MAX_POOLS; ++i) {
        if (client->pools[i].active) {
            if (client->pools[i].data != MAP_FAILED && client->pools[i].data != NULL) {
                munmap(client->pools[i].data, client->pools[i].size);
            }
            if (client->pools[i].fd >= 0) {
                close(client->pools[i].fd);
            }
            client->pools[i].active = false;
        }
    }
    for (int i = 0; i < MAX_BUFFERS; ++i) {
        client->buffers[i].active = false;
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
        } else if (opcode == WL_SEAT_GET_KEYBOARD && payload_len >= 4) {
            client->keyboard_id = *(const uint32_t *)payload;
            if (client->seat_version >= 4) {
                int32_t repeat[2] = { 40, 400 };
                send_wire_message(client->fd, client->keyboard_id, WL_KEYBOARD_REPEAT_INFO, repeat, sizeof(repeat));
            }
        } else if (opcode == WL_SEAT_GET_TOUCH && payload_len >= 4) {
            client->touch_id = *(const uint32_t *)payload;
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
        if (client->pools[i].active && client->pools[i].id == object_id) {
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
                        client->buffers[b].id = buf_id;
                        client->buffers[b].pool_id = client->pools[i].id;
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
                client->pools[i].active = false;
                if (client->pools[i].data != MAP_FAILED && client->pools[i].data != NULL) {
                    munmap(client->pools[i].data, client->pools[i].size);
                    client->pools[i].data = NULL;
                }
                if (client->pools[i].fd >= 0) {
                    close(client->pools[i].fd);
                    client->pools[i].fd = -1;
                }
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
                client->buffers[b].active = false;
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
                if (surf->attached_buffer_id == 0 && surf->xdg_surface_id != 0 && !surf->configured) {
                    send_xdg_configure(server, client, surf);
                }

                uint32_t buf_id = surf->attached_buffer_id;
                if (buf_id != 0) {
                    for (int b = 0; b < MAX_BUFFERS; ++b) {
                        if (client->buffers[b].active && client->buffers[b].id == buf_id) {
                            shm_buffer_t *buf = &client->buffers[b];
                            for (int p = 0; p < MAX_POOLS; ++p) {
                                if (client->pools[p].active && client->pools[p].id == buf->pool_id &&
                                    client->pools[p].data != MAP_FAILED && client->pools[p].data != NULL) {
                                    size_t end_off = (size_t)buf->offset + (size_t)buf->stride * (size_t)buf->height;
                                    if (end_off <= client->pools[p].size) {
                                        uint8_t *pixels = client->pools[p].data + buf->offset;
                                        if (server->render_cb) {
                                            server->render_cb(pixels, buf->width, buf->height,
                                                             buf->stride, buf->format, server->render_user_data);
                                        }
                                    }
                                    break;
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

    int ret = poll(fds, (nfds_t)nfds, timeout_ms);
    if (ret <= 0) return ret;

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
            read_client_data(server, &server->clients[c_idx]);
        }
    }

    return 1;
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

    server->output_width = 800;
    server->output_height = 1280;

    mkdir(socket_dir, 0700);
    chmod(socket_dir, 0700);
    snprintf(server->socket_path, sizeof(server->socket_path), "%s/%s", socket_dir, socket_name);
    unlink(server->socket_path);

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        free(server);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, server->socket_path, sizeof(addr.sun_path) - 1);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    if (listen(server->listen_fd, 8) < 0) {
        close(server->listen_fd);
        unlink(server->socket_path);
        free(server);
        return NULL;
    }

    int flags = fcntl(server->listen_fd, F_GETFL, 0);
    fcntl(server->listen_fd, F_SETFL, flags | O_NONBLOCK);

    return server;
}

void wayland_server_set_render_callback(wayland_server_t *server, wayland_frame_render_cb cb, void *user_data) {
    if (!server) return;
    server->render_cb = cb;
    server->render_user_data = user_data;
}

void wayland_server_set_output_size(wayland_server_t *server, int width, int height) {
    if (!server || width <= 0 || height <= 0) return;
    server->output_width = width;
    server->output_height = height;
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

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        client_close(&server->clients[i]);
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    unlink(server->socket_path);
    free(server);
}
