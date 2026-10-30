package com.core.desktop.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import com.core.desktop.R
import com.core.desktop.data.model.DesktopSessionConfig
import com.core.desktop.ui.theme.DesktopColors
import com.core.desktop.ui.theme.DesktopDimens
import com.core.desktop.ui.theme.DesktopTypography

@Composable
fun ConfigCard(
    config: DesktopSessionConfig,
    isStarting: Boolean,
    isExtracting: Boolean,
    extractedBytes: Long,
    onRootfsChange: (String) -> Unit,
    onExecutableChange: (String) -> Unit,
    onArgsChange: (String) -> Unit,
    onStartClick: () -> Unit,
    onResetClick: () -> Unit,
    onSelectArchiveClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val cardShape = RoundedCornerShape(DesktopDimens.CardCornerRadius)
    val buttonShape = RoundedCornerShape(DesktopDimens.ButtonCornerRadius)

    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(DesktopColors.SurfaceCard, cardShape)
            .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, cardShape)
            .padding(DesktopDimens.SpaceExtraLarge)
    ) {
        Text(
            text = stringResource(R.string.title_configuration),
            style = DesktopTypography.titleMedium
        )
        Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
        Text(
            text = stringResource(R.string.desc_configuration),
            style = DesktopTypography.bodyMedium
        )

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

        // Archive picker button
        OutlinedButton(
            onClick = onSelectArchiveClick,
            enabled = !isExtracting && !isStarting,
            shape = buttonShape,
            modifier = Modifier.fillMaxWidth(),
            colors = ButtonDefaults.outlinedButtonColors(
                contentColor = DesktopColors.Accent
            ),
            border = ButtonDefaults.outlinedButtonBorder.copy(
                brush = androidx.compose.ui.graphics.SolidColor(DesktopColors.Accent)
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

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceExtraLarge))

        // Rootfs input
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

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

        // Executable input
        OutlinedTextField(
            value = config.executablePath,
            onValueChange = onExecutableChange,
            label = { Text(stringResource(R.string.label_executable_path)) },
            placeholder = { Text(stringResource(R.string.hint_executable_path)) },
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

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceLarge))

        // Args input
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
                    brush = androidx.compose.ui.graphics.SolidColor(DesktopColors.BorderSubtle)
                )
            ) {
                Text(
                    text = stringResource(R.string.btn_reset_defaults),
                    style = DesktopTypography.labelLarge
                )
            }

            Spacer(modifier = Modifier.width(DesktopDimens.SpaceMedium))

            Button(
                onClick = onStartClick,
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

