package com.core.desktop.ui.components

import android.annotation.SuppressLint
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView

@SuppressLint("ClickableViewAccessibility")
@Composable
fun DesktopSurfaceView(
    modifier: Modifier = Modifier,
    onSurfaceCreated: (Surface) -> Unit,
    onSurfaceDestroyed: () -> Unit,
    onPointerEvent: (action: Int, normX: Float, normY: Float) -> Unit
) {
    AndroidView(
        modifier = modifier.fillMaxSize(),
        factory = { context ->
            SurfaceView(context).apply {
                isFocusable = true
                isFocusableInTouchMode = true
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        onSurfaceCreated(holder.surface)
                    }

                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        // Surface geometry changes handled natively via ANativeWindow_setBuffersGeometry
                    }

                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        onSurfaceDestroyed()
                    }
                })
                setOnTouchListener { view, event ->
                    val w = view.width.toFloat()
                    val h = view.height.toFloat()
                    if (w > 0f && h > 0f) {
                        val normX = (event.x / w).coerceIn(0f, 1f)
                        val normY = (event.y / h).coerceIn(0f, 1f)
                        when (event.actionMasked) {
                            MotionEvent.ACTION_DOWN -> onPointerEvent(0, normX, normY)
                            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> onPointerEvent(1, normX, normY)
                            MotionEvent.ACTION_MOVE -> onPointerEvent(2, normX, normY)
                        }
                    }
                    true
                }
            }
        }
    )
}


