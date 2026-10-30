# NativeDesktop

Arsitektur engine desktop Linux native untuk Android tanpa root, tanpa emulasi CPU, dan tanpa ketergantungan pada KVM/AVF.

## Gambaran Arsitektur

NativeDesktop membuang pendekatan lama (PRoot berbasis `ptrace`, server X11 lawas, dan VNC) dan menggantinya dengan dua subsistem inti:

1. **User-Space Execution Shim (`libdesktop_shim.so`)**:
   - Membelokkan syscall path (`openat`, `stat`, `access`) secara langsung di user-space menggunakan symbol interposition tanpa context switch `ptrace`.
   - Mengalihkan `shm_open` ke `memfd_create` untuk mengatasi pemblokiran SELinux terhadap `/dev/shm` di Android.
   - Mengintegrasikan dynamic linker redirection untuk mengeksekusi biner GNU/Linux secara langsung di core ARM64.

2. **Embedded Wayland Compositor ke Android NDK (`libdesktop_engine.so`)**:
   - Server Wayland wire protocol minimalis yang berjalan di dalam proses aplikasi Android via UNIX domain socket (`$XDG_RUNTIME_DIR/wayland-0`).
   - Menerima shared memory buffer (`wl_shm`) melalui kontrol soket `SCM_RIGHTS`.
   - Merender piksel langsung ke `ANativeWindow` Android pada 60 FPS stabil.

3. **Android Application Layer (Jetpack Compose)**:
   - Menggunakan arsitektur modular: `ui/screen`, `ui/components`, `ui/theme`, `viewmodel`, dan `data/repository`.
   - Antarmuka bertema gelap (*Dark Layered*) dengan token desain terpusat dan zero hardcoding.

## Struktur Direktori

```
├── .github/workflows/
│   └── android-build.yml      # CI/CD otomatis untuk build APK & test native
├── app/
│   ├── src/main/cpp/
│   │   ├── shim/              # Dynamic linker hook & path virtualizer
│   │   ├── compositor/        # Clean-room Wayland server & wire protocol
│   │   └── jni/               # Bridge NDK ke Android SurfaceView
│   └── src/main/java/com/core/desktop/
│       ├── data/              # Model & Repository
│       ├── viewmodel/         # State holder & lifecycle
│       └── ui/                # Jetpack Compose UI
└── native_tests/              # Verifikasi mandiri native C engine
```

## Build di GitHub Actions

Repositori ini telah dilengkapi dengan `.github/workflows/android-build.yml` yang secara otomatis menguji unit test native C engine dan mengompilasi APK Android debug menggunakan Android SDK, NDK, dan CMake.

