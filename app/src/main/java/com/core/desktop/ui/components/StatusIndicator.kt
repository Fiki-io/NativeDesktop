package com.core.desktop.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.core.desktop.R
import com.core.desktop.data.model.DesktopEngineState
import com.core.desktop.ui.theme.DesktopColors
import com.core.desktop.ui.theme.DesktopDimens
import com.core.desktop.ui.theme.DesktopTypography

@Composable
fun StatusIndicator(
    state: DesktopEngineState,
    errorMessage: String?,
    modifier: Modifier = Modifier
) {
    val (dotColor, text) = when (state) {
        is DesktopEngineState.Idle -> {
            DesktopColors.TextSecondary to stringResource(R.string.status_idle)
        }
        is DesktopEngineState.Starting -> {
            DesktopColors.Accent to stringResource(R.string.status_starting)
        }
        is DesktopEngineState.Extracting -> {
            DesktopColors.Accent to stringResource(R.string.status_extracting)
        }
        is DesktopEngineState.Running -> {
            DesktopColors.StatusActive to stringResource(R.string.status_running)
        }
        is DesktopEngineState.Error -> {
            val msg = errorMessage ?: ""
            DesktopColors.StatusError to stringResource(R.string.status_error, msg)
        }
    }

    Box(
        modifier = modifier
            .background(DesktopColors.SurfaceCard, RoundedCornerShape(100.dp))
            .border(DesktopDimens.BorderWidth, DesktopColors.BorderSubtle, RoundedCornerShape(100.dp))
            .padding(horizontal = DesktopDimens.SpaceMedium, vertical = DesktopDimens.SpaceSmall)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(DesktopDimens.StatusIndicatorSize)
                    .background(dotColor, CircleShape)
            )
            Spacer(modifier = Modifier.width(DesktopDimens.SpaceSmall))
            Text(
                text = text,
                style = DesktopTypography.bodyMedium,
                color = DesktopColors.TextPrimary
            )
        }
    }
}

