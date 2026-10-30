#define _GNU_SOURCE
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../compositor/wayland_server.h"

#define LOG_TAG "NativeDesktopEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_LOG_LINES 200
#define LOG_LINE_LEN 512

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *s_window = NULL;
static wayland_server_t *s_server = NULL;
static pid_t s_child_pid = -1;
static int s_log_pipe_fd = -1;
static int s_stdin_pipe_fd = -1;
static pthread_t s_log_thread;
static bool s_log_thread_active = false;

// Cached last frame so frames rendered before SurfaceView creation are never lost
static uint8_t *s_last_frame = NULL;
static size_t s_last_frame_cap = 0;
static int s_last_frame_w = 0;
static int s_last_frame_h = 0;
static int s_last_frame_stride = 0;

// Circular log buffer for UI inspection
static char s_logs[MAX_LOG_LINES][LOG_LINE_LEN];
static int s_log_count = 0;
static int s_log_start = 0;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

static JavaVM *s_jvm = NULL;
static jobject s_repo_global_ref = NULL;
static jmethodID s_term_callback_method = NULL;

static void append_log_line(const char *line) {
    pthread_mutex_lock(&s_log_lock);
    int idx;
    if (s_log_count < MAX_LOG_LINES) {
        idx = (s_log_start + s_log_count) % MAX_LOG_LINES;
        s_log_count++;
    } else {
        idx = s_log_start;
        s_log_start = (s_log_start + 1) % MAX_LOG_LINES;
    }
    strncpy(s_logs[idx], line, LOG_LINE_LEN - 1);
    s_logs[idx][LOG_LINE_LEN - 1] = '\0';
    pthread_mutex_unlock(&s_log_lock);

    __android_log_print(ANDROID_LOG_INFO, "GuestDesktop", "%s", line);
}

static void notify_process_exit(int exit_code, const char *msg) {
    if (!s_jvm || !s_repo_global_ref || !s_term_callback_method) return;

    JNIEnv *env = NULL;
    int res = (*s_jvm)->GetEnv(s_jvm, (void **)&env, JNI_VERSION_1_6);
    bool should_detach = false;
    if (res == JNI_EDETACHED) {
        if ((*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL) != 0) return;
        should_detach = true;
    }

    jstring jmsg = (*env)->NewStringUTF(env, msg ? msg : "");
    (*env)->CallVoidMethod(env, s_repo_global_ref, s_term_callback_method, (jint)exit_code, jmsg);
    (*env)->DeleteLocalRef(env, jmsg);

    if (should_detach) {
        (*s_jvm)->DetachCurrentThread(s_jvm);
    }
}

static int patch_arm64_seccomp_syscalls(const char *elf_path) {
    int fd = open(elf_path, O_RDWR);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd);
        return 0;
    }

    uint8_t ident[SELFMAG];
    if (read(fd, ident, SELFMAG) != SELFMAG || memcmp(ident, ELFMAG, SELFMAG) != 0) {
        close(fd);
        return 0;
    }

    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return 0;
    }

    int patched_count = 0;
    uint8_t *base = (uint8_t *)map;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;

    if (ehdr->e_ident[EI_CLASS] == ELFCLASS64 &&
        ehdr->e_ident[EI_DATA] == ELFDATA2LSB &&
        ehdr->e_machine == EM_AARCH64 &&
        ehdr->e_phoff > 0 &&
        ehdr->e_phoff + (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr) <= (size_t)st.st_size) {

        Elf64_Phdr *phdrs = (Elf64_Phdr *)(base + ehdr->e_phoff);
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
            if (phdrs[i].p_type != PT_LOAD || (phdrs[i].p_flags & PF_X) == 0) {
                continue;
            }
            size_t seg_offset = (size_t)phdrs[i].p_offset;
            size_t seg_size = (size_t)phdrs[i].p_filesz;
            if (seg_offset + seg_size > (size_t)st.st_size || seg_size < 8) {
                continue;
            }

            uint32_t *insns = (uint32_t *)(base + seg_offset);
            size_t count = seg_size / 4;

            for (size_t idx = 1; idx < count; ++idx) {
                if (insns[idx] != 0xd4000001U) {
                    continue;
                }

                int syscall_nr = -1;
                for (size_t back = 1; back <= 8 && idx >= back; ++back) {
                    uint32_t prev = insns[idx - back];
                    if (prev == 0xd4000001U || prev == 0xd65f03c0U ||
                        (prev & 0x7c000000U) == 0x14000000U ||
                        (prev & 0xff000010U) == 0x54000000U) {
                        break;
                    }
                    if ((prev & 0xffe0001fU) == 0xd2800008U || (prev & 0xffe0001fU) == 0x52800008U) {
                        syscall_nr = (int)((prev >> 5) & 0xffffU);
                        break;
                    }
                }

                if (syscall_nr < 0) continue;

                switch (syscall_nr) {
                    case 99:  // __NR_set_robust_list
                    case 291: // __NR_statx
                    case 293: // __NR_rseq
                    case 435: // __NR_clone3
                    case 437: // __NR_openat2
                    case 439: // __NR_faccessat2
                    case 441: // __NR_epoll_pwait2
                    case 452: // __NR_fchmodat2
                        insns[idx] = 0x928004a0U; // movn x0, #37 (-ENOSYS)
                        patched_count++;
                        break;
                    case 54:  // __NR_fchownat
                    case 55:  // __NR_fchown
                    case 91:  // __NR_capset
                    case 144: // __NR_setgid
                    case 145: // __NR_setreuid
                    case 146: // __NR_setuid
                    case 147: // __NR_setresuid
                    case 148: // __NR_setregid
                    case 149: // __NR_setresgid
                    case 159: // __NR_setgroups
                        insns[idx] = 0xd2800000U; // movz x0, #0
                        patched_count++;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    if (patched_count > 0) {
        msync(map, (size_t)st.st_size, MS_SYNC);
    }
    munmap(map, (size_t)st.st_size);
    close(fd);
    return patched_count;
}

static void patch_directory_recursive(const char *dir_path, int depth) {
    if (depth > 3) return;
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    char child_path[PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (lstat(child_path, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            chmod(child_path, 0755);
            patch_directory_recursive(child_path, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            chmod(child_path, 0755);
            patch_arm64_seccomp_syscalls(child_path);
        }
    }
    closedir(dir);
}

static void write_weston_config(const char *rootfs) {
    char etc_dir[PATH_MAX];
    char xdg_dir[PATH_MAX];
    char weston_dir[PATH_MAX];
    char ini_path[PATH_MAX];

    snprintf(etc_dir, sizeof(etc_dir), "%s/etc", rootfs);
    snprintf(xdg_dir, sizeof(xdg_dir), "%s/etc/xdg", rootfs);
    snprintf(weston_dir, sizeof(weston_dir), "%s/etc/xdg/weston", rootfs);
    snprintf(ini_path, sizeof(ini_path), "%s/etc/xdg/weston/weston.ini", rootfs);

    mkdir(etc_dir, 0755);
    mkdir(xdg_dir, 0755);
    mkdir(weston_dir, 0755);

    FILE *fp = fopen(ini_path, "w");
    if (fp) {
        fprintf(fp,
            "[core]\n"
            "backend=wayland-backend.so\n"
            "shell=desktop-shell.so\n"
            "use-pixman=true\n"
            "require-input=false\n"
            "idle-time=0\n\n"
            "[shell]\n"
            "locking=false\n"
            "animation=none\n"
            "panel-position=top\n"
            "background-color=0xff16202c\n\n"
            "[launcher]\n"
            "icon=%s/usr/share/weston/icon_terminal.png\n"
            "path=/usr/bin/foot\n",
            rootfs
        );
        fclose(fp);
    }
}

static void prepare_rootfs_binaries(const char *rootfs, const char *ld_path, const char *exec_path) {
    patch_arm64_seccomp_syscalls(ld_path);
    patch_arm64_seccomp_syscalls(exec_path);
    write_weston_config(rootfs);

    const char *scan_dirs[] = {
        "/usr/bin",
        "/bin",
        "/usr/libexec",
        "/usr/lib/aarch64-linux-gnu"
    };
    char dir_path[PATH_MAX];
    for (size_t i = 0; i < sizeof(scan_dirs) / sizeof(scan_dirs[0]); ++i) {
        snprintf(dir_path, sizeof(dir_path), "%s%s", rootfs, scan_dirs[i]);
        patch_directory_recursive(dir_path, 0);
    }
}

static void *log_reader_worker(void *arg) {
    int read_fd = (int)(intptr_t)arg;
    char buffer[1024];
    char line_buf[LOG_LINE_LEN];
    int line_pos = 0;

    while (1) {
        ssize_t n = read(read_fd, buffer, sizeof(buffer));
        if (n <= 0) break;

        for (ssize_t i = 0; i < n; ++i) {
            char c = buffer[i];
            if (c == '\n' || line_pos >= LOG_LINE_LEN - 2) {
                line_buf[line_pos] = '\0';
                if (line_pos > 0) {
                    append_log_line(line_buf);
                }
                line_pos = 0;
            } else if (c != '\r') {
                line_buf[line_pos++] = c;
            }
        }
    }

    if (line_pos > 0) {
        line_buf[line_pos] = '\0';
        append_log_line(line_buf);
    }

    close(read_fd);

    pthread_mutex_lock(&s_lock);
    if (s_stdin_pipe_fd >= 0) {
        close(s_stdin_pipe_fd);
        s_stdin_pipe_fd = -1;
    }
    pid_t pid_to_wait = s_child_pid;
    pthread_mutex_unlock(&s_lock);

    if (pid_to_wait > 0) {
        int status = 0;
        waitpid(pid_to_wait, &status, 0);

        char exit_msg[256];
        int exit_code = -1;
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
            snprintf(exit_msg, sizeof(exit_msg), "Proses guest keluar normal dengan kode: %d", exit_code);
        } else if (WIFSIGNALED(status)) {
            exit_code = 128 + WTERMSIG(status);
            snprintf(exit_msg, sizeof(exit_msg), "Proses guest dihentikan oleh sinyal: %d", WTERMSIG(status));
        } else {
            snprintf(exit_msg, sizeof(exit_msg), "Proses guest keluar dengan status tak dikenal.");
        }
        append_log_line(exit_msg);
        notify_process_exit(exit_code, exit_msg);

        pthread_mutex_lock(&s_lock);
        if (s_child_pid == pid_to_wait) {
            s_child_pid = -1;
        }
        pthread_mutex_unlock(&s_lock);
    }

    return NULL;
}

static void blit_frame_to_window_locked(const uint8_t *pixels, int width, int height, int stride) {
    if (!s_window || !pixels || width <= 0 || height <= 0) return;

    ANativeWindow_setBuffersGeometry(s_window, width, height, WINDOW_FORMAT_RGBA_8888);

    ANativeWindow_Buffer win_buffer;
    if (ANativeWindow_lock(s_window, &win_buffer, NULL) == 0) {
        uint8_t *dst = (uint8_t *)win_buffer.bits;
        const uint8_t *src = pixels;
        int copy_w = width < win_buffer.width ? width : win_buffer.width;
        int copy_h = height < win_buffer.height ? height : win_buffer.height;

        for (int y = 0; y < copy_h; ++y) {
            const uint32_t *src_row = (const uint32_t *)(src + (y * stride));
            uint32_t *dst_row = (uint32_t *)(dst + (y * win_buffer.stride * 4));

            for (int x = 0; x < copy_w; ++x) {
                uint32_t pixel = src_row[x];
                uint8_t b = (pixel >> 0) & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t r = (pixel >> 16) & 0xFF;
                dst_row[x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000U;
            }
        }

        ANativeWindow_unlockAndPost(s_window);
    }
}

static void on_frame_render(const uint8_t *pixels, int width, int height, int stride, uint32_t format, void *user_data) {
    (void)format;
    (void)user_data;

    if (!pixels || width <= 0 || height <= 0 || stride <= 0) return;

    pthread_mutex_lock(&s_lock);
    size_t needed = (size_t)stride * (size_t)height;
    if (needed > s_last_frame_cap) {
        uint8_t *new_buf = (uint8_t *)realloc(s_last_frame, needed);
        if (new_buf) {
            s_last_frame = new_buf;
            s_last_frame_cap = needed;
        }
    }
    if (s_last_frame && s_last_frame_cap >= needed) {
        memcpy(s_last_frame, pixels, needed);
        s_last_frame_w = width;
        s_last_frame_h = height;
        s_last_frame_stride = stride;
    }

    blit_frame_to_window_locked(pixels, width, height, stride);
    pthread_mutex_unlock(&s_lock);
}

static void find_dynamic_linker(const char *rootfs, char *out_ld, size_t max_len) {
    const char *candidates[] = {
        "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
        "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
        "/usr/lib/ld-linux-aarch64.so.1",
        "/lib/ld-linux-aarch64.so.1",
        "/lib64/ld-linux-x86-64.so.2",
        "/usr/lib64/ld-linux-x86-64.so.2",
        "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
        "/lib/ld-musl-aarch64.so.1",
        "/usr/lib/ld-musl-aarch64.so.1",
        "/lib/ld-linux-armhf.so.3"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        snprintf(out_ld, max_len, "%s%s", rootfs, candidates[i]);
        if (access(out_ld, F_OK) == 0) {
            chmod(out_ld, 0755);
            return;
        }
    }
    snprintf(out_ld, max_len, "%s/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1", rootfs);
    chmod(out_ld, 0755);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    s_jvm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jboolean JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeInit(
    JNIEnv *env, jobject thiz, jstring socket_dir, jstring socket_name) {

    if (s_repo_global_ref) {
        (*env)->DeleteGlobalRef(env, s_repo_global_ref);
    }
    s_repo_global_ref = (*env)->NewGlobalRef(env, thiz);
    jclass cls = (*env)->GetObjectClass(env, thiz);
    s_term_callback_method = (*env)->GetMethodID(env, cls, "onNativeProcessTerminated", "(ILjava/lang/String;)V");

    const char *c_dir = (*env)->GetStringUTFChars(env, socket_dir, NULL);
    const char *c_name = (*env)->GetStringUTFChars(env, socket_name, NULL);

    chmod(c_dir, 0700);

    pthread_mutex_lock(&s_lock);
    s_last_frame_w = 0;
    s_last_frame_h = 0;
    s_last_frame_stride = 0;

    if (s_server) {
        wayland_server_destroy(s_server);
        s_server = NULL;
    }

    s_server = wayland_server_create(c_dir, c_name);
    if (!s_server) {
        LOGE("Failed to create Wayland server");
        pthread_mutex_unlock(&s_lock);
        (*env)->ReleaseStringUTFChars(env, socket_dir, c_dir);
        (*env)->ReleaseStringUTFChars(env, socket_name, c_name);
        return JNI_FALSE;
    }

    if (s_window) {
        int w = ANativeWindow_getWidth(s_window);
        int h = ANativeWindow_getHeight(s_window);
        if (w > 0 && h > 0) {
            wayland_server_set_output_size(s_server, w, h);
        }
    }

    wayland_server_set_render_callback(s_server, on_frame_render, NULL);
    int res = wayland_server_start(s_server);

    pthread_mutex_unlock(&s_lock);

    (*env)->ReleaseStringUTFChars(env, socket_dir, c_dir);
    (*env)->ReleaseStringUTFChars(env, socket_name, c_name);

    return (res == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeSetSurface(
    JNIEnv *env, jobject thiz, jobject surface) {
    (void)thiz;

    pthread_mutex_lock(&s_lock);
    if (s_window) {
        ANativeWindow_release(s_window);
        s_window = NULL;
    }

    if (surface) {
        s_window = ANativeWindow_fromSurface(env, surface);
        if (s_window) {
            int w = ANativeWindow_getWidth(s_window);
            int h = ANativeWindow_getHeight(s_window);
            if (s_server && w > 0 && h > 0) {
                wayland_server_set_output_size(s_server, w, h);
            }
            // Immediately render cached frame if guest already committed before SurfaceView was ready
            if (s_last_frame && s_last_frame_w > 0 && s_last_frame_h > 0) {
                blit_frame_to_window_locked(s_last_frame, s_last_frame_w, s_last_frame_h, s_last_frame_stride);
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
}

JNIEXPORT jboolean JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeStartSession(
    JNIEnv *env, jobject thiz,
    jstring rootfs_path, jstring shim_path, jstring socket_dir,
    jstring exec_path, jobjectArray args, jobjectArray env_vars) {
    (void)thiz;

    const char *c_rootfs = (*env)->GetStringUTFChars(env, rootfs_path, NULL);
    const char *c_shim = (*env)->GetStringUTFChars(env, shim_path, NULL);
    const char *c_sockdir = (*env)->GetStringUTFChars(env, socket_dir, NULL);
    const char *c_exec = (*env)->GetStringUTFChars(env, exec_path, NULL);

    char ld_path[PATH_MAX];
    find_dynamic_linker(c_rootfs, ld_path, sizeof(ld_path));

    char rootfs_tmp[PATH_MAX];
    snprintf(rootfs_tmp, sizeof(rootfs_tmp), "%s/tmp", c_rootfs);
    mkdir(rootfs_tmp, 0777);
    chmod(rootfs_tmp, 0777);
    chmod(c_sockdir, 0700);
    chmod(ld_path, 0755);
    chmod(c_exec, 0755);
    chmod(c_shim, 0755);

    prepare_rootfs_binaries(c_rootfs, ld_path, c_exec);

    char lib_path[PATH_MAX * 3];
    snprintf(lib_path, sizeof(lib_path),
             "%s/usr/lib/aarch64-linux-gnu:%s/lib/aarch64-linux-gnu:%s/usr/lib:%s/lib:"
             "%s/usr/lib/aarch64-linux-gnu/libweston-13:%s/usr/lib/aarch64-linux-gnu/libweston-12:"
             "%s/usr/lib/aarch64-linux-gnu/libweston-10:%s/usr/lib/aarch64-linux-gnu/weston:"
             "%s/usr/lib/x86_64-linux-gnu",
             c_rootfs, c_rootfs, c_rootfs, c_rootfs,
             c_rootfs, c_rootfs, c_rootfs, c_rootfs, c_rootfs);

    int arg_count = (*env)->GetArrayLength(env, args);
    size_t exec_len = strlen(c_exec);
    bool is_weston = (exec_len >= 7 && strcmp(c_exec + exec_len - 7, "/weston") == 0);

    char weston_cfg_arg[PATH_MAX];
    snprintf(weston_cfg_arg, sizeof(weston_cfg_arg), "--config=%s/etc/xdg/weston/weston.ini", c_rootfs);

    int extra_weston_args = (is_weston && arg_count == 0) ? 4 : 0;
    char **argv = (char **)calloc((size_t)arg_count + (size_t)extra_weston_args + 5, sizeof(char *));
    argv[0] = strdup(ld_path);
    argv[1] = strdup("--library-path");
    argv[2] = strdup(lib_path);
    argv[3] = strdup(c_exec);

    if (is_weston && arg_count == 0) {
        argv[4] = strdup("--backend=wayland-backend.so");
        argv[5] = strdup("--use-pixman");
        argv[6] = strdup("--fullscreen");
        argv[7] = strdup(weston_cfg_arg);
        argv[8] = NULL;
    } else {
        for (int i = 0; i < arg_count; ++i) {
            jstring arg = (jstring)(*env)->GetObjectArrayElement(env, args, i);
            const char *arg_str = (*env)->GetStringUTFChars(env, arg, NULL);
            argv[i + 4] = strdup(arg_str);
            (*env)->ReleaseStringUTFChars(env, arg, arg_str);
            (*env)->DeleteLocalRef(env, arg);
        }
        argv[arg_count + 4] = NULL;
    }

    int custom_env_count = (*env)->GetArrayLength(env, env_vars);
    char **envp = (char **)calloc((size_t)custom_env_count + 24, sizeof(char *));
    for (int i = 0; i < custom_env_count; ++i) {
        jstring var = (jstring)(*env)->GetObjectArrayElement(env, env_vars, i);
        const char *var_str = (*env)->GetStringUTFChars(env, var, NULL);
        envp[i] = strdup(var_str);
        (*env)->ReleaseStringUTFChars(env, var, var_str);
        (*env)->DeleteLocalRef(env, var);
    }

    char buf[PATH_MAX * 3];
    int idx = custom_env_count;

    snprintf(buf, sizeof(buf), "ROOTFS=%s", c_rootfs);
    envp[idx++] = strdup(buf);

    if (access(c_shim, R_OK) == 0) {
        snprintf(buf, sizeof(buf), "LD_PRELOAD=%s", c_shim);
        envp[idx++] = strdup(buf);
    }

    snprintf(buf, sizeof(buf), "LD_LIBRARY_PATH=%s", lib_path);
    envp[idx++] = strdup(buf);

    envp[idx++] = strdup("GLIBC_TUNABLES=glibc.pthread.rseq=0");

    snprintf(buf, sizeof(buf), "XDG_RUNTIME_DIR=%s", c_sockdir);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "TMPDIR=%s", c_sockdir);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "XKB_CONFIG_ROOT=%s/usr/share/X11/xkb", c_rootfs);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "FONTCONFIG_PATH=%s/etc/fonts", c_rootfs);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "FONTCONFIG_FILE=%s/etc/fonts/fonts.conf", c_rootfs);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "WESTON_DATA_DIR=%s/usr/share/weston", c_rootfs);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "XDG_CONFIG_DIRS=%s/etc/xdg:/etc/xdg", c_rootfs);
    envp[idx++] = strdup(buf);

    snprintf(buf, sizeof(buf), "XDG_DATA_DIRS=%s/usr/local/share:%s/usr/share:/usr/local/share:/usr/share", c_rootfs, c_rootfs);
    envp[idx++] = strdup(buf);

    envp[idx++] = strdup("WAYLAND_DISPLAY=wayland-0");
    envp[idx++] = strdup("XDG_SESSION_TYPE=wayland");
    envp[idx++] = strdup("HOME=/root");
    envp[idx++] = strdup("USER=root");
    envp[idx++] = strdup("LOGNAME=root");
    envp[idx++] = strdup("SHELL=/bin/sh");
    envp[idx++] = strdup("PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:/usr/libexec");
    envp[idx++] = strdup("LANG=C.UTF-8");
    envp[idx] = NULL;

    int pipefd[2] = { -1, -1 };
    int stdin_pipe[2] = { -1, -1 };
    if (pipe(pipefd) < 0 || pipe(stdin_pipe) < 0) {
        LOGE("Failed to create pipes for child session");
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
        if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
        for (int i = 0; argv[i] != NULL; ++i) free(argv[i]);
        free(argv);
        for (int i = 0; envp[i] != NULL; ++i) free(envp[i]);
        free(envp);
        (*env)->ReleaseStringUTFChars(env, rootfs_path, c_rootfs);
        (*env)->ReleaseStringUTFChars(env, shim_path, c_shim);
        (*env)->ReleaseStringUTFChars(env, socket_dir, c_sockdir);
        (*env)->ReleaseStringUTFChars(env, exec_path, c_exec);
        return JNI_FALSE;
    }

    char start_log[512];
    snprintf(start_log, sizeof(start_log), "[SYSTEM] Meluncurkan biner via ld.so: %s %s", ld_path, c_exec);
    append_log_line(start_log);

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        close(stdin_pipe[1]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(pipefd[1]);

        chdir(c_rootfs);

        execve(ld_path, argv, envp);

        perror("execve failed in child");
        _exit(127);
    }

    close(pipefd[1]);
    close(stdin_pipe[0]);

    pthread_mutex_lock(&s_lock);
    if (s_stdin_pipe_fd >= 0) {
        close(s_stdin_pipe_fd);
    }
    s_stdin_pipe_fd = stdin_pipe[1];
    s_child_pid = pid;
    s_log_pipe_fd = pipefd[0];
    pthread_mutex_unlock(&s_lock);

    // If launching an interactive shell (/bin/sh, /bin/bash, /bin/dash) with no arguments,
    // automatically start Weston or Foot if installed, or print diagnostic instructions.
    if (pid > 0 && arg_count == 0 &&
        ((exec_len >= 3 && strcmp(c_exec + exec_len - 3, "/sh") == 0) ||
         (exec_len >= 5 && strcmp(c_exec + exec_len - 5, "/bash") == 0) ||
         (exec_len >= 5 && strcmp(c_exec + exec_len - 5, "/dash") == 0))) {
        char init_cmd[1024];
        snprintf(init_cmd, sizeof(init_cmd),
            "echo \"[GUEST] Shell Linux aktif ($(uname -srm))\";\n"
            "if [ -x /usr/bin/weston ]; then\n"
            "  echo \"[GUEST] Meluncurkan Weston Wayland Desktop...\";\n"
            "  /usr/bin/weston --backend=wayland-backend.so --use-pixman --fullscreen --config=%s/etc/xdg/weston/weston.ini || /usr/bin/foot;\n"
            "elif [ -x /usr/bin/foot ]; then\n"
            "  echo \"[GUEST] Meluncurkan terminal grafis Foot...\";\n"
            "  /usr/bin/foot;\n"
            "else\n"
            "  echo \"[GUEST] /usr/bin/weston belum ditemukan di rootfs ini.\";\n"
            "  echo \"[GUEST] Silakan tekan Hentikan Sesi lalu Pilih Berkas Rootfs (.tar.gz) ulang untuk mengekstrak penuh nativedesktop-ubuntu-arm64.tar.gz.\";\n"
            "fi\n",
            c_rootfs
        );
        ssize_t written = write(s_stdin_pipe_fd, init_cmd, strlen(init_cmd));
        (void)written;
    }

    if (pthread_create(&s_log_thread, NULL, log_reader_worker, (void *)(intptr_t)s_log_pipe_fd) == 0) {
        pthread_detach(s_log_thread);
        s_log_thread_active = true;
    }

    for (int i = 0; argv[i] != NULL; ++i) free(argv[i]);
    free(argv);
    for (int i = 0; envp[i] != NULL; ++i) free(envp[i]);
    free(envp);

    (*env)->ReleaseStringUTFChars(env, rootfs_path, c_rootfs);
    (*env)->ReleaseStringUTFChars(env, shim_path, c_shim);
    (*env)->ReleaseStringUTFChars(env, socket_dir, c_sockdir);
    (*env)->ReleaseStringUTFChars(env, exec_path, c_exec);

    return (pid > 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeGetLogs(
    JNIEnv *env, jobject thiz) {
    (void)thiz;

    pthread_mutex_lock(&s_log_lock);
    jclass string_cls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, s_log_count, string_cls, NULL);

    for (int i = 0; i < s_log_count; ++i) {
        int idx = (s_log_start + i) % MAX_LOG_LINES;
        jstring str = (*env)->NewStringUTF(env, s_logs[idx]);
        (*env)->SetObjectArrayElement(env, arr, i, str);
        (*env)->DeleteLocalRef(env, str);
    }
    pthread_mutex_unlock(&s_log_lock);

    return arr;
}

JNIEXPORT void JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeStopSession(
    JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;

    pthread_mutex_lock(&s_lock);
    if (s_stdin_pipe_fd >= 0) {
        close(s_stdin_pipe_fd);
        s_stdin_pipe_fd = -1;
    }

    if (s_child_pid > 0) {
        kill(s_child_pid, SIGTERM);
        int status;
        waitpid(s_child_pid, &status, WNOHANG);
        s_child_pid = -1;
    }

    if (s_server) {
        wayland_server_stop(s_server);
        wayland_server_destroy(s_server);
        s_server = NULL;
    }

    if (s_window) {
        ANativeWindow_release(s_window);
        s_window = NULL;
    }
    pthread_mutex_unlock(&s_lock);
}
