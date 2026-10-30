cmake_minimum_required(VERSION 3.22.1)
project(desktop_engine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 1. libdesktop_shim.so (Injected into guest Linux processes via LD_PRELOAD)
# Built standalone (-nostdlib -nodefaultlibs) so it has zero DT_NEEDED on Android Bionic
# and can be loaded directly by guest glibc/musl ld-linux dynamic linkers.
set(SAVED_C_STANDARD_LIBS "${CMAKE_C_STANDARD_LIBRARIES}")
set(CMAKE_C_STANDARD_LIBRARIES "")
add_library(desktop_shim SHARED
    shim/desktop_shim.c
)
set(CMAKE_C_STANDARD_LIBRARIES "${SAVED_C_STANDARD_LIBS}")
target_include_directories(desktop_shim PRIVATE shim)
target_compile_options(desktop_shim PRIVATE
    -fno-stack-protector
    -fno-builtin
    -fPIC
    -O2
)
target_link_options(desktop_shim PRIVATE
    -nostdlib
    -nodefaultlibs
    -Wl,--as-needed
    -Wl,--hash-style=both
)

# 2. libdesktop_engine.so (Loaded by Android Application via System.loadLibrary)
add_library(desktop_engine SHARED
    compositor/wayland_server.c
    jni/desktop_engine_jni.c
)
target_include_directories(desktop_engine PRIVATE
    compositor
    jni
)
target_link_libraries(desktop_engine PRIVATE
    android
    log
)

