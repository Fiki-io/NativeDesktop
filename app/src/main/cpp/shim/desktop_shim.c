#define _GNU_SOURCE
#include "desktop_shim.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

#ifndef __OPEN_NEEDS_MODE
#ifdef O_TMPFILE
#define __OPEN_NEEDS_MODE(flags) (((flags) & O_CREAT) != 0 || (((flags) & O_TMPFILE) == O_TMPFILE))
#else
#define __OPEN_NEEDS_MODE(flags) (((flags) & O_CREAT) != 0)
#endif
#endif

// Weak symbols resolved at runtime against guest glibc (libc.so.6) or host libc
extern int *__errno_location(void) __attribute__((weak));
extern int *__errno(void) __attribute__((weak));
extern char *getenv(const char *name) __attribute__((weak));
extern char **environ __attribute__((weak));
extern int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) __attribute__((weak));
extern void *fdopen(int fd, const char *mode) __attribute__((weak));
extern void *fdopendir(int fd) __attribute__((weak));
extern void *dlmopen(long lmid, const char *filename, int flags) __attribute__((weak));
extern int fclose(void *stream) __attribute__((weak));

static char s_rootfs_buf[PATH_MAX];
static const char *s_rootfs = NULL;
static size_t s_rootfs_len = 0;

// Self-contained string & memory utilities so libdesktop_shim.so requires zero libc DT_NEEDED
static size_t shim_strlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len] != '\0') len++;
    return len;
}

static int shim_strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];
        if (c1 != c2) return (int)c1 - (int)c2;
        if (c1 == '\0') return 0;
    }
    return 0;
}

static void shim_strncpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    if (src) {
        for (; i < max_len - 1 && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void shim_concat2(char *dst, size_t max_len, const char *s1, const char *s2) {
    if (!dst || max_len == 0) return;
    size_t pos = 0;
    if (s1) {
        for (size_t i = 0; s1[i] != '\0' && pos < max_len - 1; ++i) {
            dst[pos++] = s1[i];
        }
    }
    if (s2) {
        for (size_t i = 0; s2[i] != '\0' && pos < max_len - 1; ++i) {
            dst[pos++] = s2[i];
        }
    }
    dst[pos] = '\0';
}

static void shim_concat3(char *dst, size_t max_len, const char *s1, const char *s2, const char *s3) {
    if (!dst || max_len == 0) return;
    size_t pos = 0;
    const char *parts[3] = { s1, s2, s3 };
    for (int p = 0; p < 3; ++p) {
        const char *s = parts[p];
        if (!s) continue;
        for (size_t i = 0; s[i] != '\0' && pos < max_len - 1; ++i) {
            dst[pos++] = s[i];
        }
    }
    dst[pos] = '\0';
}

__attribute__((visibility("hidden"), used))
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

__attribute__((visibility("hidden"), used))
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)c;
    return s;
}

// Direct Linux kernel syscall primitives
static inline long raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
#if defined(__aarch64__)
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "memory", "cc"
    );
    return x0;
#elif defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory", "cc"
    );
    return ret;
#else
    return syscall(nr, a0, a1, a2, a3, a4, a5);
#endif
}

static inline long raw_syscall1(long nr, long a0) {
    return raw_syscall6(nr, a0, 0, 0, 0, 0, 0);
}

static inline long raw_syscall2(long nr, long a0, long a1) {
    return raw_syscall6(nr, a0, a1, 0, 0, 0, 0);
}

static inline long raw_syscall3(long nr, long a0, long a1, long a2) {
    return raw_syscall6(nr, a0, a1, a2, 0, 0, 0);
}

static inline long raw_syscall4(long nr, long a0, long a1, long a2, long a3) {
    return raw_syscall6(nr, a0, a1, a2, a3, 0, 0);
}

static void set_errno_val(int err) {
    if (__errno_location) {
        *__errno_location() = err;
    } else if (__errno) {
        *__errno() = err;
    }
}

static long set_errno_return(long ret) {
    if (ret < 0 && ret >= -4095) {
        set_errno_val((int)(-ret));
        return -1;
    }
    return ret;
}

static void load_rootfs_from_proc_environ(void) {
    long fd = raw_syscall4(__NR_openat, AT_FDCWD, (long)"/proc/self/environ", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return;

    char env_buf[4096];
    long n = raw_syscall3(__NR_read, fd, (long)env_buf, sizeof(env_buf) - 1);
    raw_syscall1(__NR_close, fd);
    if (n <= 0) return;
    env_buf[n] = '\0';

    long i = 0;
    while (i < n) {
        const char *entry = &env_buf[i];
        size_t len = shim_strlen(entry);
        if (len > 7 && shim_strncmp(entry, "ROOTFS=", 7) == 0) {
            shim_strncpy(s_rootfs_buf, entry + 7, sizeof(s_rootfs_buf));
            s_rootfs = s_rootfs_buf;
            s_rootfs_len = shim_strlen(s_rootfs);
            return;
        }
        i += (long)len + 1;
    }
}

static void ensure_rootfs_loaded(void) {
    if (getenv != NULL) {
        const char *env_val = getenv("ROOTFS");
        if (env_val != NULL && env_val[0] != '\0') {
            s_rootfs = env_val;
            s_rootfs_len = shim_strlen(env_val);
            return;
        }
    }
    if (s_rootfs == NULL || s_rootfs_len == 0) {
        load_rootfs_from_proc_environ();
    }
}

// In-place patcher for ARM64 ELF binaries/libraries loaded dynamically via execve or dlopen
static void shim_patch_arm64_seccomp(const char *elf_path) {
#if defined(__aarch64__)
    if (!elf_path || elf_path[0] == '\0') return;
    long fd = raw_syscall4(__NR_openat, AT_FDCWD, (long)elf_path, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) return;

    struct stat st;
    if (raw_syscall2(__NR_fstat, fd, (long)&st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        raw_syscall1(__NR_close, fd);
        return;
    }

    long map_res = raw_syscall6(__NR_mmap, 0, (long)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_res < 0 && map_res >= -4095) {
        raw_syscall1(__NR_close, fd);
        return;
    }

    uint8_t *base = (uint8_t *)map_res;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
    if (ehdr->e_ident[0] == 0x7f && ehdr->e_ident[1] == 'E' &&
        ehdr->e_ident[2] == 'L' && ehdr->e_ident[3] == 'F' &&
        ehdr->e_ident[EI_CLASS] == ELFCLASS64 &&
        ehdr->e_ident[EI_DATA] == ELFDATA2LSB &&
        ehdr->e_machine == EM_AARCH64 &&
        ehdr->e_phoff > 0 &&
        ehdr->e_phoff + (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr) <= (size_t)st.st_size) {

        Elf64_Phdr *phdrs = (Elf64_Phdr *)(base + ehdr->e_phoff);
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
            if (phdrs[i].p_type != PT_LOAD || (phdrs[i].p_flags & PF_X) == 0) continue;
            size_t seg_offset = (size_t)phdrs[i].p_offset;
            size_t seg_size = (size_t)phdrs[i].p_filesz;
            if (seg_offset + seg_size > (size_t)st.st_size || seg_size < 8) continue;

            uint32_t *insns = (uint32_t *)(base + seg_offset);
            size_t count = seg_size / 4;
            for (size_t idx = 1; idx < count; ++idx) {
                if (insns[idx] != 0xd4000001U) continue;
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
                        break;
                    default:
                        break;
                }
            }
        }
    }

    raw_syscall2(__NR_munmap, map_res, (long)st.st_size);
    raw_syscall1(__NR_close, fd);
#else
    (void)elf_path;
#endif
}

// Seccomp SIGSYS trap handler
static void sigsys_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    if (!ucontext) return;

    int nr = info ? info->si_syscall : -1;
#if defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucontext;
    if (nr < 0) {
        nr = (int)uc->uc_mcontext.regs[8];
    }

    if (nr == 439) {
        int dirfd = (int)uc->uc_mcontext.regs[0];
        const char *pathname = (const char *)uc->uc_mcontext.regs[1];
        int mode = (int)uc->uc_mcontext.regs[2];
        char resolved[PATH_MAX];
        desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
        long res = raw_syscall4(__NR_faccessat, dirfd, (long)resolved, mode, 0);
        uc->uc_mcontext.regs[0] = (uint64_t)res;
        return;
    }

    switch (nr) {
        case 54:
        case 55:
        case 91:
        case 144:
        case 145:
        case 146:
        case 147:
        case 148:
        case 149:
        case 151:
        case 152:
        case 159:
            uc->uc_mcontext.regs[0] = 0;
            return;
        default:
            uc->uc_mcontext.regs[0] = (uint64_t)(-(long)ENOSYS);
            return;
    }
#elif defined(__x86_64__)
    ucontext_t *uc = (ucontext_t *)ucontext;
    if (nr < 0) {
        nr = (int)uc->uc_mcontext.gregs[REG_RAX];
    }
    if (nr == 439) {
        int dirfd = (int)uc->uc_mcontext.gregs[REG_RDI];
        const char *pathname = (const char *)uc->uc_mcontext.gregs[REG_RSI];
        int mode = (int)uc->uc_mcontext.gregs[REG_RDX];
        char resolved[PATH_MAX];
        desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
        long res = raw_syscall4(__NR_faccessat, dirfd, (long)resolved, mode, 0);
        uc->uc_mcontext.gregs[REG_RAX] = res;
        return;
    }
    switch (nr) {
        case 92:
        case 93:
        case 94:
        case 105:
        case 106:
        case 113:
        case 114:
        case 116:
        case 117:
        case 119:
        case 126:
        case 260:
            uc->uc_mcontext.gregs[REG_RAX] = 0;
            return;
        default:
            uc->uc_mcontext.gregs[REG_RAX] = -(long)ENOSYS;
            return;
    }
#endif
}

struct kernel_sigaction_arm64 {
    void (*k_sa_handler)(int, siginfo_t *, void *);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask;
};

static void install_sigsys_handler(void) {
    if (sigaction != NULL) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = sigsys_handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSYS, &sa, NULL);
        return;
    }
#if defined(__aarch64__)
    struct kernel_sigaction_arm64 ksa;
    memset(&ksa, 0, sizeof(ksa));
    ksa.k_sa_handler = sigsys_handler;
    ksa.sa_flags = SA_SIGINFO | SA_NODEFER;
    raw_syscall4(__NR_rt_sigaction, SIGSYS, (long)&ksa, 0, sizeof(unsigned long));
#endif
}

__attribute__((constructor))
static void desktop_shim_init(void) {
    install_sigsys_handler();
    ensure_rootfs_loaded();
}

void desktop_shim_rewrite_path(const char *in_path, char *out_path, size_t max_len) {
    if (in_path == NULL || out_path == NULL || max_len == 0) {
        return;
    }

    ensure_rootfs_loaded();

    if (s_rootfs == NULL || s_rootfs_len == 0 || in_path[0] != '/') {
        shim_strncpy(out_path, in_path, max_len);
        return;
    }

    // Do not rewrite if already pointing inside rootfs
    if (shim_strncmp(in_path, s_rootfs, s_rootfs_len) == 0 &&
        (in_path[s_rootfs_len] == '/' || in_path[s_rootfs_len] == '\0')) {
        shim_strncpy(out_path, in_path, max_len);
        return;
    }

    // Do not rewrite Android app/host paths (/data/user/, /data/data/, /data/app/, /system/, /apex/)
    if (shim_strncmp(in_path, "/data/user/", 11) == 0 ||
        shim_strncmp(in_path, "/data/data/", 11) == 0 ||
        shim_strncmp(in_path, "/data/app/", 10) == 0 ||
        shim_strncmp(in_path, "/system/", 8) == 0 ||
        shim_strncmp(in_path, "/apex/", 6) == 0) {
        shim_strncpy(out_path, in_path, max_len);
        return;
    }

    // Special handling for /dev/shm (redirect to rootfs /tmp)
    if (shim_strncmp(in_path, "/dev/shm", 8) == 0 &&
        (in_path[8] == '/' || in_path[8] == '\0')) {
        shim_concat3(out_path, max_len, s_rootfs, "/tmp", in_path + 8);
        return;
    }

    // Pass through system pseudo filesystems intact (kernel boundaries)
    if ((shim_strncmp(in_path, "/dev", 4) == 0 && (in_path[4] == '/' || in_path[4] == '\0')) ||
        (shim_strncmp(in_path, "/proc", 5) == 0 && (in_path[5] == '/' || in_path[5] == '\0')) ||
        (shim_strncmp(in_path, "/sys", 4) == 0 && (in_path[4] == '/' || in_path[4] == '\0'))) {
        shim_strncpy(out_path, in_path, max_len);
        return;
    }

    if (in_path[0] == '/' && in_path[1] == '\0') {
        shim_strncpy(out_path, s_rootfs, max_len);
        return;
    }

    shim_concat2(out_path, max_len, s_rootfs, in_path);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));

    mode_t mode = 0;
    if (__OPEN_NEEDS_MODE(flags)) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    return (int)set_errno_return(raw_syscall4(__NR_openat, dirfd, (long)resolved, flags, mode));
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));

    mode_t mode = 0;
    if (__OPEN_NEEDS_MODE(flags)) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    return (int)set_errno_return(raw_syscall4(__NR_openat, dirfd, (long)resolved, flags, mode));
}

int __openat_2(int dirfd, const char *pathname, int flags) {
    return openat(dirfd, pathname, flags, 0);
}

int __openat64_2(int dirfd, const char *pathname, int flags) {
    return openat(dirfd, pathname, flags, 0);
}

int open(const char *pathname, int flags, ...) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));

    mode_t mode = 0;
    if (__OPEN_NEEDS_MODE(flags)) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    return (int)set_errno_return(raw_syscall4(__NR_openat, AT_FDCWD, (long)resolved, flags, mode));
}

int open64(const char *pathname, int flags, ...) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));

    mode_t mode = 0;
    if (__OPEN_NEEDS_MODE(flags)) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    return (int)set_errno_return(raw_syscall4(__NR_openat, AT_FDCWD, (long)resolved, flags, mode));
}

int __open_2(const char *pathname, int flags) {
    return open(pathname, flags, 0);
}

int __open64_2(const char *pathname, int flags) {
    return open(pathname, flags, 0);
}

static int parse_fopen_mode(const char *mode) {
    if (!mode || mode[0] == '\0') return O_RDONLY;
    int base = O_RDONLY;
    int extra = 0;
    int plus = 0;
    int cloexec = 0;
    for (size_t i = 1; mode[i] != '\0'; ++i) {
        if (mode[i] == '+') plus = 1;
        if (mode[i] == 'e') cloexec = 1;
        if (mode[i] == 'x') extra |= O_EXCL;
    }
    switch (mode[0]) {
        case 'r':
            base = plus ? O_RDWR : O_RDONLY;
            break;
        case 'w':
            base = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
            break;
        case 'a':
            base = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
            break;
        default:
            base = O_RDONLY;
            break;
    }
    if (cloexec) extra |= O_CLOEXEC;
    return base | extra;
}

void *fopen(const char *pathname, const char *mode) {
    if (!fdopen) {
        set_errno_val(ENOSYS);
        return NULL;
    }
    int flags = parse_fopen_mode(mode);
    int fd = open(pathname, flags, 0666);
    if (fd < 0) return NULL;
    void *fp = fdopen(fd, mode);
    if (!fp) {
        raw_syscall1(__NR_close, fd);
    }
    return fp;
}

void *fopen64(const char *pathname, const char *mode) {
    return fopen(pathname, mode);
}

void *opendir(const char *name) {
    if (!fdopendir) {
        set_errno_val(ENOSYS);
        return NULL;
    }
    int fd = open(name, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd < 0) return NULL;
    void *dirp = fdopendir(fd);
    if (!dirp) {
        raw_syscall1(__NR_close, fd);
    }
    return dirp;
}

void *dlopen(const char *filename, int flags) {
    if (!dlmopen) return NULL;
    if (filename != NULL && filename[0] == '/') {
        char resolved[PATH_MAX];
        desktop_shim_rewrite_path(filename, resolved, sizeof(resolved));
        shim_patch_arm64_seccomp(resolved);
        return dlmopen(0, resolved, flags);
    }
    return dlmopen(0, filename, flags);
}

int newfstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall4(__NR_newfstatat, dirfd, (long)resolved, (long)statbuf, flags));
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    return newfstatat(dirfd, pathname, statbuf, flags);
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *statbuf, int flags) {
    return newfstatat(dirfd, pathname, (struct stat *)statbuf, flags);
}

int stat(const char *pathname, struct stat *statbuf) {
    return newfstatat(AT_FDCWD, pathname, statbuf, 0);
}

int stat64(const char *pathname, struct stat64 *statbuf) {
    return newfstatat(AT_FDCWD, pathname, (struct stat *)statbuf, 0);
}

int lstat(const char *pathname, struct stat *statbuf) {
    return newfstatat(AT_FDCWD, pathname, statbuf, AT_SYMLINK_NOFOLLOW);
}

int lstat64(const char *pathname, struct stat64 *statbuf) {
    return newfstatat(AT_FDCWD, pathname, (struct stat *)statbuf, AT_SYMLINK_NOFOLLOW);
}

int __fxstatat(int ver, int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    (void)ver;
    return newfstatat(dirfd, pathname, statbuf, flags);
}

int __fxstatat64(int ver, int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    (void)ver;
    return newfstatat(dirfd, pathname, statbuf, flags);
}

int __xstat(int ver, const char *pathname, struct stat *statbuf) {
    (void)ver;
    return newfstatat(AT_FDCWD, pathname, statbuf, 0);
}

int __xstat64(int ver, const char *pathname, struct stat *statbuf) {
    (void)ver;
    return newfstatat(AT_FDCWD, pathname, statbuf, 0);
}

int __lxstat(int ver, const char *pathname, struct stat *statbuf) {
    (void)ver;
    return newfstatat(AT_FDCWD, pathname, statbuf, AT_SYMLINK_NOFOLLOW);
}

int __lxstat64(int ver, const char *pathname, struct stat *statbuf) {
    (void)ver;
    return newfstatat(AT_FDCWD, pathname, statbuf, AT_SYMLINK_NOFOLLOW);
}

int statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf) {
    (void)dirfd;
    (void)pathname;
    (void)flags;
    (void)mask;
    (void)statxbuf;
    // Force glibc fallback to newfstatat which is intercepted and seccomp-allowed
    set_errno_val(ENOSYS);
    return -1;
}

int access(const char *pathname, int mode) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall4(__NR_faccessat, AT_FDCWD, (long)resolved, mode, 0));
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    (void)flags;
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall4(__NR_faccessat, dirfd, (long)resolved, mode, 0));
}

int faccessat2(int dirfd, const char *pathname, int mode, int flags) {
    return faccessat(dirfd, pathname, mode, flags);
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (ssize_t)set_errno_return(raw_syscall4(__NR_readlinkat, AT_FDCWD, (long)resolved, (long)buf, (long)bufsiz));
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (ssize_t)set_errno_return(raw_syscall4(__NR_readlinkat, dirfd, (long)resolved, (long)buf, (long)bufsiz));
}

int chdir(const char *pathname) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall1(__NR_chdir, (long)resolved));
}

int mkdir(const char *pathname, mode_t mode) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall3(__NR_mkdirat, AT_FDCWD, (long)resolved, mode));
}

int mkdirat(int dirfd, const char *pathname, mode_t mode) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall3(__NR_mkdirat, dirfd, (long)resolved, mode));
}

int unlink(const char *pathname) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall3(__NR_unlinkat, AT_FDCWD, (long)resolved, 0));
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall3(__NR_unlinkat, dirfd, (long)resolved, flags));
}

int chmod(const char *pathname, mode_t mode) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall3(__NR_fchmodat, AT_FDCWD, (long)resolved, mode));
}

int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {
    (void)flags;
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));
    return (int)set_errno_return(raw_syscall3(__NR_fchmodat, dirfd, (long)resolved, mode));
}

int chown(const char *pathname, uid_t owner, gid_t group) {
    (void)pathname;
    (void)owner;
    (void)group;
    return 0;
}

int lchown(const char *pathname, uid_t owner, gid_t group) {
    (void)pathname;
    (void)owner;
    (void)group;
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags) {
    (void)dirfd;
    (void)pathname;
    (void)owner;
    (void)group;
    (void)flags;
    return 0;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (addr && addr->sa_family == AF_UNIX && addrlen > sizeof(sa_family_t)) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        if (un->sun_path[0] == '/') {
            struct sockaddr_un rewritten;
            memset(&rewritten, 0, sizeof(rewritten));
            rewritten.sun_family = AF_UNIX;
            desktop_shim_rewrite_path(un->sun_path, rewritten.sun_path, sizeof(rewritten.sun_path));
            return (int)set_errno_return(raw_syscall3(__NR_connect, sockfd, (long)&rewritten, sizeof(rewritten)));
        }
    }
    return (int)set_errno_return(raw_syscall3(__NR_connect, sockfd, (long)addr, addrlen));
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (addr && addr->sa_family == AF_UNIX && addrlen > sizeof(sa_family_t)) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        if (un->sun_path[0] == '/') {
            struct sockaddr_un rewritten;
            memset(&rewritten, 0, sizeof(rewritten));
            rewritten.sun_family = AF_UNIX;
            desktop_shim_rewrite_path(un->sun_path, rewritten.sun_path, sizeof(rewritten.sun_path));
            return (int)set_errno_return(raw_syscall3(__NR_bind, sockfd, (long)&rewritten, sizeof(rewritten)));
        }
    }
    return (int)set_errno_return(raw_syscall3(__NR_bind, sockfd, (long)addr, addrlen));
}

// Android does not provide /dev/shm. Intercept shm_open to use memfd_create or ROOTFS/tmp
int shm_open(const char *name, int oflag, mode_t mode) {
    long fd = raw_syscall2(__NR_memfd_create, (long)name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0) {
        return (int)fd;
    }

    ensure_rootfs_loaded();
    char tmp_path[PATH_MAX];
    const char *clean_name = (name && name[0] == '/') ? name + 1 : (name ? name : "shm");
    if (s_rootfs != NULL && s_rootfs_len > 0) {
        shim_concat3(tmp_path, sizeof(tmp_path), s_rootfs, "/tmp/shm_", clean_name);
    } else {
        shim_concat2(tmp_path, sizeof(tmp_path), "/tmp/shm_", clean_name);
    }

    return (int)set_errno_return(
        raw_syscall4(__NR_openat, AT_FDCWD, (long)tmp_path, oflag | O_CREAT | O_RDWR, mode)
    );
}

int shm_unlink(const char *name) {
    ensure_rootfs_loaded();
    char tmp_path[PATH_MAX];
    const char *clean_name = (name && name[0] == '/') ? name + 1 : (name ? name : "shm");
    if (s_rootfs != NULL && s_rootfs_len > 0) {
        shim_concat3(tmp_path, sizeof(tmp_path), s_rootfs, "/tmp/shm_", clean_name);
    } else {
        shim_concat2(tmp_path, sizeof(tmp_path), "/tmp/shm_", clean_name);
    }
    return (int)set_errno_return(raw_syscall3(__NR_unlinkat, AT_FDCWD, (long)tmp_path, 0));
}

static int is_elf_binary(const char *path) {
    long fd = raw_syscall4(__NR_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    char magic[4];
    long n = raw_syscall3(__NR_read, fd, (long)magic, 4);
    raw_syscall1(__NR_close, fd);
    return (n == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F');
}

static int is_shebang_script(const char *path, char *out_interp, size_t max_interp_len) {
    long fd = raw_syscall4(__NR_openat, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    char line[256];
    long n = raw_syscall3(__NR_read, fd, (long)line, sizeof(line) - 1);
    raw_syscall1(__NR_close, fd);
    if (n > 2 && line[0] == '#' && line[1] == '!') {
        line[n] = '\0';
        for (long i = 2; i < n; ++i) {
            if (line[i] == '\n' || line[i] == '\r') {
                line[i] = '\0';
                break;
            }
        }
        char *p = line + 2;
        while (*p == ' ' || *p == '\t') p++;
        size_t pos = 0;
        while (p[pos] != '\0' && p[pos] != ' ' && p[pos] != '\t' && pos < max_interp_len - 1) {
            out_interp[pos] = p[pos];
            pos++;
        }
        out_interp[pos] = '\0';
        return (pos > 0) ? 1 : 0;
    }
    return 0;
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
    size_t count = sizeof(candidates) / sizeof(candidates[0]);
    for (size_t i = 0; i < count; ++i) {
        shim_concat2(out_ld, max_len, rootfs, candidates[i]);
        if (raw_syscall4(__NR_faccessat, AT_FDCWD, (long)out_ld, F_OK, 0) == 0) {
            return;
        }
    }
    shim_concat2(out_ld, max_len, rootfs, "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1");
}

static void build_library_path(const char *rootfs, char *out_lib_path, size_t max_len) {
    const char *subdirs[] = {
        "/usr/lib/aarch64-linux-gnu",
        "/lib/aarch64-linux-gnu",
        "/usr/lib",
        "/lib",
        "/usr/lib/aarch64-linux-gnu/libweston-13",
        "/usr/lib/aarch64-linux-gnu/libweston-12",
        "/usr/lib/aarch64-linux-gnu/libweston-10",
        "/usr/lib/aarch64-linux-gnu/weston",
        "/usr/lib/x86_64-linux-gnu",
        "/lib64"
    };
    out_lib_path[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
        if (i > 0 && pos < max_len - 1) {
            out_lib_path[pos++] = ':';
        }
        for (size_t j = 0; rootfs[j] != '\0' && pos < max_len - 1; ++j) {
            out_lib_path[pos++] = rootfs[j];
        }
        const char *sub = subdirs[i];
        for (size_t j = 0; sub[j] != '\0' && pos < max_len - 1; ++j) {
            out_lib_path[pos++] = sub[j];
        }
    }
    out_lib_path[pos] = '\0';
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    char resolved[PATH_MAX];
    desktop_shim_rewrite_path(pathname, resolved, sizeof(resolved));

    if (s_rootfs == NULL || s_rootfs_len == 0) {
        return (int)set_errno_return(raw_syscall3(__NR_execve, (long)resolved, (long)argv, (long)envp));
    }

    char ld_path[PATH_MAX];
    find_dynamic_linker(s_rootfs, ld_path, sizeof(ld_path));

    if (shim_strncmp(resolved, ld_path, shim_strlen(ld_path) + 1) == 0) {
        return (int)set_errno_return(raw_syscall3(__NR_execve, (long)resolved, (long)argv, (long)envp));
    }

    char target_binary[PATH_MAX];
    shim_strncpy(target_binary, resolved, sizeof(target_binary));

    char interp[PATH_MAX];
    int is_script = is_shebang_script(resolved, interp, sizeof(interp));
    if (is_script) {
        desktop_shim_rewrite_path(interp, target_binary, sizeof(target_binary));
    } else if (!is_elf_binary(resolved)) {
        return (int)set_errno_return(raw_syscall3(__NR_execve, (long)resolved, (long)argv, (long)envp));
    }

    shim_patch_arm64_seccomp(target_binary);

    char lib_path[PATH_MAX * 2];
    build_library_path(s_rootfs, lib_path, sizeof(lib_path));

    int argc = 0;
    if (argv) {
        while (argv[argc] != NULL && argc < 240) {
            argc++;
        }
    }

    char *new_argv[256];
    new_argv[0] = ld_path;
    new_argv[1] = "--library-path";
    new_argv[2] = lib_path;
    new_argv[3] = target_binary;

    if (is_script) {
        new_argv[4] = resolved;
        for (int i = 1; i < argc; ++i) {
            new_argv[i + 4] = argv[i];
        }
        new_argv[argc + 4] = NULL;
    } else {
        for (int i = 1; i < argc; ++i) {
            new_argv[i + 3] = argv[i];
        }
        new_argv[argc + 3] = NULL;
    }

    return (int)set_errno_return(raw_syscall3(__NR_execve, (long)ld_path, (long)new_argv, (long)envp));
}

int execv(const char *pathname, char *const argv[]) {
    char **env = environ ? environ : NULL;
    return execve(pathname, argv, env);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    if (!file || file[0] == '\0') {
        set_errno_val(ENOENT);
        return -1;
    }
    // If file contains a slash, execute directly
    for (size_t i = 0; file[i] != '\0'; ++i) {
        if (file[i] == '/') {
            return execve(file, argv, envp);
        }
    }
    const char *search_dirs[] = {
        "/usr/local/bin/",
        "/usr/bin/",
        "/bin/",
        "/usr/local/sbin/",
        "/usr/sbin/",
        "/sbin/",
        "/usr/libexec/"
    };
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    for (size_t i = 0; i < sizeof(search_dirs) / sizeof(search_dirs[0]); ++i) {
        shim_concat2(candidate, sizeof(candidate), search_dirs[i], file);
        desktop_shim_rewrite_path(candidate, resolved, sizeof(resolved));
        if (raw_syscall4(__NR_faccessat, AT_FDCWD, (long)resolved, X_OK, 0) == 0) {
            return execve(candidate, argv, envp);
        }
    }
    set_errno_val(ENOENT);
    return -1;
}

int execvp(const char *file, char *const argv[]) {
    char **env = environ ? environ : NULL;
    return execvpe(file, argv, env);
}
