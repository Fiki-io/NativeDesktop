package com.core.desktop.ui.screen

import android.app.Activity
import android.content.pm.ActivityInfo
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.core.desktop.R
import com.core.desktop.data.model.DesktopEngineState
import com.core.desktop.ui.components.ConfigCard
import com.core.desktop.ui.components.DesktopSurfaceView
import com.core.desktop.ui.components.GameTelemetryHud
import com.core.desktop.ui.components.LogViewerCard
import com.core.desktop.ui.components.StatusIndicator
import com.core.desktop.ui.components.VirtualGamepadOverlay
import com.core.desktop.ui.theme.DesktopColors
import com.core.desktop.ui.theme.DesktopDimens
import com.core.desktop.ui.theme.DesktopTypography
import com.core.desktop.viewmodel.DesktopViewModel

@Composable
fun DesktopScreen(
    viewModel: DesktopViewModel,
    modifier: Modifier = Modifier
) {
    val hardwareInfo by viewModel.hardwareInfo.collectAsState()
    val engineState by viewModel.engineState.collectAsState()
    val config by viewModel.config.collectAsState()
    val perfMetrics by viewModel.perfMetrics.collectAsState()
    val errorMessage by viewModel.lastErrorMessage.collectAsState()
    val extractedBytes by viewModel.extractedBytes.collectAsState()
    val logs by viewModel.logs.collectAsState()

    var showLiveLogs by remember { mutableStateOf(false) }
    var showVirtualGamepad by remember { mutableStateOf(true) }
    var keyboardToggleCount by remember { mutableIntStateOf(0) }
    var lastPointerNormX by remember { mutableFloatStateOf(0.5f) }
    var lastPointerNormY by remember { mutableFloatStateOf(0.5f) }

    val view = LocalView.current
    val isRunning = engineState is DesktopEngineState.Running
    val isDesktopSessionActive =
        engineState is DesktopEngineState.Starting || engineState is DesktopEngineState.Running

    DisposableEffect(isDesktopSessionActive, view) {
        val activity = view.context as? Activity
        val window = activity?.window
        if (activity != null && window != null) {
            val controller = WindowCompat.getInsetsController(window, view)
            if (isDesktopSessionActive) {
                activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
                controller.systemBarsBehavior =
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                controller.hide(WindowInsetsCompat.Type.systemBars())
            } else {
                activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
                controller.show(WindowInsetsCompat.Type.systemBars())
            }
        }
        onDispose {
            if (activity != null && window != null) {
                activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
                WindowCompat.getInsetsController(window, view)
                    .show(WindowInsetsCompat.Type.systemBars())
            }
        }
    }

    val rootfsPickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            viewModel.extractRootfsArchive(uri)
        }
    }

    val gameExePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            viewModel.importGameFile(uri)
        }
    }

    val backgroundGradient = Brush.verticalGradient(
        colors = listOf(
            DesktopColors.BackgroundTop,
            DesktopColors.BackgroundBase
        )
    )

    BackHandler(enabled = isRunning) {
        viewModel.stopSession()
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(backgroundGradient)
    ) {
        if (isRunning) {
            // Fullscreen Windows Game Surface
            DesktopSurfaceView(
                onSurfaceCreated = viewModel::onSurfaceCreated,
                onSurfaceDestroyed = viewModel::onSurfaceDestroyed,
                onPointerEvent = { action, normX, normY ->
                    lastPointerNormX = normX
                    lastPointerNormY = normY
                    viewModel.onPointerEvent(action, normX, normY)
                },
                onKeyEvent = viewModel::onKeyEvent,
                keyboardToggleCount = keyboardToggleCount
            )

            // Real-Time FPS & Frame-Time Telemetry HUD
            GameTelemetryHud(
                metrics = perfMetrics,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(DesktopDimens.SpaceLarge)
            )

            // Virtual Gamepad Overlay (WASD, Shift, Esc, Space, Enter, E, R, L-Click, R-Click)
            if (showVirtualGamepad) {
                VirtualGamepadOverlay(
                    onKeyEvent = viewModel::onKeyEvent,
                    onMouseButtonEvent = { isRight, pressed ->
                        val action = if (isRight) {
                            if (pressed) 3 else 4
                        } else {
                            if (pressed) 0 else 1
                        }
                        viewModel.onPointerEvent(action, lastPointerNormX, lastPointerNormY)
                    }
                )
            }

            // Bottom-aligned control bar
            Column(
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(DesktopDimens.SpaceLarge)
            ) {
                if (showLiveLogs) {
                    LogViewerCard(logs = logs)
                    Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))
                }

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End
                ) {
                    Button(
                        onClick = { showVirtualGamepad = !showVirtualGamepad },
                        shape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (showVirtualGamepad) DesktopColors.Accent else DesktopColors.SurfaceInteractive,
                            contentColor = DesktopColors.TextPrimary
                        )
                    ) {
                        Text(
                            text = stringResource(R.string.btn_toggle_gamepad),
                            style = DesktopTypography.labelLarge
                        )
                    }

                    Spacer(modifier = Modifier.width(DesktopDimens.SpaceSmall))

                    Button(
                        onClick = { keyboardToggleCount++ },
                        shape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = DesktopColors.SurfaceInteractive,
                            contentColor = DesktopColors.TextPrimary
                        )
                    ) {
                        Text(
                            text = stringResource(R.string.btn_toggle_keyboard),
                            style = DesktopTypography.labelLarge
                        )
                    }

                    Spacer(modifier = Modifier.width(DesktopDimens.SpaceSmall))

                    Button(
                        onClick = { showLiveLogs = !showLiveLogs },
                        shape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (showLiveLogs) DesktopColors.Accent else DesktopColors.SurfaceInteractive,
                            contentColor = DesktopColors.TextPrimary
                        )
                    ) {
                        Text(
                            text = stringResource(
                                if (showLiveLogs) R.string.btn_hide_logs else R.string.btn_show_logs
                            ),
                            style = DesktopTypography.labelLarge
                        )
                    }

                    Spacer(modifier = Modifier.width(DesktopDimens.SpaceSmall))

                    Button(
                        onClick = viewModel::stopSession,
                        shape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = DesktopColors.SurfaceInteractive,
                            contentColor = DesktopColors.TextPrimary
                        )
                    ) {
                        Text(
                            text = stringResource(R.string.btn_stop_session),
                            style = DesktopTypography.labelLarge
                        )
                    }
                }
            }
        } else {
            // Game Engine Configuration & Launcher Screen
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .statusBarsPadding()
                    .verticalScroll(rememberScrollState())
                    .padding(DesktopDimens.SpaceExtraLarge)
            ) {
                Text(
                    text = stringResource(R.string.app_name),
                    style = DesktopTypography.headlineMedium
                )

                Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

                StatusIndicator(
                    state = engineState,
                    errorMessage = errorMessage
                )

                Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraLarge))

                ConfigCard(
                    hardwareInfo = hardwareInfo,
                    config = config,
                    isStarting = engineState is DesktopEngineState.Starting,
                    isExtracting = engineState is DesktopEngineState.Extracting,
                    extractedBytes = extractedBytes,
                    onRootfsChange = viewModel::updateRootfsPath,
                    onGameTargetChange = viewModel::updateGameTarget,
                    onArgsChange = viewModel::updateSessionArgs,
                    onGpuBackendChange = viewModel::updateGpuBackend,
                    onDxWrapperChange = viewModel::updateDxWrapper,
                    onDynarecPresetChange = viewModel::updateDynarecPreset,
                    onResolutionChange = viewModel::updateRenderResolution,
                    onDisplayModeChange = viewModel::updateDisplayMode,
                    onEsyncChange = viewModel::updateEnableEsync,
                    onPerfCoresChange = viewModel::updateLockPerformanceCores,
                    onStartClick = viewModel::startSession,
                    onResetClick = viewModel::resetToDefaults,
                    onSelectArchiveClick = {
                        rootfsPickerLauncher.launch(
                            arrayOf(
                                "application/gzip",
                                "application/x-gzip",
                                "application/x-tar",
                                "application/octet-stream",
                                "*/*"
                            )
                        )
                    },
                    onSelectGameExeClick = {
                        gameExePickerLauncher.launch(
                            arrayOf(
                                "application/x-msdownload",
                                "application/x-dosexec",
                                "application/vnd.microsoft.portable-executable",
                                "application/octet-stream",
                                "*/*"
                            )
                        )
                    }
                )

                Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraLarge))

                LogViewerCard(logs = logs)
            }
        }
    }
}
