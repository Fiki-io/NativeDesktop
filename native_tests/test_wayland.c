#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include "../app/src/main/cpp/compositor/wayland_server.h"
#include "../app/src/main/cpp/compositor/wayland_wire.h"

static int g_frame_rendered = 0;
static uint32_t g_last_pixel = 0;

static void test_render_callback(const uint8_t *pixels, int width, int height, int stride, uint32_t format, void *user_data) {
    (void)height;
    (void)stride;
    (void)format;
    (void)user_data;

    g_frame_rendered = 1;
    if (width > 0 && pixels) {
        g_last_pixel = *(const uint32_t *)pixels;
    }
}

static void send_msg_with_fd(int sock, uint32_t obj_id, uint16_t opcode, const void *payload, size_t payload_len, int send_fd) {
    size_t total_len = sizeof(wl_wire_header_t) + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total_len);
    wl_wire_header_t *hdr = (wl_wire_header_t *)buf;
    hdr->object_id = obj_id;
    hdr->opcode_and_size = wl_header_pack(opcode, (uint16_t)total_len);
    if (payload && payload_len > 0) {
        memcpy(buf + sizeof(wl_wire_header_t), payload, payload_len);
    }

    struct msghdr msg = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = total_len };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    if (send_fd >= 0) {
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        *((int *)CMSG_DATA(cmsg)) = send_fd;
    }

    sendmsg(sock, &msg, 0);
    free(buf);
}

int main(void) {
    const char *socket_dir = "/tmp/test_wl_dir";
    const char *socket_name = "wayland-0";

    wayland_server_t *server = wayland_server_create(socket_dir, socket_name);
    assert(server != NULL);

    wayland_server_set_render_callback(server, test_render_callback, NULL);
    assert(wayland_server_start(server) == 0);

    // Connect test client
    int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(client_sock >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", socket_dir, socket_name);

    int conn_res = connect(client_sock, (struct sockaddr *)&addr, sizeof(addr));
    assert(conn_res == 0);

    // Step 1: Send wl_display.get_registry (obj=1, opcode=1, registry_id=2)
    uint32_t registry_id = 2;
    send_msg_with_fd(client_sock, WL_DISPLAY_ID, WL_DISPLAY_GET_REGISTRY, &registry_id, sizeof(registry_id), -1);

    // Sleep briefly to let server process
    usleep(50000);

    // Step 2: Bind wl_compositor (id=3) and wl_shm (id=4)
    // Bind payload: [name=1, strlen=13+1, "wl_compositor", pad, version=4, id=3]
    uint8_t bind_comp[32];
    memset(bind_comp, 0, sizeof(bind_comp));
    uint32_t *p32 = (uint32_t *)bind_comp;
    p32[0] = 1; // name
    p32[1] = 14; // string len
    memcpy(bind_comp + 8, "wl_compositor", 14);
    p32[6] = 4; // version
    p32[7] = 3; // new_id
    send_msg_with_fd(client_sock, registry_id, WL_REGISTRY_BIND, bind_comp, sizeof(bind_comp), -1);

    // Bind wl_shm (name=2, id=4)
    uint8_t bind_shm[28];
    memset(bind_shm, 0, sizeof(bind_shm));
    p32 = (uint32_t *)bind_shm;
    p32[0] = 2; // name
    p32[1] = 7; // string len
    memcpy(bind_shm + 8, "wl_shm", 7);
    p32[4] = 1; // version
    p32[5] = 4; // new_id
    send_msg_with_fd(client_sock, registry_id, WL_REGISTRY_BIND, bind_shm, sizeof(bind_shm), -1);

    usleep(50000);

    // Step 3: Create surface (obj=3 compositor, opcode=0, surface_id=5)
    uint32_t surface_id = 5;
    send_msg_with_fd(client_sock, 3, WL_COMPOSITOR_CREATE_SURFACE, &surface_id, sizeof(surface_id), -1);

    // Step 4: Create memfd and draw test pattern
    int shm_fd = (int)syscall(__NR_memfd_create, "test_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(shm_fd >= 0);

    int width = 32;
    int height = 32;
    int stride = width * 4;
    size_t pool_size = stride * height;
    assert(ftruncate(shm_fd, pool_size) == 0);

    uint32_t *pixels = (uint32_t *)mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    assert(pixels != MAP_FAILED);

    for (int i = 0; i < width * height; ++i) {
        pixels[i] = 0xAABBCCDD; // Test pixel
    }
    munmap(pixels, pool_size);

    // Step 5: Send create_pool (obj=4 wl_shm, opcode=0, payload: [pool_id=6, size=pool_size], send_fd=shm_fd)
    uint32_t pool_payload[2] = { 6, (uint32_t)pool_size };
    send_msg_with_fd(client_sock, 4, WL_SHM_CREATE_POOL, pool_payload, sizeof(pool_payload), shm_fd);
    close(shm_fd);

    usleep(50000);

    // Step 6: Create buffer from pool (obj=6 pool, opcode=0, payload: [buf_id=7, offset=0, width, height, stride, format=0])
    uint32_t buf_payload[6] = { 7, 0, (uint32_t)width, (uint32_t)height, (uint32_t)stride, WL_SHM_FORMAT_ARGB8888 };
    send_msg_with_fd(client_sock, 6, WL_SHM_POOL_CREATE_BUFFER, buf_payload, sizeof(buf_payload), -1);

    // Step 7: Attach buffer to surface (obj=5 surface, opcode=1, payload: [buf_id=7, x=0, y=0])
    uint32_t attach_payload[3] = { 7, 0, 0 };
    send_msg_with_fd(client_sock, surface_id, WL_SURFACE_ATTACH, attach_payload, sizeof(attach_payload), -1);

    // Step 8: Commit surface (obj=5 surface, opcode=6)
    send_msg_with_fd(client_sock, surface_id, WL_SURFACE_COMMIT, NULL, 0, -1);

    // Wait for frame to be processed by compositor thread (up to 1s)
    for (int t = 0; t < 20 && !g_frame_rendered; ++t) {
        usleep(50000);
    }

    close(client_sock);

    printf("Frame rendered flag: %d\n", g_frame_rendered);
    printf("Last pixel value: 0x%08X (Expected: 0xAABBCCDD)\n", g_last_pixel);

    assert(g_frame_rendered == 1);
    assert(g_last_pixel == 0xAABBCCDD);

    wayland_server_stop(server);
    wayland_server_destroy(server);

    printf("ALL WAYLAND SERVER TESTS PASSED!\n");
    return 0;
}
