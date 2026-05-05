package com.greatcorn.masmze

import android.content.res.AssetManager
import android.view.Surface

/**
 * Bindings JNI a la biblioteca nativa masmze.
 * Todos los métodos corresponden a funciones en masmze_android.c.
 */
object MasmzeLib {

    // ── Ciclo de vida ──────────────────────────────────────────────────────────

    /** Inicializa EGL, carga assets y save. Llamar cuando la Surface esté lista. */
    external fun nativeInit(surface: Surface, assetManager: AssetManager, dataPath: String)

    /** Arranca el hilo de render. */
    external fun nativeStart()

    /** Detiene el hilo de render de forma ordenada. */
    external fun nativeStop()

    // ── Controles ──────────────────────────────────────────────────────────────

    /**
     * Joystick de movimiento (izquierdo).
     * @param x  -1..1  (izquierda/derecha)
     * @param y  -1..1  (atrás/adelante)
     */
    external fun nativeSetMoveJoy(x: Float, y: Float)

    /**
     * Joystick de cámara (derecho).
     * @param dx delta horizontal (yaw)
     * @param dy delta vertical (pitch)
     */
    external fun nativeSetCamJoy(dx: Float, dy: Float)

    /**
     * Botón de acción.
     * @param btn    0=Acción, 1=Agacharse, 2=Menú, 3=Glifo
     * @param pressed true=pulsado, false=soltado
     */
    external fun nativeButton(btn: Int, pressed: Boolean)

    // ── Persistencia ──────────────────────────────────────────────────────────

    /** Guarda el estado actual en internal storage. */
    external fun nativeSave()

    // ── Estado (para la UI superpuesta) ───────────────────────────────────────

    /** Devuelve el nivel actual del laberinto. */
    external fun nativeGetLevel(): Int

    /** Devuelve la cantidad de glifos del jugador. */
    external fun nativeGetGlyphs(): Int
}
