package com.core.desktop.ui.components

import android.annotation.SuppressLint
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewConfiguration
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import kotlin.math.hypot

private data class LinuxKeyMapping(val keycode: Int, val shift: Boolean = false)

private fun mapCharToLinuxKey(ch: Char): LinuxKeyMapping? {
    return when (ch) {
        'a' -> LinuxKeyMapping(30)
        'b' -> LinuxKeyMapping(48)
        'c' -> LinuxKeyMapping(46)
        'd' -> LinuxKeyMapping(32)
        'e' -> LinuxKeyMapping(18)
        'f' -> LinuxKeyMapping(33)
        'g' -> LinuxKeyMapping(34)
        'h' -> LinuxKeyMapping(35)
        'i' -> LinuxKeyMapping(23)
        'j' -> LinuxKeyMapping(36)
        'k' -> LinuxKeyMapping(37)
        'l' -> LinuxKeyMapping(38)
        'm' -> LinuxKeyMapping(50)
        'n' -> LinuxKeyMapping(49)
        'o' -> LinuxKeyMapping(24)
        'p' -> LinuxKeyMapping(25)
        'q' -> LinuxKeyMapping(16)
        'r' -> LinuxKeyMapping(19)
        's' -> LinuxKeyMapping(31)
        't' -> LinuxKeyMapping(20)
        'u' -> LinuxKeyMapping(22)
        'v' -> LinuxKeyMapping(47)
        'w' -> LinuxKeyMapping(17)
        'x' -> LinuxKeyMapping(45)
        'y' -> LinuxKeyMapping(21)
        'z' -> LinuxKeyMapping(44)
        'A' -> LinuxKeyMapping(30, true)
        'B' -> LinuxKeyMapping(48, true)
        'C' -> LinuxKeyMapping(46, true)
        'D' -> LinuxKeyMapping(32, true)
        'E' -> LinuxKeyMapping(18, true)
        'F' -> LinuxKeyMapping(33, true)
        'G' -> LinuxKeyMapping(34, true)
        'H' -> LinuxKeyMapping(35, true)
        'I' -> LinuxKeyMapping(23, true)
        'J' -> LinuxKeyMapping(36, true)
        'K' -> LinuxKeyMapping(37, true)
        'L' -> LinuxKeyMapping(38, true)
        'M' -> LinuxKeyMapping(50, true)
        'N' -> LinuxKeyMapping(49, true)
        'O' -> LinuxKeyMapping(24, true)
        'P' -> LinuxKeyMapping(25, true)
        'Q' -> LinuxKeyMapping(16, true)
        'R' -> LinuxKeyMapping(19, true)
        'S' -> LinuxKeyMapping(31, true)
        'T' -> LinuxKeyMapping(20, true)
        'U' -> LinuxKeyMapping(22, true)
        'V' -> LinuxKeyMapping(47, true)
        'W' -> LinuxKeyMapping(17, true)
        'X' -> LinuxKeyMapping(45, true)
        'Y' -> LinuxKeyMapping(21, true)
        'Z' -> LinuxKeyMapping(44, true)
        '1' -> LinuxKeyMapping(2)
        '2' -> LinuxKeyMapping(3)
        '3' -> LinuxKeyMapping(4)
        '4' -> LinuxKeyMapping(5)
        '5' -> LinuxKeyMapping(6)
        '6' -> LinuxKeyMapping(7)
        '7' -> LinuxKeyMapping(8)
        '8' -> LinuxKeyMapping(9)
        '9' -> LinuxKeyMapping(10)
        '0' -> LinuxKeyMapping(11)
        '!' -> LinuxKeyMapping(2, true)
        '@' -> LinuxKeyMapping(3, true)
        '#' -> LinuxKeyMapping(4, true)
        '$' -> LinuxKeyMapping(5, true)
        '%' -> LinuxKeyMapping(6, true)
        '^' -> LinuxKeyMapping(7, true)
        '&' -> LinuxKeyMapping(8, true)
        '*' -> LinuxKeyMapping(9, true)
        '(' -> LinuxKeyMapping(10, true)
        ')' -> LinuxKeyMapping(11, true)
        ' ' -> LinuxKeyMapping(57)
        '\n', '\r' -> LinuxKeyMapping(28)
        '\t' -> LinuxKeyMapping(15)
        '-' -> LinuxKeyMapping(12)
        '_' -> LinuxKeyMapping(12, true)
        '=' -> LinuxKeyMapping(13)
        '+' -> LinuxKeyMapping(13, true)
        '[' -> LinuxKeyMapping(26)
        '{' -> LinuxKeyMapping(26, true)
        ']' -> LinuxKeyMapping(27)
        '}' -> LinuxKeyMapping(27, true)
        '\\' -> LinuxKeyMapping(43)
        '|' -> LinuxKeyMapping(43, true)
        ';' -> LinuxKeyMapping(39)
        ':' -> LinuxKeyMapping(39, true)
        '\'' -> LinuxKeyMapping(40)
        '"' -> LinuxKeyMapping(40, true)
        '`' -> LinuxKeyMapping(41)
        '~' -> LinuxKeyMapping(41, true)
        ',' -> LinuxKeyMapping(51)
        '<' -> LinuxKeyMapping(51, true)
        '.' -> LinuxKeyMapping(52)
        '>' -> LinuxKeyMapping(52, true)
        '/' -> LinuxKeyMapping(53)
        '?' -> LinuxKeyMapping(53, true)
        else -> null
    }
}

private fun mapAndroidKeyCodeToLinux(keyCode: Int): Int? {
    return when (keyCode) {
        KeyEvent.KEYCODE_DEL -> 14          // KEY_BACKSPACE
        KeyEvent.KEYCODE_FORWARD_DEL -> 111 // KEY_DELETE
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> 28 // KEY_ENTER
        KeyEvent.KEYCODE_TAB -> 15          // KEY_TAB
        KeyEvent.KEYCODE_ESCAPE -> 1        // KEY_ESC
        KeyEvent.KEYCODE_SPACE -> 57        // KEY_SPACE
        KeyEvent.KEYCODE_DPAD_UP -> 103     // KEY_UP
        KeyEvent.KEYCODE_DPAD_LEFT -> 105   // KEY_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> 106  // KEY_RIGHT
        KeyEvent.KEYCODE_DPAD_DOWN -> 108   // KEY_DOWN
        KeyEvent.KEYCODE_MOVE_HOME -> 102   // KEY_HOME
        KeyEvent.KEYCODE_MOVE_END -> 107    // KEY_END
        KeyEvent.KEYCODE_PAGE_UP -> 104     // KEY_PAGEUP
        KeyEvent.KEYCODE_PAGE_DOWN -> 109   // KEY_PAGEDOWN
        KeyEvent.KEYCODE_A -> 30
        KeyEvent.KEYCODE_B -> 48
        KeyEvent.KEYCODE_C -> 46
        KeyEvent.KEYCODE_D -> 32
        KeyEvent.KEYCODE_E -> 18
        KeyEvent.KEYCODE_F -> 33
        KeyEvent.KEYCODE_G -> 34
        KeyEvent.KEYCODE_H -> 35
        KeyEvent.KEYCODE_I -> 23
        KeyEvent.KEYCODE_J -> 36
        KeyEvent.KEYCODE_K -> 37
        KeyEvent.KEYCODE_L -> 38
        KeyEvent.KEYCODE_M -> 50
        KeyEvent.KEYCODE_N -> 49
        KeyEvent.KEYCODE_O -> 24
        KeyEvent.KEYCODE_P -> 25
        KeyEvent.KEYCODE_Q -> 16
        KeyEvent.KEYCODE_R -> 19
        KeyEvent.KEYCODE_S -> 31
        KeyEvent.KEYCODE_T -> 20
        KeyEvent.KEYCODE_U -> 22
        KeyEvent.KEYCODE_V -> 47
        KeyEvent.KEYCODE_W -> 17
        KeyEvent.KEYCODE_X -> 45
        KeyEvent.KEYCODE_Y -> 21
        KeyEvent.KEYCODE_Z -> 44
        KeyEvent.KEYCODE_1 -> 2
        KeyEvent.KEYCODE_2 -> 3
        KeyEvent.KEYCODE_3 -> 4
        KeyEvent.KEYCODE_4 -> 5
        KeyEvent.KEYCODE_5 -> 6
        KeyEvent.KEYCODE_6 -> 7
        KeyEvent.KEYCODE_7 -> 8
        KeyEvent.KEYCODE_8 -> 9
        KeyEvent.KEYCODE_9 -> 10
        KeyEvent.KEYCODE_0 -> 11
        KeyEvent.KEYCODE_MINUS -> 12
        KeyEvent.KEYCODE_EQUALS -> 13
        KeyEvent.KEYCODE_LEFT_BRACKET -> 26
        KeyEvent.KEYCODE_RIGHT_BRACKET -> 27
        KeyEvent.KEYCODE_BACKSLASH -> 43
        KeyEvent.KEYCODE_SEMICOLON -> 39
        KeyEvent.KEYCODE_APOSTROPHE -> 40
        KeyEvent.KEYCODE_GRAVE -> 41
        KeyEvent.KEYCODE_COMMA -> 51
        KeyEvent.KEYCODE_PERIOD -> 52
        KeyEvent.KEYCODE_SLASH -> 53
        else -> null
    }
}

private class InteractiveDesktopSurfaceView(
    context: Context,
    private val onKeyEventCallback: (linuxKeycode: Int, pressed: Boolean, shiftMod: Boolean) -> Unit
) : SurfaceView(context) {

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
            InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_ACTION_NONE

        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                if (!text.isNullOrEmpty()) {
                    for (i in 0 until text.length) {
                        typeChar(text[i])
                    }
                }
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                val count = if (beforeLength > 0) beforeLength else 1
                for (i in 0 until count) {
                    onKeyEventCallback(14, true, false)
                    onKeyEventCallback(14, false, false)
                }
                return true
            }

            override fun performEditorAction(actionCode: Int): Boolean {
                onKeyEventCallback(28, true, false)
                onKeyEventCallback(28, false, false)
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                handleAndroidKeyEvent(event)
                return true
            }
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return super.onKeyDown(keyCode, event)
        }
        return handleAndroidKeyEvent(event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return super.onKeyUp(keyCode, event)
        }
        return handleAndroidKeyEvent(event)
    }

    fun showKeyboard() {
        requestFocus()
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
    }

    fun toggleKeyboard() {
        requestFocus()
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.toggleSoftInputFromWindow(windowToken, InputMethodManager.SHOW_FORCED, 0)
    }

    private fun typeChar(ch: Char) {
        val mapped = mapCharToLinuxKey(ch) ?: return
        onKeyEventCallback(mapped.keycode, true, mapped.shift)
        onKeyEventCallback(mapped.keycode, false, mapped.shift)
    }

    private fun handleAndroidKeyEvent(event: KeyEvent): Boolean {
        val unicode = event.unicodeChar
        if (unicode > 0 && event.keyCode != KeyEvent.KEYCODE_DEL && event.keyCode != KeyEvent.KEYCODE_ENTER) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                typeChar(unicode.toChar())
            }
            return true
        }

        val linuxCode = mapAndroidKeyCodeToLinux(event.keyCode) ?: return false
        val shift = event.isShiftPressed
        when (event.action) {
            KeyEvent.ACTION_DOWN -> onKeyEventCallback(linuxCode, true, shift)
            KeyEvent.ACTION_UP -> onKeyEventCallback(linuxCode, false, shift)
        }
        return true
    }
}

@SuppressLint("ClickableViewAccessibility")
@Composable
fun DesktopSurfaceView(
    modifier: Modifier = Modifier,
    keyboardToggleCount: Int = 0,
    onSurfaceCreated: (Surface) -> Unit,
    onSurfaceDestroyed: () -> Unit,
    onPointerEvent: (action: Int, normX: Float, normY: Float) -> Unit,
    onKeyEvent: (linuxKeycode: Int, pressed: Boolean, shiftMod: Boolean) -> Unit
) {
    AndroidView(
        modifier = modifier.fillMaxSize(),
        factory = { context ->
            InteractiveDesktopSurfaceView(context, onKeyEvent).apply {
                tag = 0
                val mainHandler = Handler(Looper.getMainLooper())
                val touchSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
                val longPressTimeout = 450L

                var downRawX = 0f
                var downRawY = 0f
                var downNormX = 0f
                var downNormY = 0f
                var isDragging = false
                var longPressFired = false

                val longPressRunnable = Runnable {
                    longPressFired = true
                    performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                    // Native Wayland right-click (LINUX_BTN_RIGHT press + release)
                    onPointerEvent(3, downNormX, downNormY)
                    onPointerEvent(4, downNormX, downNormY)
                }

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
                    if (w <= 0f || h <= 0f) return@setOnTouchListener true

                    val normX = (event.x / w).coerceIn(0f, 1f)
                    val normY = (event.y / h).coerceIn(0f, 1f)

                    when (event.actionMasked) {
                        MotionEvent.ACTION_DOWN -> {
                            downRawX = event.x
                            downRawY = event.y
                            downNormX = normX
                            downNormY = normY
                            isDragging = false
                            longPressFired = false
                            onPointerEvent(2, normX, normY)
                            mainHandler.removeCallbacks(longPressRunnable)
                            mainHandler.postDelayed(longPressRunnable, longPressTimeout)
                        }

                        MotionEvent.ACTION_MOVE -> {
                            val dist = hypot(event.x - downRawX, event.y - downRawY)
                            if (!longPressFired) {
                                if (!isDragging && dist > touchSlop) {
                                    isDragging = true
                                    mainHandler.removeCallbacks(longPressRunnable)
                                    // Begin left-button drag
                                    onPointerEvent(0, downNormX, downNormY)
                                }
                                onPointerEvent(2, normX, normY)
                            }
                        }

                        MotionEvent.ACTION_UP -> {
                            mainHandler.removeCallbacks(longPressRunnable)
                            if (!longPressFired) {
                                if (isDragging) {
                                    onPointerEvent(2, normX, normY)
                                    onPointerEvent(1, normX, normY)
                                } else {
                                    // Short tap -> Left click (press + release)
                                    onPointerEvent(0, normX, normY)
                                    onPointerEvent(1, normX, normY)
                                    // Automatically open soft keyboard when tapping inside the desktop workspace (below top bar)
                                    if (normY > 0.06f) {
                                        showKeyboard()
                                    }
                                }
                            }
                            isDragging = false
                        }

                        MotionEvent.ACTION_CANCEL -> {
                            mainHandler.removeCallbacks(longPressRunnable)
                            if (isDragging) {
                                onPointerEvent(1, normX, normY)
                            }
                            isDragging = false
                        }
                    }
                    true
                }
            }
        },
        update = { surfaceView ->
            val lastCount = surfaceView.tag as? Int ?: 0
            if (keyboardToggleCount != lastCount) {
                surfaceView.tag = keyboardToggleCount
                surfaceView.toggleKeyboard()
            }
        }
    )
}



