package com.greatcorn.masmze

import android.content.Context
import android.os.Bundle
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import kotlin.math.*

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var surfaceView: SurfaceView
    private lateinit var overlayView: GameOverlayView
    private var surfaceReady = false

    // ── Lifecycle ──────────────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Pantalla completa, mantener pantalla encendida
        window.addFlags(
            WindowManager.LayoutParams.FLAG_FULLSCREEN or
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        )
        // Inmersive sticky (barras de sistema ocultas)
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        )

        setContentView(R.layout.activity_main)
        surfaceView = findViewById(R.id.gameSurface)
        overlayView = findViewById(R.id.gameOverlay)

        surfaceView.holder.addCallback(this)
        overlayView.onButtonEvent = ::handleButton
        overlayView.onMoveJoy     = ::handleMoveJoy
        overlayView.onCamJoy      = ::handleCamJoy

        System.loadLibrary("masmze")
    }

    override fun onResume() {
        super.onResume()
        if (surfaceReady) MasmzeLib.nativeStart()
    }

    override fun onPause() {
        super.onPause()
        if (surfaceReady) {
            MasmzeLib.nativeSave()
            MasmzeLib.nativeStop()
        }
    }

    // ── SurfaceHolder.Callback ─────────────────────────────────────────────────

    override fun surfaceCreated(holder: SurfaceHolder) {
        MasmzeLib.nativeInit(
            holder.surface,
            assets,
            filesDir.absolutePath
        )
        surfaceReady = true
        MasmzeLib.nativeStart()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {
        // El contexto EGL ya se creó con el tamaño correcto en nativeInit
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        MasmzeLib.nativeStop()
        surfaceReady = false
    }

    // ── Control callbacks ──────────────────────────────────────────────────────

    private fun handleButton(btn: Int, pressed: Boolean) {
        MasmzeLib.nativeButton(btn, pressed)
    }

    private fun handleMoveJoy(x: Float, y: Float) {
        MasmzeLib.nativeSetMoveJoy(x, y)
    }

    private fun handleCamJoy(dx: Float, dy: Float) {
        MasmzeLib.nativeSetCamJoy(dx, dy)
    }
}
