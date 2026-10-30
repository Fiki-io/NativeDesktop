package com.core.desktop.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes

val DesktopShapes = Shapes(
    small = RoundedCornerShape(DesktopDimens.ButtonCornerRadius),
    medium = RoundedCornerShape(DesktopDimens.CardCornerRadius),
    large = RoundedCornerShape(DesktopDimens.DialogCornerRadius)
)

