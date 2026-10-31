package com.core.desktop.viewmodel

import android.app.Application
import android.net.Uri
import android.view.Surface
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.core.desktop.R
import com.core.desktop.data.model.DesktopEngineState
import com.core.desktop.data.model.DesktopSessionConfig
import com.core.desktop.data.model.DisplayMode
import com.core.desktop.data.model.DxWrapper
import com.core.desktop.data.model.DynarecPreset
import com.core.desktop.data.model.GpuBackend
import com.core.desktop.data.model.PerfMetrics
import com.core.desktop.data.model.RenderResolution
import com.core.desktop.data.model.SoCHardwareInfo
import com.core.desktop.data.repository.DesktopEngineRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class DesktopViewModel(application: Application) : AndroidViewModel(application) {

    private val repository = DesktopEngineRepository(application.applicationContext)

    private val _hardwareInfo = MutableStateFlow(repository.detectHardwareInfo())
    val hardwareInfo: StateFlow<SoCHardwareInfo> = _hardwareInfo.asStateFlow()

    private val _engineState = MutableStateFlow<DesktopEngineState>(DesktopEngineState.Idle)
    val engineState: StateFlow<DesktopEngineState> = _engineState.asStateFlow()

    private val _config = MutableStateFlow(
        DesktopSessionConfig(
            rootfsPath = repository.getDefaultRootfsPath(),
            executablePath = repository.getDefaultExecutablePath(),
            gpuBackend = _hardwareInfo.value.recommendedBackend
        )
    )
    val config: StateFlow<DesktopSessionConfig> = _config.asStateFlow()

    private val _perfMetrics = MutableStateFlow(PerfMetrics())
    val perfMetrics: StateFlow<PerfMetrics> = _perfMetrics.asStateFlow()

    private val _lastErrorMessage = MutableStateFlow<String?>(null)
    val lastErrorMessage: StateFlow<String?> = _lastErrorMessage.asStateFlow()

    private val _isRootfsInstalled = MutableStateFlow(repository.isRootfsInstalled())
    val isRootfsInstalled: StateFlow<Boolean> = _isRootfsInstalled.asStateFlow()

    private val _extractedBytes = MutableStateFlow(0L)
    val extractedBytes: StateFlow<Long> = _extractedBytes.asStateFlow()

    private val _logs = MutableStateFlow<List<String>>(emptyList())
    val logs: StateFlow<List<String>> = _logs.asStateFlow()

    private var currentSurface: Surface? = null

    init {
        repository.processTerminatedListener = { exitCode, message ->
            viewModelScope.launch {
                _lastErrorMessage.value = "$message (Kode: $exitCode)"
                _engineState.value = DesktopEngineState.Error(R.string.status_error)
                refreshLogs()
            }
        }
    }

    fun refreshLogs() {
        _logs.value = repository.getLogs()
        _perfMetrics.value = repository.getPerfMetrics()
    }

    fun updateRootfsPath(path: String) {
        _config.value = _config.value.copy(rootfsPath = path)
    }

    fun updateGameTarget(target: String) {
        _config.value = _config.value.copy(gameTarget = target)
    }

    fun updateSessionArgs(argsString: String) {
        val parsedArgs = argsString.trim().split("\\s+".toRegex()).filter { it.isNotEmpty() }
        _config.value = _config.value.copy(sessionArgs = parsedArgs)
    }

    fun updateGpuBackend(backend: GpuBackend) {
        _config.value = _config.value.copy(gpuBackend = backend)
    }

    fun updateDxWrapper(wrapper: DxWrapper) {
        _config.value = _config.value.copy(dxWrapper = wrapper)
    }

    fun updateDynarecPreset(preset: DynarecPreset) {
        _config.value = _config.value.copy(dynarecPreset = preset)
    }

    fun updateRenderResolution(resolution: RenderResolution) {
        _config.value = _config.value.copy(renderResolution = resolution)
    }

    fun updateDisplayMode(mode: DisplayMode) {
        _config.value = _config.value.copy(displayMode = mode)
    }

    fun updateEnableEsync(enabled: Boolean) {
        _config.value = _config.value.copy(enableEsync = enabled)
    }

    fun updateLockPerformanceCores(enabled: Boolean) {
        _config.value = _config.value.copy(lockPerformanceCores = enabled)
    }

    fun resetToDefaults() {
        _config.value = DesktopSessionConfig(
            rootfsPath = repository.getDefaultRootfsPath(),
            executablePath = repository.getDefaultExecutablePath(),
            gpuBackend = _hardwareInfo.value.recommendedBackend
        )
        _lastErrorMessage.value = null
    }

    fun importGameFile(uri: Uri) {
        viewModelScope.launch {
            val res = repository.importGameFileFromUri(uri)
            res.onSuccess { importedPath ->
                _config.value = _config.value.copy(gameTarget = importedPath)
                _lastErrorMessage.value = null
            }.onFailure { err ->
                _lastErrorMessage.value = err.message
                _engineState.value = DesktopEngineState.Error(R.string.status_error)
            }
        }
    }

    fun onSurfaceCreated(surface: Surface) {
        currentSurface = surface
        repository.updateSurface(surface)
    }

    fun onSurfaceDestroyed() {
        currentSurface = null
        repository.updateSurface(null)
    }

    fun onPointerEvent(action: Int, normX: Float, normY: Float) {
        repository.injectPointer(action, normX, normY)
    }

    fun onKeyEvent(linuxKeycode: Int, pressed: Boolean, shiftMod: Boolean = false) {
        repository.injectKey(linuxKeycode, pressed, shiftMod)
    }

    fun extractRootfsArchive(uri: Uri) {
        if (_engineState.value is DesktopEngineState.Extracting) {
            return
        }

        _engineState.value = DesktopEngineState.Extracting
        _lastErrorMessage.value = null
        _extractedBytes.value = 0L

        viewModelScope.launch {
            val result = repository.extractRootfsFromUri(uri) { bytes ->
                _extractedBytes.value = bytes
            }
            result.onSuccess {
                _isRootfsInstalled.value = repository.isRootfsInstalled()
                _config.value = _config.value.copy(
                    rootfsPath = repository.getDefaultRootfsPath(),
                    executablePath = repository.getDefaultExecutablePath()
                )
                _engineState.value = DesktopEngineState.Idle
            }.onFailure { throwable ->
                _lastErrorMessage.value = throwable.message
                _engineState.value = DesktopEngineState.Error(R.string.error_extract_failed)
            }
        }
    }

    fun startSession() {
        if (_engineState.value is DesktopEngineState.Running || _engineState.value is DesktopEngineState.Starting) {
            return
        }

        _engineState.value = DesktopEngineState.Starting
        _lastErrorMessage.value = null

        viewModelScope.launch {
            val result = repository.startSession(_config.value, currentSurface)
            result.onSuccess {
                _engineState.value = DesktopEngineState.Running
                launch {
                    while (_engineState.value is DesktopEngineState.Running || _engineState.value is DesktopEngineState.Starting) {
                        refreshLogs()
                        kotlinx.coroutines.delay(500)
                    }
                    refreshLogs()
                }
            }.onFailure { throwable ->
                _lastErrorMessage.value = throwable.message
                _engineState.value = DesktopEngineState.Error(R.string.status_error)
                refreshLogs()
            }
        }
    }

    fun stopSession() {
        viewModelScope.launch {
            repository.stopSession()
            _engineState.value = DesktopEngineState.Idle
        }
    }

    override fun onCleared() {
        super.onCleared()
        repository.updateSurface(null)
    }
}
