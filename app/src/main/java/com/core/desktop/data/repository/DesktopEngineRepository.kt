package com.core.desktop.data.repository

import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.view.Surface
import com.core.desktop.R
import com.core.desktop.data.model.DesktopSessionConfig
import com.core.desktop.data.model.GpuBackend
import com.core.desktop.data.model.PerfMetrics
import com.core.desktop.data.model.SoCHardwareInfo
import com.core.desktop.util.TarExtractor
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.PushbackInputStream
import java.util.zip.GZIPInputStream
import java.util.zip.ZipFile

class DesktopEngineRepository(private val context: Context) {

    init {
        System.loadLibrary("desktop_engine")
    }

    fun detectHardwareInfo(): SoCHardwareInfo {
        val socMan = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            Build.SOC_MANUFACTURER.takeIf { it.isNotBlank() && it != Build.UNKNOWN } ?: ""
        } else {
            ""
        }
        val socMod = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            Build.SOC_MODEL.takeIf { it.isNotBlank() && it != Build.UNKNOWN } ?: ""
        } else {
            ""
        }
        val hw = Build.HARDWARE ?: ""
        val board = Build.BOARD ?: ""
        val socName = listOf(socMan, socMod).filter { it.isNotBlank() }.joinToString(" ")
            .ifBlank { "$hw ($board)" }

        val hasKgsl = File("/dev/kgsl-3d0").exists()
        val hasMali = File("/dev/mali0").exists()
        val hasDrm = File("/dev/dri/renderD128").exists()

        val (gpuNode, recommendedBackend) = when {
            hasMali -> "Mali / Immortalis (/dev/mali0)" to GpuBackend.MALI_UNIVERSAL
            hasKgsl -> "Adreno KGSL (/dev/kgsl-3d0)" to GpuBackend.ADRENO_TURNIP
            hasDrm -> "DRM RenderNode (/dev/dri/renderD128)" to GpuBackend.MALI_UNIVERSAL
            else -> "Vulkan WSI Universal" to GpuBackend.AUTO
        }

        val freqsKhz = mutableListOf<Long>()
        for (i in 0 until 16) {
            val freqFile = File("/sys/devices/system/cpu/cpu$i/cpufreq/cpuinfo_max_freq")
            if (freqFile.exists()) {
                val khz = freqFile.readText().trim().toLongOrNull()
                if (khz != null && khz > 0L) {
                    freqsKhz.add(khz)
                }
            }
        }

        val cpuTopology = if (freqsKhz.isNotEmpty()) {
            val minKhz = freqsKhz.minOrNull() ?: 0L
            val maxKhz = freqsKhz.maxOrNull() ?: 0L
            val perfCores = freqsKhz.count { it > minKhz }.let { if (it == 0) freqsKhz.size else it }
            val effCores = freqsKhz.size - perfCores
            val maxMhz = maxKhz / 1000L
            if (effCores > 0) {
                "${freqsKhz.size} Cores ($perfCores Perf + $effCores Eff, Max ${maxMhz} MHz)"
            } else {
                "${freqsKhz.size} Cores (Max ${maxMhz} MHz)"
            }
        } else {
            "${Runtime.getRuntime().availableProcessors()} Cores ARM64"
        }

        return SoCHardwareInfo(
            socName = socName,
            gpuDriverNode = gpuNode,
            cpuTopology = cpuTopology,
            recommendedBackend = recommendedBackend
        )
    }

    private fun prepareShimLibrary(): File {
        val dstShim = File(context.filesDir, "libdesktop_shim.so")
        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        val installedShim = File(nativeLibDir, "libdesktop_shim.so")

        if (installedShim.exists() && installedShim.length() > 0L) {
            try {
                installedShim.inputStream().use { input ->
                    FileOutputStream(dstShim).use { output ->
                        input.copyTo(output)
                    }
                }
                dstShim.setReadable(true, false)
                dstShim.setExecutable(true, false)
                return dstShim
            } catch (_: Exception) {
                installedShim.setReadable(true, false)
                installedShim.setExecutable(true, false)
                return installedShim
            }
        }

        val apkPath = context.applicationInfo.sourceDir
        if (!apkPath.isNullOrEmpty()) {
            try {
                ZipFile(apkPath).use { zip ->
                    val supportedAbis = Build.SUPPORTED_ABIS ?: arrayOf("arm64-v8a", "x86_64")
                    for (abi in supportedAbis) {
                        val entry = zip.getEntry("lib/$abi/libdesktop_shim.so")
                        if (entry != null) {
                            zip.getInputStream(entry).use { input ->
                                FileOutputStream(dstShim).use { output ->
                                    input.copyTo(output)
                                }
                            }
                            dstShim.setReadable(true, false)
                            dstShim.setExecutable(true, false)
                            break
                        }
                    }
                }
            } catch (_: Exception) {
            }
        }

        dstShim.setReadable(true, false)
        dstShim.setExecutable(true, false)
        return dstShim
    }

    suspend fun importGameFileFromUri(uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        try {
            val rootfsDir = File(context.filesDir, "rootfs")
            val gamesDir = File(rootfsDir, "games")
            if (!gamesDir.exists()) {
                gamesDir.mkdirs()
            }

            var displayName = "game_exec.exe"
            context.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (nameIndex >= 0 && cursor.moveToFirst()) {
                    val name = cursor.getString(nameIndex)
                    if (!name.isNullOrBlank()) {
                        displayName = name.replace(Regex("[^a-zA-Z0-9._-]"), "_")
                    }
                }
            }

            val dstFile = File(gamesDir, displayName)
            val input = context.contentResolver.openInputStream(uri)
                ?: return@withContext Result.failure(
                    java.io.IOException(context.getString(R.string.error_game_file_open_failed))
                )

            input.use { inStream ->
                FileOutputStream(dstFile).use { outStream ->
                    inStream.copyTo(outStream)
                }
            }

            if (!dstFile.exists() || dstFile.length() <= 0L) {
                return@withContext Result.failure(
                    java.io.IOException(context.getString(R.string.error_game_file_empty))
                )
            }

            dstFile.setReadable(true, false)
            dstFile.setExecutable(true, false)

            Result.success("/games/${dstFile.name}")
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun startSession(
        config: DesktopSessionConfig,
        surface: Surface?
    ): Result<Unit> = withContext(Dispatchers.IO) {
        val rootfsDir = File(config.rootfsPath)
        if (!rootfsDir.exists() || !rootfsDir.isDirectory) {
            return@withContext Result.failure(
                IllegalArgumentException(context.getString(R.string.error_rootfs_not_found))
            )
        }

        val shimFile = prepareShimLibrary()
        val shimPath = shimFile.absolutePath

        val socketDir = File(context.filesDir, "tmp")
        if (!socketDir.exists()) {
            socketDir.mkdirs()
        }
        socketDir.setReadable(true, true)
        socketDir.setWritable(true, true)
        socketDir.setExecutable(true, true)

        nativeSetResolution(config.renderResolution.width, config.renderResolution.height)

        val waylandInitOk = nativeInit(socketDir.absolutePath, "wayland-0")
        if (!waylandInitOk) {
            return@withContext Result.failure(
                IllegalStateException(context.getString(R.string.error_wayland_init_failed))
            )
        }

        if (surface != null) {
            nativeSetSurface(surface)
        }

        val runnerFile = File(rootfsDir, "usr/local/bin/game-runner")
        val bashFile = File(rootfsDir, "bin/bash")
        val shFile = File(rootfsDir, "bin/sh")
        if (!bashFile.exists() && !shFile.exists()) {
            nativeStopSession()
            return@withContext Result.failure(
                IllegalArgumentException(context.getString(R.string.error_executable_not_found))
            )
        }

        val envMap = config.buildEngineEnvironment()
        val envList = envMap.map { "${it.key}=${it.value}" }.toTypedArray()

        val fullArgs = mutableListOf<String>()
        if (config.gameTarget.isNotBlank()) {
            fullArgs.add(config.gameTarget)
        }
        fullArgs.addAll(config.sessionArgs)

        val processStarted = nativeStartSession(
            rootfsPath = rootfsDir.absolutePath,
            shimPath = shimPath,
            socketDir = socketDir.absolutePath,
            execPath = runnerFile.absolutePath,
            args = fullArgs.toTypedArray(),
            envVars = envList
        )

        if (!processStarted) {
            nativeStopSession()
            return@withContext Result.failure(
                IllegalStateException(context.getString(R.string.error_session_start_failed))
            )
        }

        Result.success(Unit)
    }

    fun updateSurface(surface: Surface?) {
        nativeSetSurface(surface)
    }

    fun injectPointer(action: Int, normX: Float, normY: Float) {
        nativeInjectPointer(action, normX, normY)
    }

    fun injectKey(linuxKeycode: Int, pressed: Boolean, shiftMod: Boolean) {
        nativeInjectKey(linuxKeycode, pressed, shiftMod)
    }

    fun getPerfMetrics(): PerfMetrics {
        val raw = nativeGetPerfMetrics()
        if (raw.size >= 4) {
            return PerfMetrics(
                fps = raw[0],
                frameTimeMs = raw[1],
                renderWidth = raw[2].toInt(),
                renderHeight = raw[3].toInt()
            )
        }
        return PerfMetrics()
    }

    var processTerminatedListener: ((exitCode: Int, message: String) -> Unit)? = null

    fun onNativeProcessTerminated(exitCode: Int, message: String) {
        processTerminatedListener?.invoke(exitCode, message)
    }

    fun getLogs(): List<String> {
        return nativeGetLogs().toList()
    }

    suspend fun stopSession(): Unit = withContext(Dispatchers.IO) {
        nativeStopSession()
    }

    fun getDefaultRootfsPath(): String {
        return File(context.filesDir, "rootfs").absolutePath
    }

    fun getDefaultExecutablePath(): String {
        return "/usr/local/bin/game-runner"
    }

    fun isRootfsInstalled(): Boolean {
        val rootfsDir = File(context.filesDir, "rootfs")
        return rootfsDir.exists() && rootfsDir.isDirectory &&
            (File(rootfsDir, "bin").exists() || File(rootfsDir, "usr").exists())
    }

    suspend fun extractRootfsFromUri(
        uri: Uri,
        onProgress: (Long) -> Unit
    ): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val rootfsDir = File(context.filesDir, "rootfs")
            if (rootfsDir.exists()) {
                rootfsDir.deleteRecursively()
            }
            rootfsDir.mkdirs()

            val inputStream = context.contentResolver.openInputStream(uri)
                ?: return@withContext Result.failure(
                    java.io.IOException("Gagal membuka berkas arsip yang dipilih.")
                )

            inputStream.use { rawIn ->
                val pushback = PushbackInputStream(rawIn, 2)
                val magic = ByteArray(2)
                val read = pushback.read(magic)
                if (read > 0) {
                    pushback.unread(magic, 0, read)
                }

                val isGzip = (read == 2 && magic[0] == 0x1F.toByte() && magic[1] == 0x8B.toByte())
                val stream: InputStream = if (isGzip) {
                    GZIPInputStream(pushback)
                } else {
                    pushback
                }

                TarExtractor.extract(stream, rootfsDir, onProgress)
            }

            rootfsDir.walkTopDown().forEach { f ->
                if (f.isDirectory) {
                    f.setReadable(true, false)
                    f.setExecutable(true, false)
                } else if (f.name.endsWith(".so") || f.name.contains(".so.") ||
                    f.parentFile?.name == "bin" || f.parentFile?.name == "sbin" ||
                    f.parentFile?.name == "libexec") {
                    f.setReadable(true, false)
                    f.setExecutable(true, false)
                }
            }

            Result.success(Unit)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    private external fun nativeSetResolution(width: Int, height: Int)
    private external fun nativeGetPerfMetrics(): FloatArray
    private external fun nativeInit(socketDir: String, socketName: String): Boolean
    private external fun nativeSetSurface(surface: Surface?)
    private external fun nativeInjectPointer(action: Int, normX: Float, normY: Float)
    private external fun nativeInjectKey(linuxKeycode: Int, pressed: Boolean, shiftMod: Boolean)
    private external fun nativeStartSession(
        rootfsPath: String,
        shimPath: String,
        socketDir: String,
        execPath: String,
        args: Array<String>,
        envVars: Array<String>
    ): Boolean
    private external fun nativeGetLogs(): Array<String>
    private external fun nativeStopSession()
}
