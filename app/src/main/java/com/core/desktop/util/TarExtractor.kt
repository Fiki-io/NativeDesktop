package com.core.desktop.util

import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Paths
import java.nio.file.StandardCopyOption

object TarExtractor {

    private const val BLOCK_SIZE = 512

    fun extract(
        inputStream: InputStream,
        targetDir: File,
        onProgress: ((bytesExtracted: Long) -> Unit)? = null
    ) {
        if (!targetDir.exists()) {
            targetDir.mkdirs()
        }

        val canonicalTarget = targetDir.canonicalPath
        val header = ByteArray(BLOCK_SIZE)
        var totalBytesRead = 0L

        var pendingLongName: String? = null
        var pendingLongLink: String? = null

        while (true) {
            val bytesRead = readFully(inputStream, header, 0, BLOCK_SIZE)
            if (bytesRead < BLOCK_SIZE) {
                break
            }

            if (isZeroBlock(header)) {
                break
            }

            val fileSize = parseOctal(header, 124, 11)
            val typeFlag = header[156].toInt().toChar()
            val mode = parseOctal(header, 100, 7).toInt()

            // Handle GNU LongName ('L') and LongLink ('K')
            if (typeFlag == 'L' || typeFlag == 'K') {
                val payload = readPayloadBytes(inputStream, fileSize)
                totalBytesRead += fileSize
                onProgress?.invoke(totalBytesRead)
                val str = readNullTerminatedString(payload, 0, payload.size)
                if (typeFlag == 'L') {
                    pendingLongName = str
                } else {
                    pendingLongLink = str
                }
                continue
            }

            // Handle POSIX PAX Extended Header ('x' or 'g')
            if (typeFlag == 'x' || typeFlag == 'g') {
                val payload = readPayloadBytes(inputStream, fileSize)
                totalBytesRead += fileSize
                onProgress?.invoke(totalBytesRead)
                val paxString = String(payload, Charsets.UTF_8)
                parsePaxRecords(paxString) { key, value ->
                    if (key == "path") pendingLongName = value
                    if (key == "linkpath") pendingLongLink = value
                }
                continue
            }

            val rawFilename = pendingLongName ?: parseFilename(header)
            val rawLinkTarget = pendingLongLink ?: parseLinkTarget(header)
            pendingLongName = null
            pendingLongLink = null

            val filename = cleanTarPath(rawFilename)
            if (filename.isEmpty()) {
                skipBytes(inputStream, fileSize)
                continue
            }

            val destFile = File(targetDir, filename)
            val normalizedDest = destFile.toPath().normalize().toFile()
            if (!normalizedDest.path.startsWith(targetDir.toPath().normalize().toFile().path)) {
                skipBytes(inputStream, fileSize)
                continue
            }

            when (typeFlag) {
                '5' -> {
                    // Directory
                    if (!Files.isSymbolicLink(normalizedDest.toPath())) {
                        normalizedDest.mkdirs()
                        normalizedDest.setReadable(true, false)
                        normalizedDest.setExecutable(true, false)
                    }
                    skipBytes(inputStream, fileSize)
                }
                '2' -> {
                    // Symbolic link
                    normalizedDest.parentFile?.mkdirs()
                    try {
                        Files.deleteIfExists(normalizedDest.toPath())
                        val targetPath = if (rawLinkTarget.startsWith("/")) {
                            val cleanTarget = cleanTarPath(rawLinkTarget)
                            val absInRootfs = File(targetDir, cleanTarget).toPath().normalize()
                            val parentDir = (normalizedDest.parentFile ?: targetDir).toPath().normalize()
                            parentDir.relativize(absInRootfs)
                        } else {
                            Paths.get(rawLinkTarget)
                        }
                        Files.createSymbolicLink(normalizedDest.toPath(), targetPath)
                    } catch (_: Exception) {
                        // Ignore if OS restricts symlink creation
                    }
                    skipBytes(inputStream, fileSize)
                }
                '1' -> {
                    // Hard link
                    normalizedDest.parentFile?.mkdirs()
                    val cleanTarget = cleanTarPath(rawLinkTarget)
                    val linkSource = File(targetDir, cleanTarget).toPath().normalize()
                    try {
                        Files.deleteIfExists(normalizedDest.toPath())
                        if (Files.exists(linkSource, LinkOption.NOFOLLOW_LINKS)) {
                            try {
                                Files.createLink(normalizedDest.toPath(), linkSource)
                            } catch (_: Exception) {
                                Files.copy(
                                    linkSource,
                                    normalizedDest.toPath(),
                                    StandardCopyOption.REPLACE_EXISTING,
                                    LinkOption.NOFOLLOW_LINKS
                                )
                            }
                        } else {
                            val parentDir = (normalizedDest.parentFile ?: targetDir).toPath().normalize()
                            Files.createSymbolicLink(normalizedDest.toPath(), parentDir.relativize(linkSource))
                        }
                    } catch (_: Exception) {
                    }
                    skipBytes(inputStream, fileSize)
                }
                '0', '\u0000' -> {
                    // Normal file
                    normalizedDest.parentFile?.mkdirs()
                    try {
                        Files.deleteIfExists(normalizedDest.toPath())
                    } catch (_: Exception) {
                    }

                    FileOutputStream(normalizedDest).use { out ->
                        var remaining = fileSize
                        val buffer = ByteArray(8192)
                        while (remaining > 0) {
                            val toRead = minOf(remaining, buffer.size.toLong()).toInt()
                            val n = inputStream.read(buffer, 0, toRead)
                            if (n == -1) break
                            out.write(buffer, 0, n)
                            remaining -= n
                            totalBytesRead += n
                            onProgress?.invoke(totalBytesRead)
                        }
                    }

                    normalizedDest.setReadable(true, false)
                    normalizedDest.setWritable(true, false)

                    val isBinary = (mode and 0b001_001_001) != 0 ||
                            filename.startsWith("bin/") ||
                            filename.startsWith("usr/bin/") ||
                            filename.startsWith("sbin/") ||
                            filename.startsWith("usr/sbin/") ||
                            filename.startsWith("usr/libexec/") ||
                            filename.contains(".so")
                    if (isBinary) {
                        normalizedDest.setExecutable(true, false)
                    }

                    val padding = (BLOCK_SIZE - (fileSize % BLOCK_SIZE)) % BLOCK_SIZE
                    if (padding > 0) {
                        skipFully(inputStream, padding)
                    }
                }
                else -> {
                    // Other special entries (char/block device, fifo, etc. - skip payload safely)
                    skipBytes(inputStream, fileSize)
                }
            }
        }

        // Ensure canonical root and /tmp exist
        val tmpDir = File(canonicalTarget, "tmp")
        if (!tmpDir.exists()) {
            tmpDir.mkdirs()
        }
        tmpDir.setReadable(true, false)
        tmpDir.setWritable(true, false)
        tmpDir.setExecutable(true, false)
    }

    private fun cleanTarPath(raw: String): String {
        var s = raw.trim()
        while (s.startsWith("./")) {
            s = s.substring(2)
        }
        while (s.startsWith("/")) {
            s = s.substring(1)
        }
        if (s == ".") return ""
        return s
    }

    private fun readPayloadBytes(inputStream: InputStream, fileSize: Long): ByteArray {
        val size = fileSize.toInt().coerceAtLeast(0)
        val data = ByteArray(size)
        readFully(inputStream, data, 0, size)
        val padding = (BLOCK_SIZE - (fileSize % BLOCK_SIZE)) % BLOCK_SIZE
        if (padding > 0) {
            skipFully(inputStream, padding)
        }
        return data
    }

    private fun parsePaxRecords(pax: String, onRecord: (key: String, value: String) -> Unit) {
        var idx = 0
        val len = pax.length
        while (idx < len) {
            val spaceIdx = pax.indexOf(' ', idx)
            if (spaceIdx <= idx) break
            val recLen = pax.substring(idx, spaceIdx).toIntOrNull() ?: break
            if (recLen <= 0 || idx + recLen > len) break
            val record = pax.substring(spaceIdx + 1, idx + recLen).trimEnd('\n')
            val eqIdx = record.indexOf('=')
            if (eqIdx > 0) {
                val k = record.substring(0, eqIdx)
                val v = record.substring(eqIdx + 1)
                onRecord(k, v)
            }
            idx += recLen
        }
    }

    private fun readFully(stream: InputStream, b: ByteArray, off: Int, len: Int): Int {
        var count = 0
        while (count < len) {
            val r = stream.read(b, off + count, len - count)
            if (r < 0) break
            count += r
        }
        return count
    }

    private fun skipFully(stream: InputStream, bytes: Long) {
        var remaining = bytes
        while (remaining > 0) {
            val skipped = stream.skip(remaining)
            if (skipped <= 0) {
                if (stream.read() == -1) break
                remaining--
            } else {
                remaining -= skipped
            }
        }
    }

    private fun skipBytes(stream: InputStream, fileSize: Long) {
        if (fileSize <= 0) return
        val totalToSkip = fileSize + ((BLOCK_SIZE - (fileSize % BLOCK_SIZE)) % BLOCK_SIZE)
        skipFully(stream, totalToSkip)
    }

    private fun isZeroBlock(header: ByteArray): Boolean {
        for (b in header) {
            if (b.toInt() != 0) return false
        }
        return true
    }

    private fun parseFilename(header: ByteArray): String {
        val name = readNullTerminatedString(header, 0, 100)
        val prefix = readNullTerminatedString(header, 345, 155)
        return if (prefix.isNotEmpty()) "$prefix/$name" else name
    }

    private fun parseLinkTarget(header: ByteArray): String {
        return readNullTerminatedString(header, 157, 100)
    }

    private fun readNullTerminatedString(b: ByteArray, offset: Int, length: Int): String {
        var len = 0
        while (len < length && b[offset + len].toInt() != 0) {
            len++
        }
        return String(b, offset, len, Charsets.UTF_8).trim()
    }

    private fun parseOctal(b: ByteArray, offset: Int, length: Int): Long {
        // Support GNU base-256 binary size extension (leading byte 0x80)
        if (length > 0 && (b[offset].toInt() and 0x80) != 0) {
            var value = 0L
            for (i in 1 until length) {
                value = (value shl 8) or (b[offset + i].toLong() and 0xFFL)
            }
            return value
        }
        var result = 0L
        for (i in offset until offset + length) {
            val c = b[i].toInt().toChar()
            if (c in '0'..'7') {
                result = (result shl 3) + (c - '0')
            } else if (c == ' ' || c == '\u0000') {
                if (result > 0) break
            }
        }
        return result
    }
}
