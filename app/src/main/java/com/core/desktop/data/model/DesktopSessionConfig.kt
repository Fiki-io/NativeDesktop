package com.core.desktop.data.model

data class DesktopSessionConfig(
    val rootfsPath: String,
    val executablePath: String,
    val sessionArgs: List<String> = emptyList(),
    val envVars: Map<String, String> = emptyMap()
)

