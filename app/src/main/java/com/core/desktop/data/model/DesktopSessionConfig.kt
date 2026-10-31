package com.core.desktop.data.model

enum class GpuBackend(val id: String) {
    AUTO("auto"),
    MALI_UNIVERSAL("mali_universal"),
    ADRENO_TURNIP("adreno_turnip"),
    SOFTWARE_LVP("software_lvp")
}

enum class DxWrapper(val id: String) {
    DXVK_ASYNC("dxvk"),
    WINED3D("wined3d")
}

enum class DynarecPreset(val id: String) {
    PERFORMANCE("performance"),
    BALANCED("balanced"),
    STABILITY("stability")
}

enum class RenderResolution(val width: Int, val height: Int, val label: String) {
    RES_960x540(960, 540, "960x540 (16:9)"),
    RES_1280x720(1280, 720, "1280x720 (HD)"),
    RES_1600x900(1600, 900, "1600x900 (HD+)"),
    RES_1920x1080(1920, 1080, "1920x1080 (FHD)")
}

enum class DisplayMode(val id: String) {
    WAYLAND_DIRECT("wayland_direct"),
    KIOSK_XWAYLAND("kiosk_xwayland")
}

data class SoCHardwareInfo(
    val socName: String,
    val gpuDriverNode: String,
    val cpuTopology: String,
    val recommendedBackend: GpuBackend
)

data class PerfMetrics(
    val fps: Float = 0f,
    val frameTimeMs: Float = 0f,
    val renderWidth: Int = 1280,
    val renderHeight: Int = 720
)

data class DesktopSessionConfig(
    val rootfsPath: String,
    val executablePath: String,
    val gameTarget: String = "winecfg",
    val sessionArgs: List<String> = emptyList(),
    val gpuBackend: GpuBackend = GpuBackend.AUTO,
    val dxWrapper: DxWrapper = DxWrapper.DXVK_ASYNC,
    val dynarecPreset: DynarecPreset = DynarecPreset.PERFORMANCE,
    val renderResolution: RenderResolution = RenderResolution.RES_1280x720,
    val displayMode: DisplayMode = DisplayMode.WAYLAND_DIRECT,
    val enableEsync: Boolean = true,
    val lockPerformanceCores: Boolean = true
) {
    fun buildEngineEnvironment(): Map<String, String> {
        val env = mutableMapOf<String, String>()
        env["ND_GPU_BACKEND"] = gpuBackend.id
        env["ND_DX_WRAPPER"] = dxWrapper.id
        env["ND_DYNAREC_PRESET"] = dynarecPreset.id
        env["ND_RESOLUTION"] = "${renderResolution.width}x${renderResolution.height}"
        env["ND_DISPLAY_MODE"] = displayMode.id
        env["ND_PERF_CORES"] = if (lockPerformanceCores) "1" else "0"

        // Box64 Dynarec real tuning variables
        env["BOX64_DYNAREC"] = "1"
        env["BOX64_MMAP32"] = "1"
        env["BOX64_AVX"] = "1"
        when (dynarecPreset) {
            DynarecPreset.PERFORMANCE -> {
                env["BOX64_DYNAREC_BIGBLOCK"] = "3"
                env["BOX64_DYNAREC_STRONGMEM"] = "0"
                env["BOX64_DYNAREC_SAFEFLAGS"] = "0"
                env["BOX64_DYNAREC_FASTNAN"] = "1"
                env["BOX64_DYNAREC_FASTROUND"] = "1"
                env["BOX64_DYNAREC_X87DOUBLE"] = "0"
                env["BOX64_DYNAREC_CALLRET"] = "1"
            }
            DynarecPreset.BALANCED -> {
                env["BOX64_DYNAREC_BIGBLOCK"] = "2"
                env["BOX64_DYNAREC_STRONGMEM"] = "1"
                env["BOX64_DYNAREC_SAFEFLAGS"] = "1"
                env["BOX64_DYNAREC_FASTNAN"] = "1"
                env["BOX64_DYNAREC_FASTROUND"] = "1"
                env["BOX64_DYNAREC_X87DOUBLE"] = "0"
                env["BOX64_DYNAREC_CALLRET"] = "1"
            }
            DynarecPreset.STABILITY -> {
                env["BOX64_DYNAREC_BIGBLOCK"] = "1"
                env["BOX64_DYNAREC_STRONGMEM"] = "2"
                env["BOX64_DYNAREC_SAFEFLAGS"] = "2"
                env["BOX64_DYNAREC_FASTNAN"] = "0"
                env["BOX64_DYNAREC_FASTROUND"] = "0"
                env["BOX64_DYNAREC_X87DOUBLE"] = "1"
                env["BOX64_DYNAREC_CALLRET"] = "0"
            }
        }

        // Wine Esync / Fsync & DXVK Async tuning
        if (enableEsync) {
            env["WINEESYNC"] = "1"
            env["WINEFSYNC"] = "1"
        } else {
            env["WINEESYNC"] = "0"
            env["WINEFSYNC"] = "0"
        }
        env["DXVK_ASYNC"] = "1"
        env["WINE_LARGE_ADDRESS_AWARE"] = "1"

        return env
    }
}
