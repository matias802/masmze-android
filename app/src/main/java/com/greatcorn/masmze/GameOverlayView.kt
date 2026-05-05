package com.greatcorn.masmze

import android.content.Context
import android.graphics.*
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.*

/**
 * GameOverlayView
 * ─────────────────────────────────────────────────────────────────────────────
 * Superposición táctil que dibuja y procesa:
 *   • Joystick izquierdo  → movimiento del jugador
 *   • Zona táctil derecha → rotación de cámara (drag libre)
 *   • Botones: Acción (A), Agacharse (B), Menú (≡), Glifo (G)
 *
 * El diseño imita el HUD sobrio del original:
 *   – fondo semi-transparente en los controles
 *   – texto en mayúsculas, tipografía monoespaciada
 */
class GameOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    // Callbacks hacia Activity / NDK
    var onMoveJoy:     ((Float, Float) -> Unit)? = null
    var onCamJoy:      ((Float, Float) -> Unit)? = null
    var onButtonEvent: ((Int, Boolean) -> Unit)? = null

    // ── Dimensiones (relativas, se recalculan en onSizeChanged) ───────────────
    private var joyRadius    = 0f   // radio del joystick izquierdo (px)
    private var joyBaseX     = 0f   // centro base del joystick
    private var joyBaseY     = 0f
    private var joyThumbX    = 0f   // posición actual del pulgar
    private var joyThumbY    = 0f
    private var joyPointerID = -1   // pointer que controla el joystick

    private var camPointerID = -1   // pointer para la cámara
    private var camLastX     = 0f
    private var camLastY     = 0f

    // Botones: rect + estado
    data class Btn(val rect: RectF, val label: String, val id: Int, var pressed: Boolean = false)
    private val buttons = mutableListOf<Btn>()
    private val btnPointers = mutableMapOf<Int, Int>() // pointerID → btnId

    // ── Paints ─────────────────────────────────────────────────────────────────
    private val paintJoyBase = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(60, 255, 255, 255); style = Paint.Style.FILL
    }
    private val paintJoyRing = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(120, 255, 255, 255); style = Paint.Style.STROKE; strokeWidth = 3f
    }
    private val paintJoyThumb = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(180, 220, 220, 220); style = Paint.Style.FILL
    }
    private val paintBtnNormal = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(80, 200, 200, 200); style = Paint.Style.FILL
    }
    private val paintBtnPressed = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(160, 255, 255, 255); style = Paint.Style.FILL
    }
    private val paintBtnBorder = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(160, 255, 255, 255); style = Paint.Style.STROKE; strokeWidth = 2f
    }
    private val paintText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textAlign = Paint.Align.CENTER
        typeface = Typeface.MONOSPACE
    }

    // ── Layout ─────────────────────────────────────────────────────────────────
    override fun onSizeChanged(w: Int, h: Int, oldW: Int, oldH: Int) {
        super.onSizeChanged(w, h, oldW, oldH)

        // Joystick izquierdo (esquina inferior izquierda)
        joyRadius = h * 0.12f
        joyBaseX  = w * 0.12f
        joyBaseY  = h * 0.78f
        joyThumbX = joyBaseX
        joyThumbY = joyBaseY

        // Botones (esquina inferior derecha)
        buttons.clear()
        val bSize = h * 0.09f
        val bMarg = h * 0.015f
        val bRight = w - bMarg
        val bBottom= h * 0.92f
        paintText.textSize = bSize * 0.38f

        // Diseño en diamante (estilo PlayStation)
        //        [G]
        //    [B]     [A]
        //        [≡]
        val cx = bRight - bSize * 1.5f
        val cy = bBottom - bSize * 1.5f
        buttons += Btn(RectF(cx-bSize/2, cy-bSize*1.5f-bSize/2, cx+bSize/2, cy-bSize*1.5f+bSize/2), "G",  3) // arriba
        buttons += Btn(RectF(cx-bSize*1.5f-bSize/2, cy-bSize/2, cx-bSize*1.5f+bSize/2, cy+bSize/2), "B",  1) // izquierda
        buttons += Btn(RectF(cx+bSize*1.5f-bSize/2, cy-bSize/2, cx+bSize*1.5f+bSize/2, cy+bSize/2), "A",  0) // derecha
        buttons += Btn(RectF(cx-bSize/2, cy+bSize*1.5f-bSize/2, cx+bSize/2, cy+bSize*1.5f+bSize/2), "≡",  2) // abajo
    }

    // ── Dibujo ─────────────────────────────────────────────────────────────────
    override fun onDraw(canvas: Canvas) {
        // Joystick base
        canvas.drawCircle(joyBaseX, joyBaseY, joyRadius, paintJoyBase)
        canvas.drawCircle(joyBaseX, joyBaseY, joyRadius, paintJoyRing)
        // Pulgar
        canvas.drawCircle(joyThumbX, joyThumbY, joyRadius * 0.38f, paintJoyThumb)

        // Botones
        for (btn in buttons) {
            val cx = btn.rect.centerX(); val cy = btn.rect.centerY()
            val r  = btn.rect.width() * 0.5f
            canvas.drawCircle(cx, cy, r, if (btn.pressed) paintBtnPressed else paintBtnNormal)
            canvas.drawCircle(cx, cy, r, paintBtnBorder)
            canvas.drawText(btn.label, cx, cy + paintText.textSize * 0.38f, paintText)
        }

        invalidate() // render continuo para animación del joystick
    }

    // ── Touch ──────────────────────────────────────────────────────────────────
    override fun onTouchEvent(event: MotionEvent): Boolean {
        val action    = event.actionMasked
        val ptrIndex  = event.actionIndex
        val pointerId = event.getPointerId(ptrIndex)

        when (action) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> {
                val x = event.getX(ptrIndex)
                val y = event.getY(ptrIndex)
                assignPointer(pointerId, x, y)
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    val pid = event.getPointerId(i)
                    val x   = event.getX(i)
                    val y   = event.getY(i)
                    movePointer(pid, x, y)
                }
            }
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP,
            MotionEvent.ACTION_CANCEL -> {
                releasePointer(pointerId)
            }
        }
        return true
    }

    private fun assignPointer(pid: Int, x: Float, y: Float) {
        // ¿Botón?
        for (btn in buttons) {
            val cx = btn.rect.centerX(); val cy = btn.rect.centerY()
            val r  = btn.rect.width() * 0.5f
            if (hypot(x - cx, y - cy) <= r) {
                btn.pressed = true
                btnPointers[pid] = btn.id
                onButtonEvent?.invoke(btn.id, true)
                return
            }
        }

        // ¿Zona del joystick izquierdo? (mitad izquierda de pantalla)
        if (x < width * 0.5f && joyPointerID == -1) {
            joyPointerID = pid
            joyBaseX = x; joyBaseY = y   // joystick flotante: base sigue al dedo inicial
            joyThumbX = x; joyThumbY = y
            onMoveJoy?.invoke(0f, 0f)
            return
        }

        // Zona de cámara (mitad derecha)
        if (x >= width * 0.5f && camPointerID == -1) {
            camPointerID = pid
            camLastX = x; camLastY = y
        }
    }

    private fun movePointer(pid: Int, x: Float, y: Float) {
        // Botón — no hay movimiento relevante

        // Joystick
        if (pid == joyPointerID) {
            val dx = x - joyBaseX
            val dy = y - joyBaseY
            val dist = hypot(dx, dy)
            val clamped = joyRadius
            val nx = if (dist > clamped) dx / dist * clamped else dx
            val ny = if (dist > clamped) dy / dist * clamped else dy
            joyThumbX = joyBaseX + nx
            joyThumbY = joyBaseY + ny
            onMoveJoy?.invoke(nx / joyRadius, ny / joyRadius)
            return
        }

        // Cámara
        if (pid == camPointerID) {
            val sensitivity = 0.004f
            val ddx = (x - camLastX) * sensitivity
            val ddy = (y - camLastY) * sensitivity
            camLastX = x; camLastY = y
            onCamJoy?.invoke(ddx, ddy)
        }
    }

    private fun releasePointer(pid: Int) {
        // Botón
        btnPointers[pid]?.let { btnId ->
            buttons.find { it.id == btnId }?.pressed = false
            onButtonEvent?.invoke(btnId, false)
            btnPointers.remove(pid)
            return
        }

        if (pid == joyPointerID) {
            joyPointerID = -1
            joyThumbX = joyBaseX
            joyThumbY = joyBaseY
            onMoveJoy?.invoke(0f, 0f)
        }
        if (pid == camPointerID) {
            camPointerID = -1
            onCamJoy?.invoke(0f, 0f)
        }
    }
}
