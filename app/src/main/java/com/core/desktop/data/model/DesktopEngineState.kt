package com.core.desktop.data.model

import androidx.annotation.StringRes

sealed interface DesktopEngineState {
    data object Idle : DesktopEngineState
    data object Starting : DesktopEngineState
    data object Extracting : DesktopEngineState
    data object Running : DesktopEngineState
    data class Error(@StringRes val messageResId: Int) : DesktopEngineState
}

