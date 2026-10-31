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
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../compositor/wayland_server.h"

#define LOG_TAG "NativeDesktopEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_LOG_LINES 200
#define LOG_LINE_LEN 512

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_render_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *s_window = NULL;
static wayland_server_t *s_server = NULL;
static pid_t s_child_pid = -1;
static int s_log_pipe_fd = -1;
static int s_stdin_pipe_fd = -1;
static pthread_t s_log_thread;
static bool s_log_thread_active = false;

static int s_target_res_w = 1280;
static int s_target_res_h = 720;

// Cached last frame so frames rendered before SurfaceView creation are never lost
static uint8_t *s_last_frame = NULL;
static size_t s_last_frame_cap = 0;
static int s_last_frame_w = 0;
static int s_last_frame_h = 0;
static int s_last_frame_stride = 0;

// Real-time measured frame telemetry (from actual Wayland buffer commits)
static uint64_t s_last_frame_ts_ns = 0;
static uint64_t s_fps_window_start_ns = 0;
static uint32_t s_fps_window_frames = 0;
static float s_measured_fps = 0.0f;
static float s_measured_frametime_ms = 0.0f;

// Circular log buffer for UI inspection
static char s_logs[MAX_LOG_LINES][LOG_LINE_LEN];
static int s_log_count = 0;
static int s_log_start = 0;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

static JavaVM *s_jvm = NULL;
static jobject s_repo_global_ref = NULL;
static jmethodID s_term_callback_method = NULL;

static uint64_t monotonic_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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

static void write_engine_configs(const char *rootfs) {
    char root_home[PATH_MAX];
    char games_dir[PATH_MAX];
    char usr_dir[PATH_MAX];
    char usr_local_dir[PATH_MAX];
    char usr_local_bin_dir[PATH_MAX];
    char etc_dir[PATH_MAX];
    char xdg_dir[PATH_MAX];
    char weston_dir[PATH_MAX];
    char foot_dir[PATH_MAX];
    char ini_path[PATH_MAX];
    char kiosk_ini_path[PATH_MAX];
    char foot_ini_path[PATH_MAX];
    char runner_path[PATH_MAX];

    snprintf(root_home, sizeof(root_home), "%s/root", rootfs);
    snprintf(games_dir, sizeof(games_dir), "%s/games", rootfs);
    snprintf(usr_dir, sizeof(usr_dir), "%s/usr", rootfs);
    snprintf(usr_local_dir, sizeof(usr_local_dir), "%s/usr/local", rootfs);
    snprintf(usr_local_bin_dir, sizeof(usr_local_bin_dir), "%s/usr/local/bin", rootfs);
    snprintf(etc_dir, sizeof(etc_dir), "%s/etc", rootfs);
    snprintf(xdg_dir, sizeof(xdg_dir), "%s/etc/xdg", rootfs);
    snprintf(weston_dir, sizeof(weston_dir), "%s/etc/xdg/weston", rootfs);
    snprintf(foot_dir, sizeof(foot_dir), "%s/etc/xdg/foot", rootfs);
    snprintf(ini_path, sizeof(ini_path), "%s/etc/xdg/weston/weston.ini", rootfs);
    snprintf(kiosk_ini_path, sizeof(kiosk_ini_path), "%s/etc/xdg/weston/weston-kiosk.ini", rootfs);
    snprintf(foot_ini_path, sizeof(foot_ini_path), "%s/etc/xdg/foot/foot.ini", rootfs);
    snprintf(runner_path, sizeof(runner_path), "%s/usr/local/bin/game-runner", rootfs);

    mkdir(root_home, 0755);
    mkdir(games_dir, 0755);
    mkdir(usr_dir, 0755);
    mkdir(usr_local_dir, 0755);
    mkdir(usr_local_bin_dir, 0755);
    mkdir(etc_dir, 0755);
    mkdir(xdg_dir, 0755);
    mkdir(weston_dir, 0755);
    mkdir(foot_dir, 0755);

    FILE *ffp = fopen(foot_ini_path, "w");
    if (ffp) {
        fprintf(ffp,
            "[main]\n"
            "shell=/bin/bash\n"
            "term=xterm-256color\n"
            "font=monospace:size=11\n"
            "pad=8x8\n\n"
            "[csd]\n"
            "preferred=client\n"
            "size=24\n"
        );
        fclose(ffp);
    }

    FILE *kfp = fopen(kiosk_ini_path, "w");
    if (kfp) {
        fprintf(kfp,
            "[core]\n"
            "backend=wayland-backend.so\n"
            "shell=kiosk-shell.so\n"
            "use-pixman=true\n"
            "require-input=false\n"
            "idle-time=0\n"
        );
        fclose(kfp);
    }

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
            "background-color=0xff121212\n\n"
            "[launcher]\n"
            "icon=%s/usr/share/weston/icon_terminal.png\n"
            "path=/usr/bin/foot -c %s/etc/xdg/foot/foot.ini -l error /bin/bash\n",
            rootfs,
            rootfs
        );
        fclose(fp);
    }

    FILE *rfp = fopen(runner_path, "w");
    if (rfp) {
        fprintf(rfp,
            "#!/bin/bash\n"
            "export HOME=\"${HOME:-%s/root}\"\n"
            "export WINEPREFIX=\"${WINEPREFIX:-$HOME/.wine}\"\n"
            "export WINEARCH=\"win64\"\n"
            "export WINEDEBUG=\"${WINEDEBUG:--all}\"\n"
            "export BOX64_PATH=\"/opt/wine/bin:/usr/local/bin:/usr/bin:/bin\"\n"
            "export BOX64_LD_LIBRARY_PATH=\"/opt/wine/lib/wine/x86_64-unix:/opt/wine/lib64/wine/x86_64-unix:/usr/lib/x86_64-linux-gnu:/lib64:/usr/lib/aarch64-linux-gnu\"\n"
            "export BOX64_LOG=\"${BOX64_LOG:-0}\"\n"
            "export BOX64_NOBANNER=1\n\n"
            "echo \"[ENGINE] SoC GPU Backend : ${ND_GPU_BACKEND:-auto}\"\n"
            "echo \"[ENGINE] DX Wrapper      : ${ND_DX_WRAPPER:-dxvk}\"\n"
            "echo \"[ENGINE] Dynarec Preset  : ${ND_DYNAREC_PRESET:-performance}\"\n"
            "echo \"[ENGINE] Render Res      : ${ND_RESOLUTION:-1280x720}\"\n"
            "echo \"[ENGINE] Display Mode    : ${ND_DISPLAY_MODE:-wayland_direct}\"\n\n"
            "export MESA_GL_VERSION_OVERRIDE=4.6\n"
            "export MESA_GLSL_VERSION_OVERRIDE=460\n"
            "export MESA_NO_ERROR=1\n"
            "export PAN_MESA_DEBUG=gofaster,gl3\n\n"
            "case \"${ND_GPU_BACKEND:-auto}\" in\n"
            "  adreno_turnip)\n"
            "    if [ -f /usr/share/vulkan/icd.d/freedreno_icd.aarch64.json ]; then\n"
            "      export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json\n"
            "    fi\n"
            "    export TU_DEBUG=noconform\n"
            "    ;;\n"
            "  mali_universal)\n"
            "    export DXVK_CONFIG=\"d3d11.relaxedBarriers = True; d3d9.floatEmulation = True; dxvk.enableAsync = True\"\n"
            "    if [ -e /dev/mali0 ] && [ -f /usr/share/vulkan/icd.d/panfrost_icd.aarch64.json ]; then\n"
            "      export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/panfrost_icd.aarch64.json:/usr/share/vulkan/icd.d/lvp_icd.aarch64.json\n"
            "    elif [ -f /usr/share/vulkan/icd.d/lvp_icd.aarch64.json ]; then\n"
            "      export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json\n"
            "    fi\n"
            "    ;;\n"
            "  software_lvp)\n"
            "    if [ -f /usr/share/vulkan/icd.d/lvp_icd.aarch64.json ]; then\n"
            "      export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json\n"
            "    fi\n"
            "    ;;\n"
            "  *)\n"
            "    if [ -e /dev/kgsl-3d0 ] && [ -f /usr/share/vulkan/icd.d/freedreno_icd.aarch64.json ]; then\n"
            "      export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json:/usr/share/vulkan/icd.d/lvp_icd.aarch64.json\n"
            "      export TU_DEBUG=noconform\n"
            "    elif [ -f /usr/share/vulkan/icd.d/lvp_icd.aarch64.json ]; then\n"
            "      export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json\n"
            "      export DXVK_CONFIG=\"d3d11.relaxedBarriers = True; d3d9.floatEmulation = True; dxvk.enableAsync = True\"\n"
            "    fi\n"
            "    ;;\n"
            "esac\n\n"
            "BOX64_BIN=\"\"\n"
            "if [ -x /usr/local/bin/box64 ]; then BOX64_BIN=\"/usr/local/bin/box64\";\n"
            "elif [ -x /usr/bin/box64 ]; then BOX64_BIN=\"/usr/bin/box64\"; fi\n\n"
            "WINE_BIN=\"\"\n"
            "if [ -x /opt/wine/bin/wine64 ]; then WINE_BIN=\"/opt/wine/bin/wine64\";\n"
            "elif [ -x /opt/wine/bin/wine ]; then WINE_BIN=\"/opt/wine/bin/wine\";\n"
            "elif [ -x /usr/bin/wine64 ]; then WINE_BIN=\"/usr/bin/wine64\"; fi\n\n"
            "TARGET_CMD=\"${1:-winecfg}\"\n"
            "shift || true\n\n"
            "if [ \"$TARGET_CMD\" = \"terminal\" ]; then\n"
            "  exec /usr/bin/foot -c %s/etc/xdg/foot/foot.ini -l error /bin/bash\n"
            "fi\n\n"
            "if [ -z \"$BOX64_BIN\" ] || [ -z \"$WINE_BIN\" ]; then\n"
            "  echo \"[ENGINE] Komponen Box64 / Wine belum ditemukan di rootfs ini.\"\n"
            "  echo \"[ENGINE] Silakan unduh dan pilih paket rootfs v2.0.0-rootfs terbaru dari GitHub Releases.\"\n"
            "  if [ -x /usr/bin/foot ]; then\n"
            "    exec /usr/bin/foot -c %s/etc/xdg/foot/foot.ini -l error /bin/bash -c 'echo \"=== NATIVE GAME ENGINE DIAGNOSTICS ===\"; echo \"Box64 & Wine belum terpasang di rootfs lama ini.\"; echo \"Unduh nativedesktop-ubuntu-arm64.tar.gz dari Release v2.0.0-rootfs.\"; echo \"\"; exec /bin/bash'\n"
            "  fi\n"
            "  exit 1\n"
            "fi\n\n"
            "if [ ! -d \"$WINEPREFIX/drive_c\" ]; then\n"
            "  echo \"[ENGINE] Menginisialisasi Wine Prefix (64-bit WoW64)...\"\n"
            "  mkdir -p \"$WINEPREFIX/dosdevices\"\n"
            "  \"$BOX64_BIN\" \"$WINE_BIN\" wineboot --init || true\n"
            "  ln -sfn \"/storage/emulated/0/Download\" \"$WINEPREFIX/dosdevices/d:\" 2>/dev/null || true\n"
            "  ln -sfn \"%s/games\" \"$WINEPREFIX/dosdevices/e:\" 2>/dev/null || true\n"
            "fi\n\n"
            "if [ \"${ND_DISPLAY_MODE:-wayland_direct}\" = \"wayland_direct\" ]; then\n"
            "  unset DISPLAY\n"
            "  \"$BOX64_BIN\" \"$WINE_BIN\" reg add \"HKCU\\\\Software\\\\Wine\\\\Drivers\" /v Graphics /t REG_SZ /d wayland /f >/dev/null 2>&1 || true\n"
            "fi\n\n"
            "if [ \"${ND_DX_WRAPPER:-dxvk}\" = \"dxvk\" ] && [ -d /opt/dxvk/x64 ]; then\n"
            "  if [ ! -f \"$WINEPREFIX/.dxvk_installed\" ]; then\n"
            "    echo \"[ENGINE] Memasang modul DXVK ke system32/syswow64...\"\n"
            "    cp -f /opt/dxvk/x64/*.dll \"$WINEPREFIX/drive_c/windows/system32/\" 2>/dev/null || true\n"
            "    if [ -d \"$WINEPREFIX/drive_c/windows/syswow64\" ] && [ -d /opt/dxvk/x32 ]; then\n"
            "      cp -f /opt/dxvk/x32/*.dll \"$WINEPREFIX/drive_c/windows/syswow64/\" 2>/dev/null || true\n"
            "    fi\n"
            "    touch \"$WINEPREFIX/.dxvk_installed\"\n"
            "  fi\n"
            "  export WINEDLLOVERRIDES=\"d3d11,d3d10core,d3d9,dxgi=n,b\"\n"
            "else\n"
            "  export WINEDLLOVERRIDES=\"d3d11,d3d10core,d3d9,dxgi=b\"\n"
            "fi\n\n"
            "if [ \"${ND_DISPLAY_MODE:-wayland_direct}\" = \"kiosk_xwayland\" ] && [ -x /usr/bin/weston ]; then\n"
            "  /usr/bin/weston --backend=wayland-backend.so --use-pixman --fullscreen --config=%s/etc/xdg/weston/weston-kiosk.ini &\n"
            "  sleep 1\n"
            "  export DISPLAY=:0\n"
            "fi\n\n"
            "echo \"[ENGINE] Meluncurkan eksekusi Windows: $TARGET_CMD $*\"\n"
            "exec \"$BOX64_BIN\" \"$WINE_BIN\" \"$TARGET_CMD\" \"$@\"\n",
            rootfs,
            rootfs,
            rootfs,
            rootfs,
            rootfs
        );
        fclose(rfp);
        chmod(runner_path, 0755);
    }
}

static void prepare_rootfs_binaries(const char *rootfs, const char *ld_path, const char *exec_path) {
    patch_arm64_seccomp_syscalls(ld_path);
    patch_arm64_seccomp_syscalls(exec_path);
    write_engine_configs(rootfs);

    const char *scan_dirs[] = {
        "/usr/local/bin",
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

static void apply_performance_cpu_affinity(void) {
    long max_freqs[16];
    long min_freq = 0x7fffffffL;
    long peak_freq = 0;
    int valid_cores = 0;

    for (int i = 0; i < 16; ++i) {
        max_freqs[i] = -1;
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[32];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                long f = atol(buf);
                if (f > 0) {
                    max_freqs[i] = f;
                    if (f < min_freq) min_freq = f;
                    if (f > peak_freq) peak_freq = f;
                    valid_cores++;
                }
            }
        }
    }

    if (valid_cores > 1 && peak_freq > min_freq) {
        cpu_set_t set;
        CPU_ZERO(&set);
        int big_count = 0;
        for (int i = 0; i < 16; ++i) {
            if (max_freqs[i] > min_freq) {
                CPU_SET(i, &set);
                big_count++;
            }
        }
        if (big_count > 0) {
            sched_setaffinity(0, sizeof(set), &set);
        }
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
            snprintf(exit_msg, sizeof(exit_msg), "Proses engine selesai dengan kode: %d", exit_code);
        } else if (WIFSIGNALED(status)) {
            exit_code = 128 + WTERMSIG(status);
            snprintf(exit_msg, sizeof(exit_msg), "Proses engine dihentikan sinyal: %d", WTERMSIG(status));
        } else {
            snprintf(exit_msg, sizeof(exit_msg), "Proses engine berhenti.");
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

    uint64_t now_ns = monotonic_now_ns();

    pthread_mutex_lock(&s_render_lock);
    if (s_last_frame_ts_ns > 0 && now_ns > s_last_frame_ts_ns) {
        s_measured_frametime_ms = (float)(now_ns - s_last_frame_ts_ns) / 1000000.0f;
    }
    s_last_frame_ts_ns = now_ns;

    if (s_fps_window_start_ns == 0) {
        s_fps_window_start_ns = now_ns;
        s_fps_window_frames = 1;
    } else {
        s_fps_window_frames++;
        uint64_t elapsed_ns = now_ns - s_fps_window_start_ns;
        if (elapsed_ns >= 500000000ULL) {
            s_measured_fps = ((float)s_fps_window_frames * 1000000000.0f) / (float)elapsed_ns;
            s_fps_window_start_ns = now_ns;
            s_fps_window_frames = 0;
        }
    }

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
    pthread_mutex_unlock(&s_render_lock);
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

JNIEXPORT void JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeSetResolution(
    JNIEnv *env, jobject thiz, jint width, jint height) {
    (void)env;
    (void)thiz;

    if (width <= 0 || height <= 0) return;

    pthread_mutex_lock(&s_lock);
    s_target_res_w = (int)width;
    s_target_res_h = (int)height;
    if (s_server) {
        wayland_server_set_output_size(s_server, s_target_res_w, s_target_res_h);
    }
    pthread_mutex_unlock(&s_lock);
}

JNIEXPORT jfloatArray JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeGetPerfMetrics(
    JNIEnv *env, jobject thiz) {
    (void)thiz;

    float metrics[4];
    pthread_mutex_lock(&s_render_lock);
    uint64_t now_ns = monotonic_now_ns();
    if (s_last_frame_ts_ns > 0 && (now_ns - s_last_frame_ts_ns) > 2000000000ULL) {
        s_measured_fps = 0.0f;
    }
    metrics[0] = s_measured_fps;
    metrics[1] = s_measured_frametime_ms;
    metrics[2] = (float)(s_last_frame_w > 0 ? s_last_frame_w : s_target_res_w);
    metrics[3] = (float)(s_last_frame_h > 0 ? s_last_frame_h : s_target_res_h);
    pthread_mutex_unlock(&s_render_lock);

    jfloatArray out = (*env)->NewFloatArray(env, 4);
    if (out) {
        (*env)->SetFloatArrayRegion(env, out, 0, 4, metrics);
    }
    return out;
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
    pthread_mutex_lock(&s_render_lock);
    s_last_frame_w = 0;
    s_last_frame_h = 0;
    s_last_frame_stride = 0;
    s_last_frame_ts_ns = 0;
    s_fps_window_start_ns = 0;
    s_fps_window_frames = 0;
    s_measured_fps = 0.0f;
    s_measured_frametime_ms = 0.0f;
    pthread_mutex_unlock(&s_render_lock);

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

    wayland_server_set_output_size(
        s_server,
        s_target_res_w > 0 ? s_target_res_w : 1280,
        s_target_res_h > 0 ? s_target_res_h : 720
    );

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

    pthread_mutex_lock(&s_render_lock);
    if (s_window) {
        ANativeWindow_release(s_window);
        s_window = NULL;
    }

    if (surface) {
        s_window = ANativeWindow_fromSurface(env, surface);
        if (s_window) {
            if (s_last_frame && s_last_frame_w > 0 && s_last_frame_h > 0) {
                blit_frame_to_window_locked(s_last_frame, s_last_frame_w, s_last_frame_h, s_last_frame_stride);
            }
        }
    }
    pthread_mutex_unlock(&s_render_lock);
}

JNIEXPORT void JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeInjectPointer(
    JNIEnv *env, jobject thiz, jint action, jfloat norm_x, jfloat norm_y) {
    (void)env;
    (void)thiz;

    pthread_mutex_lock(&s_lock);
    if (s_server) {
        wayland_server_inject_pointer(s_server, (int)action, (float)norm_x, (float)norm_y);
    }
    pthread_mutex_unlock(&s_lock);
}

JNIEXPORT void JNICALL
Java_com_core_desktop_data_repository_DesktopEngineRepository_nativeInjectKey(
    JNIEnv *env, jobject thiz, jint linux_keycode, jboolean pressed, jboolean shift_mod) {
    (void)env;
    (void)thiz;

    pthread_mutex_lock(&s_lock);
    if (s_server) {
        wayland_server_inject_key(s_server, (int)linux_keycode, (bool)pressed, (bool)shift_mod);
    }
    pthread_mutex_unlock(&s_lock);
}

static bool is_file_elf_arm64(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char hdr[20];
    ssize_t n = read(fd, hdr, sizeof(hdr));
    close(fd);
    if (n >= 20 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        uint16_t machine = (uint16_t)hdr[18] | ((uint16_t)hdr[19] << 8);
        return (machine == EM_AARCH64);
    }
    return false;
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
             "%s/usr/lib/aarch64-linux-gnu:%s/lib/aarch64-linux-gnu:%s/usr/local/lib:%s/usr/lib:%s/lib:"
             "%s/usr/lib/aarch64-linux-gnu/libweston-13:%s/usr/lib/aarch64-linux-gnu/libweston-12:"
             "%s/usr/lib/aarch64-linux-gnu/libweston-10:%s/usr/lib/aarch64-linux-gnu/weston:"
             "%s/opt/wine/lib:%s/opt/wine/lib64:%s/usr/lib/x86_64-linux-gnu",
             c_rootfs, c_rootfs, c_rootfs, c_rootfs, c_rootfs,
             c_rootfs, c_rootfs, c_rootfs, c_rootfs,
             c_rootfs, c_rootfs, c_rootfs);

    int arg_count = (*env)->GetArrayLength(env, args);
    size_t exec_len = strlen(c_exec);
    bool is_weston = (exec_len >= 7 && strcmp(c_exec + exec_len - 7, "/weston") == 0);
    bool direct_arm64_elf = is_file_elf_arm64(c_exec);

    char bash_bin[PATH_MAX];
    snprintf(bash_bin, sizeof(bash_bin), "%s/bin/bash", c_rootfs);
    if (access(bash_bin, X_OK) != 0) {
        snprintf(bash_bin, sizeof(bash_bin), "%s/bin/sh", c_rootfs);
    }

    char weston_cfg_arg[PATH_MAX];
    snprintf(weston_cfg_arg, sizeof(weston_cfg_arg), "--config=%s/etc/xdg/weston/weston.ini", c_rootfs);

    char **argv = (char **)calloc((size_t)arg_count + 12, sizeof(char *));
    int a_idx = 0;
    argv[a_idx++] = strdup(ld_path);
    argv[a_idx++] = strdup("--library-path");
    argv[a_idx++] = strdup(lib_path);

    if (direct_arm64_elf) {
        argv[a_idx++] = strdup(c_exec);
        if (is_weston && arg_count == 0) {
            argv[a_idx++] = strdup("--backend=wayland-backend.so");
            argv[a_idx++] = strdup("--use-pixman");
            argv[a_idx++] = strdup("--fullscreen");
            argv[a_idx++] = strdup(weston_cfg_arg);
        } else {
            for (int i = 0; i < arg_count; ++i) {
                jstring arg = (jstring)(*env)->GetObjectArrayElement(env, args, i);
                const char *arg_str = (*env)->GetStringUTFChars(env, arg, NULL);
                argv[a_idx++] = strdup(arg_str);
                (*env)->ReleaseStringUTFChars(env, arg, arg_str);
                (*env)->DeleteLocalRef(env, arg);
            }
        }
    } else {
        // Script (/usr/local/bin/game-runner) or Windows .exe launched via bash + game-runner
        argv[a_idx++] = strdup(bash_bin);
        bool is_exe = (exec_len >= 4 &&
                       (strcasecmp(c_exec + exec_len - 4, ".exe") == 0 ||
                        strcasecmp(c_exec + exec_len - 4, ".msi") == 0 ||
                        strcasecmp(c_exec + exec_len - 4, ".bat") == 0));
        if (is_exe) {
            char runner_script[PATH_MAX];
            snprintf(runner_script, sizeof(runner_script), "%s/usr/local/bin/game-runner", c_rootfs);
            argv[a_idx++] = strdup(runner_script);
        }
        argv[a_idx++] = strdup(c_exec);
        for (int i = 0; i < arg_count; ++i) {
            jstring arg = (jstring)(*env)->GetObjectArrayElement(env, args, i);
            const char *arg_str = (*env)->GetStringUTFChars(env, arg, NULL);
            argv[a_idx++] = strdup(arg_str);
            (*env)->ReleaseStringUTFChars(env, arg, arg_str);
            (*env)->DeleteLocalRef(env, arg);
        }
    }
    argv[a_idx] = NULL;

    bool lock_perf_cores = true;
    int custom_env_count = (*env)->GetArrayLength(env, env_vars);
    char **envp = (char **)calloc((size_t)custom_env_count + 40, sizeof(char *));
    for (int i = 0; i < custom_env_count; ++i) {
        jstring var = (jstring)(*env)->GetObjectArrayElement(env, env_vars, i);
        const char *var_str = (*env)->GetStringUTFChars(env, var, NULL);
        if (strncmp(var_str, "ND_PERF_CORES=0", 15) == 0) {
            lock_perf_cores = false;
        }
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

    snprintf(buf, sizeof(buf), "LOCPATH=%s/usr/lib/locale", c_rootfs);
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

    snprintf(buf, sizeof(buf), "HOME=%s/root", c_rootfs);
    envp[idx++] = strdup(buf);

    envp[idx++] = strdup("WAYLAND_DISPLAY=wayland-0");
    envp[idx++] = strdup("XDG_SESSION_TYPE=wayland");
    envp[idx++] = strdup("USER=root");
    envp[idx++] = strdup("LOGNAME=root");
    envp[idx++] = strdup("SHELL=/bin/bash");
    envp[idx++] = strdup("TERM=xterm-256color");
    envp[idx++] = strdup("PATH=/opt/wine/bin:/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:/usr/libexec");
    envp[idx++] = strdup("LANG=C.UTF-8");
    envp[idx++] = strdup("LC_ALL=C.UTF-8");
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
    snprintf(start_log, sizeof(start_log), "[ENGINE] Memulai pipeline native (%dx%d): %s",
             s_target_res_w, s_target_res_h, c_exec);
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

        // Raise open file descriptor limit for Wine Esync/Fsync
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            rl.rlim_cur = rl.rlim_max > 65536 ? 65536 : rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }

        // Minimize timer slack for game loop frame pacing
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

        // Pin to Performance/Prime CPU cores across MediaTek Dimensity / Snapdragon / Exynos
        if (lock_perf_cores) {
            apply_performance_cpu_affinity();
        }

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

    pthread_mutex_lock(&s_render_lock);
    if (s_window) {
        ANativeWindow_release(s_window);
        s_window = NULL;
    }
    s_measured_fps = 0.0f;
    s_measured_frametime_ms = 0.0f;
    pthread_mutex_unlock(&s_render_lock);
    pthread_mutex_unlock(&s_lock);
}
