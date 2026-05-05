cmake_minimum_required(VERSION 3.22.1)
project(masmze C)

# ── Ajustes de optimización ──────────────────────────────────────────────────
# -O3          : optimización máxima
# -ffast-math  : matemáticas rápidas (igual que el modo FPU del MASM original)
# -funroll-loops: desenrollar bucles del generador y raycast
# -fvisibility=hidden: reduce tamaño de .so
# -DNDEBUG     : desactiva asserts en release
set(CMAKE_C_FLAGS_RELEASE
    "-O3 -ffast-math -funroll-loops -fvisibility=hidden -DNDEBUG")

# ARM NEON para aceleración SIMD en cálculos de cámara/colisión
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    add_compile_options(-mfpu=neon -mfloat-abi=softfp)
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    # NEON siempre disponible en AArch64
    add_compile_options(-march=armv8-a+simd)
endif()

# ── Biblioteca nativa ─────────────────────────────────────────────────────────
add_library(masmze SHARED
    masmze_android.c
)

# ── Dependencias del sistema ──────────────────────────────────────────────────
find_library(log-lib     log)
find_library(android-lib android)
find_library(egl-lib     EGL)
find_library(gles2-lib   GLESv2)
find_library(opensl-lib  OpenSLES)

target_link_libraries(masmze
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles2-lib}
    ${opensl-lib}
    m          # libm — math.h
)

target_include_directories(masmze PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
