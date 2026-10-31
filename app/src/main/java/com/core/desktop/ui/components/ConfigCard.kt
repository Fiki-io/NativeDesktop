package com.core.desktop.ui.components

import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import com.core.desktop.R
import com.core.desktop.data.model.DesktopSessionConfig
import com.core.desktop.data.model.DisplayMode
import com.core.desktop.data.model.DxWrapper
import com.core.desktop.data.model.DynarecPreset
import com.core.desktop.data.model.GpuBackend
import com.core.desktop.data.model.RenderResolution
import com.core.desktop.data.model.SoCHardwareInfo
import com.core.desktop.ui.theme.DesktopColors
import com.core.desktop.ui.theme.DesktopDimens
import com.core.desktop.ui.theme.DesktopTypography

@Composable
fun ConfigCard(
    hardwareInfo: SoCHardwareInfo,
    config: DesktopSessionConfig,
    isStarting: Boolean,
    isExtracting: Boolean,
    extractedBytes: Long,
    onRootfsChange: (String) -> Unit,
    onGameTargetChange: (String) -> Unit,
    onArgsChange: (String) -> Unit,
    onGpuBackendChange: (GpuBackend) -> Unit,
    onDxWrapperChange: (DxWrapper) -> Unit,
    onDynarecPresetChange: (DynarecPreset) -> Unit,
    onResolutionChange: (RenderResolution) -> Unit,
    onDisplayModeChange: (DisplayMode) -> Unit,
    onEsyncChange: (Boolean) -> Unit,
    onPerfCoresChange: (Boolean) -> Unit,
    onStartClick: () -> Unit,
    onResetClick: () -> Unit,
    onSelectArchiveClick: () -> Unit,
    onSelectGameExeClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val view = LocalView.current
    val cardShape = RoundedCornerShape(DesktopDimens.CardCornerRadius)
    val buttonShape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius)

    Column(modifier = modifier.fillMaxWidth()) {
        // Section 1: Real Hardware Profile
        SectionHeader(text = stringResource(R.string.section_hardware_detected))
        Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(DesktopColors.SurfaceCard, cardShape)
                .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, cardShape)
                .padding(DesktopDimens.SpaceLarge)
        ) {
            HardwareInfoRow(
                label = stringResource(R.string.label_soc_detected),
                value = hardwareInfo.socName
            )
            HorizontalDivider(
                modifier = Modifier.padding(vertical = DesktopDimens.SpaceSmall),
                color = DesktopColors.DividerSubtle
            )
            HardwareInfoRow(
                label = stringResource(R.string.label_gpu_node_detected),
                value = hardwareInfo.gpuDriverNode
            )
            HorizontalDivider(
                modifier = Modifier.padding(vertical = DesktopDimens.SpaceSmall),
                color = DesktopColors.DividerSubtle
            )
            HardwareInfoRow(
                label = stringResource(R.string.label_cpu_topology_detected),
                value = hardwareInfo.cpuTopology
            )
        }

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraLarge))

        // Section 2: Game Target & Container Setup
        SectionHeader(text = stringResource(R.string.section_game_target))
        Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(DesktopColors.SurfaceCard, cardShape)
                .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, cardShape)
                .padding(DesktopDimens.SpaceExtraLarge)
        ) {
            Text(
                text = stringResource(R.string.title_configuration),
                style = DesktopTypography.titleMedium
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraSmall))
            Text(
                text = stringResource(R.string.desc_configuration),
                style = DesktopTypography.bodyMedium
            )

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

            // Rootfs Archive Picker
            OutlinedButton(
                onClick = onSelectArchiveClick,
                enabled = !isExtracting && !isStarting,
                shape = buttonShape,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.outlinedButtonColors(
                    contentColor = DesktopColors.TextPrimary
                ),
                border = ButtonDefaults.outlinedButtonBorder.copy(
                    brush = SolidColor(DesktopColors.BorderInteractive)
                )
            ) {
                val buttonText = if (isExtracting) {
                    val mb = extractedBytes / (1024 * 1024)
                    "${stringResource(R.string.status_extracting)} (${mb} MB)"
                } else {
                    stringResource(R.string.btn_select_rootfs)
                }
                Text(
                    text = buttonText,
                    style = DesktopTypography.labelLarge
                )
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

            // Windows .exe Game File Importer
            OutlinedButton(
                onClick = onSelectGameExeClick,
                enabled = !isExtracting && !isStarting,
                shape = buttonShape,
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.outlinedButtonColors(
                    contentColor = DesktopColors.Accent
                ),
                border = ButtonDefaults.outlinedButtonBorder.copy(
                    brush = SolidColor(DesktopColors.Accent)
                )
            ) {
                Text(
                    text = stringResource(R.string.btn_select_game_exe),
                    style = DesktopTypography.labelLarge
                )
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

            // Quick Target Presets
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)
            ) {
                val presets = listOf(
                    "winecfg" to stringResource(R.string.preset_target_winecfg),
                    "explorer" to stringResource(R.string.preset_target_explorer),
                    "wineconsole" to stringResource(R.string.preset_target_cmd),
                    "terminal" to stringResource(R.string.preset_target_terminal)
                )
                presets.forEach { (cmd, label) ->
                    OptionPill(
                        text = label,
                        selected = config.gameTarget == cmd,
                        onClick = {
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                            onGameTargetChange(cmd)
                        }
                    )
                }
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

            OutlinedTextField(
                value = config.gameTarget,
                onValueChange = onGameTargetChange,
                label = { Text(stringResource(R.string.label_game_target)) },
                placeholder = { Text(stringResource(R.string.hint_game_target)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                shape = buttonShape,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = DesktopColors.Accent,
                    unfocusedBorderColor = DesktopColors.BorderSubtle,
                    focusedTextColor = DesktopColors.TextPrimary,
                    unfocusedTextColor = DesktopColors.TextPrimary
                )
            )

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

            OutlinedTextField(
                value = config.sessionArgs.joinToString(" "),
                onValueChange = onArgsChange,
                label = { Text(stringResource(R.string.label_session_args)) },
                placeholder = { Text(stringResource(R.string.hint_session_args)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                shape = buttonShape,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = DesktopColors.Accent,
                    unfocusedBorderColor = DesktopColors.BorderSubtle,
                    focusedTextColor = DesktopColors.TextPrimary,
                    unfocusedTextColor = DesktopColors.TextPrimary
                )
            )

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

            OutlinedTextField(
                value = config.rootfsPath,
                onValueChange = onRootfsChange,
                label = { Text(stringResource(R.string.label_rootfs_path)) },
                placeholder = { Text(stringResource(R.string.hint_rootfs_path)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                shape = buttonShape,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = DesktopColors.Accent,
                    unfocusedBorderColor = DesktopColors.BorderSubtle,
                    focusedTextColor = DesktopColors.TextPrimary,
                    unfocusedTextColor = DesktopColors.TextPrimary
                )
            )
        }

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraLarge))

        // Section 3: Multi-SoC Graphics & Dynarec Tuning
        SectionHeader(text = stringResource(R.string.section_graphics_tuning))
        Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(DesktopColors.SurfaceCard, cardShape)
                .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, cardShape)
                .padding(DesktopDimens.SpaceExtraLarge)
        ) {
            // GPU Backend Selector
            Text(
                text = stringResource(R.string.label_gpu_backend),
                style = DesktopTypography.labelLarge
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)
            ) {
                val gpuOptions = listOf(
                    GpuBackend.AUTO to stringResource(R.string.gpu_backend_auto),
                    GpuBackend.MALI_UNIVERSAL to stringResource(R.string.gpu_backend_mali),
                    GpuBackend.ADRENO_TURNIP to stringResource(R.string.gpu_backend_turnip),
                    GpuBackend.SOFTWARE_LVP to stringResource(R.string.gpu_backend_lvp)
                )
                gpuOptions.forEach { (backend, label) ->
                    OptionPill(
                        text = label,
                        selected = config.gpuBackend == backend,
                        onClick = {
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                            onGpuBackendChange(backend)
                        }
                    )
                }
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

            // DirectX Wrapper Selector
            Text(
                text = stringResource(R.string.label_dx_wrapper),
                style = DesktopTypography.labelLarge
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)
            ) {
                val dxOptions = listOf(
                    DxWrapper.DXVK_ASYNC to stringResource(R.string.dx_wrapper_dxvk),
                    DxWrapper.WINED3D to stringResource(R.string.dx_wrapper_wined3d)
                )
                dxOptions.forEach { (wrapper, label) ->
                    OptionPill(
                        text = label,
                        selected = config.dxWrapper == wrapper,
                        onClick = {
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                            onDxWrapperChange(wrapper)
                        }
                    )
                }
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

            // Box64 Dynarec Preset Selector
            Text(
                text = stringResource(R.string.label_dynarec_preset),
                style = DesktopTypography.labelLarge
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)
            ) {
                val dynarecOptions = listOf(
                    DynarecPreset.PERFORMANCE to stringResource(R.string.dynarec_performance),
                    DynarecPreset.BALANCED to stringResource(R.string.dynarec_balanced),
                    DynarecPreset.STABILITY to stringResource(R.string.dynarec_stability)
                )
                dynarecOptions.forEach { (preset, label) ->
                    OptionPill(
                        text = label,
                        selected = config.dynarecPreset == preset,
                        onClick = {
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                            onDynarecPresetChange(preset)
                        }
                    )
                }
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

            // Internal Render Resolution Selector
            Text(
                text = stringResource(R.string.label_render_resolution),
                style = DesktopTypography.labelLarge
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)
            ) {
                RenderResolution.values().forEach { res ->
                    OptionPill(
                        text = res.label,
                        selected = config.renderResolution == res,
                        onClick = {
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                            onResolutionChange(res)
                        }
                    )
                }
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

            // Display Pipeline Mode Selector
            Text(
                text = stringResource(R.string.label_display_mode),
                style = DesktopTypography.labelLarge
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)
            ) {
                val displayOptions = listOf(
                    DisplayMode.WAYLAND_DIRECT to stringResource(R.string.display_mode_wayland),
                    DisplayMode.KIOSK_XWAYLAND to stringResource(R.string.display_mode_kiosk)
                )
                displayOptions.forEach { (mode, label) ->
                    OptionPill(
                        text = label,
                        selected = config.displayMode == mode,
                        onClick = {
                            view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                            onDisplayModeChange(mode)
                        }
                    )
                }
            }

            HorizontalDivider(
                modifier = Modifier.padding(vertical = DesktopDimens.SpaceLarge),
                color = DesktopColors.DividerSubtle
            )

            // Toggle: Wine Esync & Fsync
            ToggleRow(
                title = stringResource(R.string.label_toggle_esync),
                description = stringResource(R.string.desc_toggle_esync),
                checked = config.enableEsync,
                onCheckedChange = {
                    view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                    onEsyncChange(it)
                }
            )

            HorizontalDivider(
                modifier = Modifier.padding(vertical = DesktopDimens.SpaceMedium),
                color = DesktopColors.DividerSubtle
            )

            // Toggle: Lock Performance / Prime CPU Cores
            ToggleRow(
                title = stringResource(R.string.label_toggle_perf_cores),
                description = stringResource(R.string.desc_toggle_perf_cores),
                checked = config.lockPerformanceCores,
                onCheckedChange = {
                    view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                    onPerfCoresChange(it)
                }
            )

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraLarge))

            Row(modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(
                    onClick = onResetClick,
                    shape = buttonShape,
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.outlinedButtonColors(
                        contentColor = DesktopColors.TextSecondary
                    ),
                    border = ButtonDefaults.outlinedButtonBorder.copy(
                        brush = SolidColor(DesktopColors.BorderSubtle)
                    )
                ) {
                    Text(
                        text = stringResource(R.string.btn_reset_defaults),
                        style = DesktopTypography.labelLarge
                    )
                }

                Spacer(modifier = Modifier.width(DesktopDimens.SpaceMedium))

                Button(
                    onClick = {
                        view.performHapticFeedback(HapticFeedbackConstants.CONTEXT_CLICK)
                        onStartClick()
                    },
                    enabled = !isStarting && !isExtracting,
                    shape = buttonShape,
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = DesktopColors.Accent,
                        contentColor = DesktopColors.TextPrimary
                    )
                ) {
                    Text(
                        text = stringResource(R.string.btn_start_session),
                        style = DesktopTypography.labelLarge
                    )
                }
            }
        }
    }
}

@Composable
private fun SectionHeader(text: String) {
    Text(
        text = text,
        style = DesktopTypography.bodyMedium,
        modifier = Modifier.padding(horizontal = DesktopDimens.SpaceSmall)
    )
}

@Composable
private fun HardwareInfoRow(
    label: String,
    value: String
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            style = DesktopTypography.bodyMedium
        )
        Spacer(modifier = Modifier.width(DesktopDimens.SpaceMedium))
        Text(
            text = value,
            style = DesktopTypography.labelLarge
        )
    }
}

@Composable
private fun OptionPill(
    text: String,
    selected: Boolean,
    onClick: () -> Unit
) {
    val pillShape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius)
    val bgColor = if (selected) DesktopColors.Accent else DesktopColors.SurfaceInteractive
    val borderColor = if (selected) DesktopColors.Accent else DesktopColors.BorderSubtle

    Box(
        modifier = Modifier
            .clip(pillShape)
            .background(bgColor, pillShape)
            .border(DesktopDimens.BorderWidth, borderColor, pillShape)
            .clickable(onClick = onClick)
            .padding(
                horizontal = DesktopDimens.SpaceLarge,
                vertical = DesktopDimens.SpaceSmall
            ),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = text,
            style = DesktopTypography.labelLarge
        )
    }
}

@Composable
private fun ToggleRow(
    title: String,
    description: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onCheckedChange(!checked) }
            .padding(vertical = DesktopDimens.SpaceExtraSmall),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = DesktopTypography.labelLarge
            )
            Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraSmall))
            Text(
                text = description,
                style = DesktopTypography.bodyMedium
            )
        }
        Spacer(modifier = Modifier.width(DesktopDimens.SpaceLarge))
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = DesktopColors.Accent,
                uncheckedThumbColor = DesktopColors.TextSecondary,
                uncheckedTrackColor = DesktopColors.SurfaceInteractive
            )
        )
    }
}
