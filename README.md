# MASMZE-3D — Port Android

Port completo de MASMZE-3D (juego de laberinto 3D original escrito en MASM32/x86 ASM)
para Android, usando **NDK + OpenGL ES 2.0 + Kotlin**.

---

## Arquitectura del port

### ¿Qué cambió respecto al original Windows?

| Componente original | Equivalente Android |
|---|---|
| WinAPI / `WndProc` | `SurfaceHolder.Callback` + hilo NDK |
| OpenGL (legacy `glBegin/glEnd`) | **OpenGL ES 2.0** con shaders GLSL ES |
| `GlobalAlloc` / `GlobalFree` | `malloc` / `free` estándar C |
| Windows Registry | `internal storage` (`filesDir`) |
| `settings.ini` / `GetPrivateProfileString` | `SharedPreferences` vía JNI |
| WinMM / DirectInput joystick | **Joystick virtual táctil** en `GameOverlayView` |
| OpenAL32 (DLL Windows) | **OpenSL ES** (NDK nativo Android) |
| `MSG` loop / `WM_PAINT` | Hilo de render dedicado con `pthread` |
| `mmsystem.h` / `GetTickCount` | `clock_gettime(CLOCK_MONOTONIC)` |
| `VK_*` keycodes | Callbacks JNI desde overlay táctil |
| `SetFullscreen` / `WS_POPUP` | `FLAG_FULLSCREEN` + `SYSTEM_UI_FLAG_IMMERSIVE_STICKY` |

### Lógica del juego — **sin cambios**

Todo el código de lógica de juego se portó **línea a línea** desde el original:

- `GenerateMaze()` — Recursive Backtracker idéntico
- `CheckBlocked()` — Raycast de línea de visión idéntico
- `CollidePlayerWall()` — Colisión AABB idéntica
- `Control()` — Física de movimiento idéntica
- `Lerp()`, `Clamp()`, `DistanceToSqr()` — Matemáticas idénticas
- Todos los estados de entidades (Wmblyk, Kubale, WB, EBD, etc.)
- Todos los textos y subtítulos del juego

---

## Estructura del proyecto

```
masmze-android/
├── app/
│   ├── build.gradle               ← ABIs, CMake, ProGuard
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── jni/
│       │   ├── masmze_android.c   ← TODO el código C portado
│       │   └── CMakeLists.txt     ← Build NDK con -O3 -ffast-math
│       ├── java/com/greatcorn/masmze/
│       │   ├── MainActivity.kt    ← Ciclo de vida + Surface
│       │   ├── MasmzeLib.kt       ← Bindings JNI
│       │   └── GameOverlayView.kt ← Joystick virtual + botones
│       └── res/layout/
│           └── activity_main.xml
├── build.gradle
├── settings.gradle
└── gradle.properties
```

---

## Optimizaciones aplicadas

### NDK / C
- `-O3` — optimización máxima del compilador
- `-ffast-math` — equivalente al modo FPU `0C00h` del MASM original
- `-funroll-loops` — desenrolla los bucles del generador y el raycast
- ARM NEON activado en `armeabi-v7a` (`-mfpu=neon`)
- AArch64 SIMD en `arm64-v8a` (`-march=armv8-a+simd`)
- `malloc/free` en lugar de `GlobalAlloc/GlobalFree` (evita overhead de Win32)

### Render (OpenGL ES 2.0)
- Eliminados todos los `glBegin/glEnd` (deprecated, sin HW accel en ES)
- Shaders GLSL ES compilados una sola vez al inicio
- Hilo de render separado del UI thread (sin bloqueos de `WM_PAINT`)
- `eglSwapInterval(0)` implícito — sin vsync forzado en el render loop
- Delta time con `CLOCK_MONOTONIC` (alta resolución, sin drift)

### Controles táctiles
- Joystick flotante: la base aparece donde pones el dedo (no en posición fija)
- Joystick derecho: drag libre en toda la mitad derecha de pantalla
- Multi-touch: cada pointer tiene su propio slot (`MAX_TOUCHES = 10`)
- Sensibilidad configurable con `camTurnSpeed`

### APK
- `abiFilters "arm64-v8a", "armeabi-v7a"` — cubre >98% de dispositivos Android
- ProGuard habilitado en release
- Strip de símbolos debug del `.so`
- `minSdk 21` — Android 5.0+ (OpenGL ES 2.0 garantizado)

---

## Cómo compilar

### Requisitos
- Android Studio Hedgehog o superior
- NDK r25c o superior
- CMake 3.22+

### Pasos
```bash
# 1. Abrir el proyecto en Android Studio
#    File → Open → masmze-android/

# 2. Seleccionar Build Variant: release

# 3. Compilar
./gradlew assembleRelease

# El APK estará en:
# app/build/outputs/apk/release/app-release.apk
```

### Compilar solo la biblioteca nativa
```bash
cd app/src/main/jni
cmake -B build -DANDROID_ABI=arm64-v8a \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_PLATFORM=android-21 \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## Assets (.gct / .gcm / .gcf)

El juego original usa formatos de textura y fuente propios (`.gct`, `.gcm`, `.gcf`).
Coloca los assets originales en:

```
app/src/main/assets/
├── GFX/
│   ├── bricks.gct
│   ├── font/font.gcf
│   └── ...
└── SFX/
    ├── amb.gcm
    └── ...
```

El `AAssetManager` ya está inicializado en `nativeInit()` y disponible en C como `assetMgr`.

---

## Licencia

GPL v3+ — igual que el proyecto original de GreatCorn.
