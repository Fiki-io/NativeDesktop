package com.core.desktop.ui.components

import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.core.desktop.R
import com.core.desktop.data.model.PerfMetrics
import com.core.desktop.ui.theme.DesktopColors
import com.core.desktop.ui.theme.DesktopDimens
import com.core.desktop.ui.theme.DesktopTypography

@Composable
fun GameTelemetryHud(
    metrics: PerfMetrics,
    modifier: Modifier = Modifier
) {
    val hudShape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius)
    Box(
        modifier = modifier
            .background(DesktopColors.ScrimBackdrop, hudShape)
            .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, hudShape)
            .padding(
                horizontal = DesktopDimens.SpaceMedium,
                vertical = DesktopDimens.SpaceExtraSmall
            )
    ) {
        Text(
            text = stringResource(
                R.string.hud_fps_format,
                metrics.fps,
                metrics.frameTimeMs,
                metrics.renderWidth,
                metrics.renderHeight
            ),
            style = DesktopTypography.labelLarge
        )
    }
}

@Composable
fun VirtualGamepadOverlay(
    onKeyEvent: (linuxKeycode: Int, pressed: Boolean, shiftMod: Boolean) -> Unit,
    onMouseButtonEvent: (isRight: Boolean, pressed: Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .fillMaxSize()
            .padding(DesktopDimens.SpaceLarge)
    ) {
        // Left cluster: ESC + SHIFT + WASD D-Pad
        Column(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(bottom = DesktopDimens.SpaceDoubleLarge),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)) {
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_esc),
                    widthDp = 56,
                    heightDp = 40,
                    onStateChanged = { pressed -> onKeyEvent(1, pressed, false) }
                )
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_shift),
                    widthDp = 64,
                    heightDp = 40,
                    onStateChanged = { pressed -> onKeyEvent(42, pressed, false) }
                )
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

            // W (Up)
            HoldableGamePadButton(
                label = stringResource(R.string.pad_up),
                widthDp = 56,
                heightDp = 52,
                onStateChanged = { pressed -> onKeyEvent(17, pressed, false) }
            )

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraSmall))

            // A (Left) + S (Down) + D (Right)
            Row(horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceExtraSmall)) {
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_left),
                    widthDp = 56,
                    heightDp = 52,
                    onStateChanged = { pressed -> onKeyEvent(30, pressed, false) }
                )
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_down),
                    widthDp = 56,
                    heightDp = 52,
                    onStateChanged = { pressed -> onKeyEvent(31, pressed, false) }
                )
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_right),
                    widthDp = 56,
                    heightDp = 52,
                    onStateChanged = { pressed -> onKeyEvent(32, pressed, false) }
                )
            }
        }

        // Right cluster: L-CLICK / R-CLICK + E / R + SPACE / ENTER
        Column(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(bottom = 64.dp),
            horizontalAlignment = Alignment.End
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)) {
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_lmb),
                    widthDp = 76,
                    heightDp = 44,
                    onStateChanged = { pressed -> onMouseButtonEvent(false, pressed) }
                )
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_rmb),
                    widthDp = 76,
                    heightDp = 44,
                    onStateChanged = { pressed -> onMouseButtonEvent(true, pressed) }
                )
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))

            Row(horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)) {
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_e),
                    widthDp = 56,
                    heightDp = 48,
                    onStateChanged = { pressed -> onKeyEvent(18, pressed, false) }
                )
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_r),
                    widthDp = 56,
                    heightDp = 48,
                    onStateChanged = { pressed -> onKeyEvent(19, pressed, false) }
                )
            }

            Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))

            Row(horizontalArrangement = Arrangement.spacedBy(DesktopDimens.SpaceSmall)) {
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_space),
                    widthDp = 76,
                    heightDp = 52,
                    onStateChanged = { pressed -> onKeyEvent(57, pressed, false) }
                )
                HoldableGamePadButton(
                    label = stringResource(R.string.pad_enter),
                    widthDp = 76,
                    heightDp = 52,
                    onStateChanged = { pressed -> onKeyEvent(28, pressed, false) }
                )
            }
        }
    }
}

@Composable
private fun HoldableGamePadButton(
    label: String,
    widthDp: Int,
    heightDp: Int,
    onStateChanged: (Boolean) -> Unit
) {
    val view = LocalView.current
    var isPressed by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius)
    val bgColor = if (isPressed) DesktopColors.Accent else DesktopColors.ScrimBackdrop
    val borderColor = if (isPressed) DesktopColors.Accent else DesktopColors.BorderInteractive

    Box(
        modifier = Modifier
            .size(width = widthDp.dp, height = heightDp.dp)
            .clip(shape)
            .background(bgColor, shape)
            .border(DesktopDimens.BorderWidth, borderColor, shape)
            .pointerInput(label) {
                awaitEachGesture {
                    awaitFirstDown(requireUnconsumed = false)
                    isPressed = true
                    view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
                    onStateChanged(true)
                    waitForUpOrCancellation()
                    isPressed = false
                    onStateChanged(false)
                }
            },
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            style = DesktopTypography.labelLarge
        )
    }
}
