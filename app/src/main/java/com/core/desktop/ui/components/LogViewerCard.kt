package com.core.desktop.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.core.desktop.R
import com.core.desktop.ui.theme.DesktopColors
import com.core.desktop.ui.theme.DesktopDimens
import com.core.desktop.ui.theme.DesktopTypography

@Composable
fun LogViewerCard(
    logs: List<String>,
    modifier: Modifier = Modifier
) {
    val cardShape = RoundedCornerShape(DesktopDimens.CardCornerRadius)
    val scrollState = rememberScrollState()

    LaunchedEffect(logs.size) {
        if (logs.isNotEmpty()) {
            scrollState.animateScrollTo(scrollState.maxValue)
        }
    }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(DesktopColors.SurfaceCard, cardShape)
            .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, cardShape)
            .padding(DesktopDimens.SpaceExtraLarge)
    ) {
        Text(
            text = stringResource(R.string.title_console_logs),
            style = DesktopTypography.titleMedium
        )
        Spacer(modifier = Modifier.height(DesktopDimens.SpaceSmall))
        Text(
            text = stringResource(R.string.desc_console_logs),
            style = DesktopTypography.bodyMedium
        )

        Spacer(modifier = Modifier.height(DesktopDimens.SpaceMedium))

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 120.dp, max = 220.dp)
                .background(Color(0xFF0F0F0F), RoundedCornerShape(DesktopDimens.ButtonCornerRadius))
                .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, RoundedCornerShape(DesktopDimens.ButtonCornerRadius))
                .padding(DesktopDimens.SpaceMedium)
                .verticalScroll(scrollState)
        ) {
            if (logs.isEmpty()) {
                Text(
                    text = stringResource(R.string.empty_console_logs),
                    color = DesktopColors.TextDisabled,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            } else {
                logs.forEach { line ->
                    Text(
                        text = line,
                        color = if (line.contains("error", ignoreCase = true) || line.contains("failed", ignoreCase = true) || line.contains("keluar", ignoreCase = true)) {
                            DesktopColors.StatusError
                        } else {
                            DesktopColors.TextPrimary
                        },
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp,
                        lineHeight = 16.sp
                    )
                }
            }
        }
    }
}

