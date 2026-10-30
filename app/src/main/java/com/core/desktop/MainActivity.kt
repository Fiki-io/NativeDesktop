package com.core.desktop

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.lifecycle.viewmodel.compose.viewModel
import com.core.desktop.ui.screen.DesktopScreen
import com.core.desktop.ui.theme.NativeDesktopTheme
import com.core.desktop.viewmodel.DesktopViewModel

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        setContent {
            NativeDesktopTheme {
                val viewModel: DesktopViewModel = viewModel()
                DesktopScreen(viewModel = viewModel)
            }
        }
    }
}

