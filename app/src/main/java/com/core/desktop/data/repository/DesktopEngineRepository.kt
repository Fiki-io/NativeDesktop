package com.core.desktop.data.repository

import android.content.Context
import android.net.Uri
import android.os.Build
import android.view.Surface
import com.core.desktop.R
import com.core.desktop.data.model.DesktopSessionConfig
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

        // Fallback: extract libdesktop_shim.so directly from the APK archive if not unpacked
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

        val requestedExec = if (config.executablePath == "/bin/sh" && config.sessionArgs.isEmpty()) {
            val westonCandidate = File(rootfsDir, "usr/bin/weston")
            if (westonCandidate.exists()) "/usr/bin/weston" else config.executablePath
        } else {
            config.executablePath
        }

        val targetExec = if (requestedExec.startsWith("/")) {
            File(rootfsDir, requestedExec.substring(1))
        } else {
            File(rootfsDir, requestedExec)
        }

        if (!targetExec.exists()) {
            return@withContext Result.failure(
                IllegalArgumentException(context.getString(R.string.error_executable_not_found))
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

        val waylandInitOk = nativeInit(socketDir.absolutePath, "wayland-0")
        if (!waylandInitOk) {
            return@withContext Result.failure(
                IllegalStateException(context.getString(R.string.error_wayland_init_failed))
            )
        }

        if (surface != null) {
            nativeSetSurface(surface)
        }

        val envList = config.envVars.map { "${it.key}=${it.value}" }.toTypedArray()
        val argsArray = config.sessionArgs.toTypedArray()

        val processStarted = nativeStartSession(
            rootfsPath = rootfsDir.absolutePath,
            shimPath = shimPath,
            socketDir = socketDir.absolutePath,
            execPath = targetExec.absolutePath,
            args = argsArray,
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
        val rootfsDir = File(context.filesDir, "rootfs")
        val westonBin = File(rootfsDir, "usr/bin/weston")
        return if (westonBin.exists()) "/usr/bin/weston" else "/bin/sh"
    }

    fun isRootfsInstalled(): Boolean {
        val rootfsDir = File(context.filesDir, "rootfs")
        return rootfsDir.exists() && rootfsDir.isDirectory && (File(rootfsDir, "bin").exists() || File(rootfsDir, "usr").exists())
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

            // Post-extraction: ensure permissions on all binaries and dirs
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

    private external fun nativeInit(socketDir: String, socketName: String): Boolean
    private external fun nativeSetSurface(surface: Surface?)
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
