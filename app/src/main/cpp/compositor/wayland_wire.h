#ifndef WAYLAND_WIRE_H
#define WAYLAND_WIRE_H

#include <stdint.h>

#define WL_DISPLAY_ID 1

// wl_display opcodes (requests)
#define WL_DISPLAY_SYNC 0
#define WL_DISPLAY_GET_REGISTRY 1

// wl_display events
#define WL_DISPLAY_ERROR 0
#define WL_DISPLAY_DELETE_ID 1

// wl_registry opcodes
#define WL_REGISTRY_BIND 0

// wl_registry events
#define WL_REGISTRY_GLOBAL 0
#define WL_REGISTRY_GLOBAL_REMOVE 1

// wl_compositor opcodes
#define WL_COMPOSITOR_CREATE_SURFACE 0
#define WL_COMPOSITOR_CREATE_REGION 1

// wl_subcompositor opcodes
#define WL_SUBCOMPOSITOR_DESTROY 0
#define WL_SUBCOMPOSITOR_GET_SUBSURFACE 1

// wl_shm opcodes
#define WL_SHM_CREATE_POOL 0

// wl_shm events
#define WL_SHM_FORMAT 0

// wl_shm formats
#define WL_SHM_FORMAT_ARGB8888 0
#define WL_SHM_FORMAT_XRGB8888 1

// wl_shm_pool opcodes
#define WL_SHM_POOL_CREATE_BUFFER 0
#define WL_SHM_POOL_DESTROY 1
#define WL_SHM_POOL_RESIZE 2

// wl_buffer opcodes & events
#define WL_BUFFER_DESTROY 0
#define WL_BUFFER_RELEASE 0

// wl_surface opcodes
#define WL_SURFACE_DESTROY 0
#define WL_SURFACE_ATTACH 1
#define WL_SURFACE_DAMAGE 2
#define WL_SURFACE_FRAME 3
#define WL_SURFACE_SET_OPAQUE_REGION 4
#define WL_SURFACE_SET_INPUT_REGION 5
#define WL_SURFACE_COMMIT 6
#define WL_SURFACE_SET_BUFFER_TRANSFORM 7
#define WL_SURFACE_SET_BUFFER_SCALE 8
#define WL_SURFACE_DAMAGE_BUFFER 9

// wl_output events
#define WL_OUTPUT_GEOMETRY 0
#define WL_OUTPUT_MODE 1
#define WL_OUTPUT_DONE 2
#define WL_OUTPUT_SCALE 3

#define WL_OUTPUT_MODE_CURRENT 0x1
#define WL_OUTPUT_MODE_PREFERRED 0x2

// wl_seat opcodes & events
#define WL_SEAT_GET_POINTER 0
#define WL_SEAT_GET_KEYBOARD 1
#define WL_SEAT_GET_TOUCH 2
#define WL_SEAT_RELEASE 3

#define WL_SEAT_CAPABILITIES 0
#define WL_SEAT_NAME 1

#define WL_SEAT_CAPABILITY_POINTER 1
#define WL_SEAT_CAPABILITY_KEYBOARD 2
#define WL_SEAT_CAPABILITY_TOUCH 4

// wl_pointer opcodes & events
#define WL_POINTER_SET_CURSOR 0
#define WL_POINTER_RELEASE 1

#define WL_POINTER_ENTER 0
#define WL_POINTER_LEAVE 1
#define WL_POINTER_MOTION 2
#define WL_POINTER_BUTTON 3
#define WL_POINTER_AXIS 4
#define WL_POINTER_FRAME 5

#define WL_POINTER_BUTTON_STATE_RELEASED 0
#define WL_POINTER_BUTTON_STATE_PRESSED 1
#define LINUX_BTN_LEFT 0x110
#define LINUX_BTN_RIGHT 0x111

// wl_keyboard opcodes & events
#define WL_KEYBOARD_RELEASE 0

#define WL_KEYBOARD_KEYMAP 0
#define WL_KEYBOARD_ENTER 1
#define WL_KEYBOARD_LEAVE 2
#define WL_KEYBOARD_KEY 3
#define WL_KEYBOARD_MODIFIERS 4
#define WL_KEYBOARD_REPEAT_INFO 5

#define WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP 0
#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1

#define WL_KEYBOARD_KEY_STATE_RELEASED 0
#define WL_KEYBOARD_KEY_STATE_PRESSED 1
#define LINUX_KEY_LEFTSHIFT 42

// xdg_wm_base opcodes & events
#define XDG_WM_BASE_DESTROY 0
#define XDG_WM_BASE_CREATE_POSITIONER 1
#define XDG_WM_BASE_GET_XDG_SURFACE 2
#define XDG_WM_BASE_PONG 3
#define XDG_WM_BASE_PING 0

// xdg_surface opcodes & events
#define XDG_SURFACE_DESTROY 0
#define XDG_SURFACE_GET_TOPLEVEL 1
#define XDG_SURFACE_GET_POPUP 2
#define XDG_SURFACE_SET_WINDOW_GEOMETRY 3
#define XDG_SURFACE_ACK_CONFIGURE 4
#define XDG_SURFACE_CONFIGURE 0

// xdg_toplevel opcodes & events
#define XDG_TOPLEVEL_DESTROY 0
#define XDG_TOPLEVEL_SET_FULLSCREEN 11
#define XDG_TOPLEVEL_CONFIGURE 0
#define XDG_TOPLEVEL_CLOSE 1

#define XDG_TOPLEVEL_STATE_MAXIMIZED 1
#define XDG_TOPLEVEL_STATE_FULLSCREEN 2
#define XDG_TOPLEVEL_STATE_RESIZING 3
#define XDG_TOPLEVEL_STATE_ACTIVATED 4

// wl_shell & wl_shell_surface opcodes/events
#define WL_SHELL_GET_SHELL_SURFACE 0
#define WL_SHELL_SURFACE_PONG 0
#define WL_SHELL_SURFACE_CONFIGURE 1

// wl_callback events
#define WL_CALLBACK_DONE 0

typedef struct {
    uint32_t object_id;
    uint32_t opcode_and_size;
} __attribute__((packed)) wl_wire_header_t;

static inline uint16_t wl_header_get_opcode(uint32_t opcode_and_size) {
    return (uint16_t)(opcode_and_size & 0xFFFF);
}

static inline uint16_t wl_header_get_size(uint32_t opcode_and_size) {
    return (uint16_t)((opcode_and_size >> 16) & 0xFFFF);
}

static inline uint32_t wl_header_pack(uint16_t opcode, uint16_t size) {
    return ((uint32_t)size << 16) | (uint32_t)opcode;
}

#endif // WAYLAND_WIRE_H
