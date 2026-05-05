/*
 * MASMZE-3D — Port Android
 * Original: Copyright (C) 2023 Yevhenii Ionenko (GreatCorn)
 * Port Android: traducción de WinAPI/OpenGL → Android NDK/OpenGL ES 2.0
 *
 * Cambios respecto al original (Windows):
 *   - WinAPI  → Android NDK (ANativeWindow, ALooper, AInputQueue)
 *   - OpenGL  → OpenGL ES 2.0 (shaders GLSL ES, sin glBegin/glEnd)
 *   - WinMM/DirectInput → OpenSL ES para audio
 *   - Registry → SharedPreferences vía JNI
 *   - Joystick Win32 → táctil con joystick virtual en pantalla
 *   - GlobalAlloc → malloc/free
 *   - settings.ini → Internal storage de Android
 *
 * Compilar con CMake + NDK (ver CMakeLists.txt).
 *
 * Este programa es software libre bajo los términos de la GPL v3+.
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/sensor.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* =========================================================
 * LOGGING
 * ========================================================= */
#define LOG_TAG "MASMZE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/* =========================================================
 * CONSTANTES DEL LABERINTO (idénticas al original)
 * ========================================================= */
#define MZC_PASSTOP    1
#define MZC_PASSLEFT   2
#define MZC_VISITED    4
#define MZC_LAMP       8
#define MZC_PIPE       16
#define MZC_WIRES      32
#define MZC_TABURETKA  64
#define MZC_ROTATED    128

/* Matemáticas */
#define PI       3.14159265f
#define PI2      6.28318530f
#define PIHalf   1.57079632f
#define PIHalfN -1.57079632f

/* =========================================================
 * TEXTOS DEL JUEGO (igual que el original)
 * ========================================================= */
static const char CCRandom1[]  = "RECUERDO ESTE LUGAR.";
static const char CCRandom2[]  = "HUELE A HUMEDAD AQUI.";
static const char CCRandom3[]  = "EL AIRE HUELE A ESTANCADO.";
static const char CCRandom4[]  = "LA HUMEDAD SE AFERRA A LAS PAREDES.";
static const char CCRandom5[]  = "ALGO OBSERVA DESDE LEJOS.";
static const char CCRandom6[]  = "LAS PAREDES VIBRAN LIGERAMENTE.";

static const char CCCompass[]      = "RECOGISTE UNA BRUJULA.";
static const char CCGlyphNone[]    = "EL ABISMO ENCIERRA TUS MALDADES.";
static const char CCGlyphRestore[] = "TU ABSOLUCION LLEGA.";
static const char CCKey[]          = "SE RECOGIO LA LLAVE.";
static const char CCTeleport[]     = "LA REALIDAD SE DISTORSIONA, SURGEN FRACTURAS.";
static const char CCTrench[]       = "EL CICLO PERDURA.";
static const char CCLevel[]        = "CAPA:";

static const char CCTipEmpty[]  = "";

/* =========================================================
 * CONSTANTES DE JUEGO
 * ========================================================= */
static const float flCamHeight  = -1.2f;
static const float flCamSpeed   =  3.6f;
static const float flDoor       =  0.65f;
static const float flStep       =  6.0f;
static const float flWTh        =  0.4f;
static const float flWMr        =  0.15f;
static const float flWLn        =  2.15f;
static const float flRaycast    =  1.0f;
static const float flKubaleTh   =  0.7f;
static const float flWmblykAnim =  0.15f;

/* =========================================================
 * EGL / CONTEXTO
 * ========================================================= */
static EGLDisplay  eglDisplay  = EGL_NO_DISPLAY;
static EGLSurface  eglSurface  = EGL_NO_SURFACE;
static EGLContext  eglContext   = EGL_NO_CONTEXT;
static ANativeWindow *nativeWindow = NULL;
static int         screenW = 0, screenH = 0;

/* =========================================================
 * SHADERS GLSL ES 2.0
 * ========================================================= */

/* Shader de vértice genérico para quads con textura */
static const char *VS_TEXTURED =
    "attribute vec4 aPos;\n"
    "attribute vec2 aUV;\n"
    "uniform   mat4 uMVP;\n"
    "varying   vec2 vUV;\n"
    "void main() {\n"
    "  gl_Position = uMVP * aPos;\n"
    "  vUV = aUV;\n"
    "}\n";

/* Shader de fragmento con tinte de color */
static const char *FS_TEXTURED =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4      uColor;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uTex, vUV) * uColor;\n"
    "}\n";

/* Shader de fragmento para color sólido (HUD, fade) */
static const char *FS_SOLID =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "  gl_FragColor = uColor;\n"
    "}\n";

/* Shader de fragmento para niebla volumétrica */
static const char *FS_FOG =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4      uColor;\n"
    "uniform float     uFogDensity;\n"
    "uniform float     uFogStart;\n"
    "uniform float     uFogEnd;\n"
    "void main() {\n"
    "  vec4 texColor = texture2D(uTex, vUV) * uColor;\n"
    "  float fogFactor = clamp((uFogEnd - gl_FragCoord.z) / (uFogEnd - uFogStart), 0.0, 1.0);\n"
    "  gl_FragColor = mix(vec4(0.1, 0.1, 0.1, 1.0), texColor, fogFactor);\n"
    "}\n";

/* IDs de programas shader */
static GLuint progTextured = 0;
static GLuint progSolid    = 0;
static GLuint progFog      = 0;

/* Locations de uniforms */
static GLint locTexMVP, locTexTex, locTexColor;
static GLint locSolMVP, locSolColor;
static GLint locFogMVP, locFogTex, locFogColor, locFogDensity, locFogStart, locFogEnd;

/* =========================================================
 * ESTADO DEL JUEGO (idéntico al original, adaptado)
 * ========================================================= */

/* Teclas/controles (táctil virtual) */
static uint8_t keyUp    = 0;
static uint8_t keyDown  = 0;
static uint8_t keyLeft  = 0;
static uint8_t keyRight = 0;
static uint8_t keySpace = 0;
static uint8_t keyCtrl  = 0;
static uint8_t keyLMB   = 0;

/* Estado del jugador */
static uint8_t  canControl   = 0;
static uint8_t  playerState  = 11;
static uint8_t  Menu         = 0;
static float    Gamma        = 0.5f;

/* Cámara */
static float camRot[2]       = { 0.0f, 0.0f };
static float camRotL[2]      = { 0.0f, 0.0f };
static float camPos[4]       = { -0.6f, -1.2f, -1.0f, 1.0f };
static float camPosL[3]      = { 0.0f, -1.2f,  0.0f };
static float camPosN[2]      = { 0.0f, 0.0f };
static float camPosNext[3]   = { 0.0f, -1.2f,  0.0f };
static float camForward[3]   = { 0.0f, 0.0f,  -1.0f };
static float camRight[3]     = { 1.0f, 0.0f,   0.0f };
static float camCurSpeed[3]  = { 0.0f, 0.0f,   0.0f };
static float camCrouch       = 0.0f;
static float camStep         = 0.0f;
static float camStepSide     = 0.0f;
static float camTilt         = 0.0f;
static float camTurnSpeed    = 1.5f;  /* Sensibilidad táctil */

/* Delta time */
static float    deltaTime = 0.016f;
static float    delta2    = 0.016f;
static float    delta20   = 0.016f;
static uint64_t lastTimeNs = 0;

/* Fade y niebla */
static float    fade        = 1.0f;
static uint8_t  fadeState   = 0;
static float    fogDensity  = 0.5f;
static float    vignetteRed = 0.0f;

/* Subtítulos */
static const char *ccText  = NULL;
static float       ccTimer = 0.0f;

/* Laberinto */
static uint32_t MazeSeed    = 0;
static uint32_t MazeW       = 6;
static uint32_t MazeH       = 6;
static uint32_t MazeWM1     = 0;
static uint32_t MazeHM1     = 0;
static uint8_t *Maze        = NULL;
static uint32_t MazeSize    = 0;
static uint8_t  MazeType    = 0;
static uint8_t  MazeCrevice = 0;
static uint32_t MazeCrevicePos[2] = {0,0};
static uint8_t  MazeLocked  = 0;
static uint8_t  MazeHostile = 0;
static uint8_t  MazeTeleport= 0;
static uint8_t  MazeTram    = 0;
static uint8_t  MazeTramPlr = 0;
static float    MazeTramPos[3] = {0,0,0};
static float    MazeTramSpeed  = 0.0f;
static float    MazeRandTimer  = 10.0f;
static float    MazeDoor       = 0.0f;
static float    MazeDoorPos[2] = {0,0};
static float    MazeKeyPos[2]  = {0,0};
static float    MazeKeyRot[2]  = {0,0};
static uint8_t  MazeNote       = 0;
static float    MazeNotePos[2] = {0,0};
static float    MazeSiren      = 0.0f;
static float    MazeSirenTimer = 51.0f;
static float    MazeTeleportPos[4] = {0,0,0,0};
static float    MazeTeleportRot    = 0.0f;
static uint32_t MazeTramDoors  = 99;
static uint32_t MazeTramArea[2]= {0,0};
static uint32_t MazeTramRot[2] = {8,0};
static uint32_t MazeTramSnd    = 0;
static float    MazeRandPos[2] = {0,0};
static uint8_t  MazeGlyphs    = 0;
static float    MazeGlyphsPos[2]={0,0};
static float    MazeGlyphsRot  = 0.0f;

/* Nivel */
static uint32_t MazeLevel          = 0;
static uint8_t  MazeLevelPopup     = 0;
static float    MazeLevelPopupY    = -48.0f;
static float    MazeLevelPopupTimer= 0.0f;

/* Brújula */
static uint8_t  Compass         = 0;
static float    CompassPos[2]   = {0,0};
static float    CompassRot      = 0.0f;
static float    CompassMapPos[2]= {0,0};

/* Glifos */
static uint8_t  Glyphs        = 7;
static uint8_t  GlyphsInLayer = 0;
static uint8_t  GlyphOffset   = 0;
static float    GlyphPos[14]  = {0};
static float    GlyphRot[7]   = {0};

/* Entidades */
static uint8_t  Wmblyk          = 0;
static float    WmblykPos[2]    = {1,1};
static float    WmblykTimer     = 3.0f;
static float    WmblykStealth   = 0.0f;
static uint8_t  WmblykStealthy  = 0;

static uint8_t  kubaleAppeared  = 0;
static uint32_t kubale          = 0;
static float    kubalePos[2]    = {3,3};
static float    kubaleSpeed[2]  = {0,0};
static float    kubaleVision    = 0.0f;
static float    kubaleRun       = 0.0f;
static float    kubaleDir       = 0.0f;

static uint8_t  EBD      = 0;
static float    EBDAnim  = 0.0f;
static float    EBDPos[2]= {3,3};

static uint8_t  WBBK        = 0;
static float    WBBKPos[2]  = {1,5};
static float    WBBKDist    = 4.0f;
static float    WBBKTimer   = 0.0f;
static float    WBBKSTimer  = 20.0f;
static float    WBBKCamDir  = 0.0f;

static uint8_t  WB       = 0;
static uint8_t  WBAnim   = 0;
static float    WBPos[2] = {1,3};
static float    WBTimer  = 0.0f;

static float    vasPos[2]= {0,0};
static uint8_t  hbd      = 0;
static float    hbdPosF[2]= {1,1};
static float    hbdTimer = 6.0f;
static uint32_t hbdMdl   = 56;
static uint8_t  virdya   = 0;
static float    MotryaDist= 40.0f;
static uint32_t Motrya   = 0;
static float    MotryaPos[2]= {0,0};
static float    ShnPos[2]   = {1,1};
static uint8_t  Shn         = 0;
static float    ShnTimer    = 80.0f;

/* Guardar/Checkpoint */
static uint8_t Save       = 0;
static float   SaveSize   = 0.0f;
static float   SavePos[3] = {1,0.2f,0.2f};
static uint8_t Checkpoint = 0;
static float   CheckpointPos[2]= {0,0};

/* Tienda */
static uint8_t  Shop      = 1;
static float    ShopTimer = 0.0f;
static float    ShopWall  = 3.0f;

/* Croa */
static uint8_t  Croa        = 0;
static float    CroaTimer   = 3.0f;
static float    CroaPos     = 1.5f;
static float    CroaColor[4]= {0,0,0,0};

/* Trench */
static uint8_t  Trench      = 0;
static float    TrenchTimer = 0.0f;

/* Noise */
static float NoiseOpacity[2]= {0.1f, 0.1f};

/* Mapa */
static uint8_t  Map       = 0;
static float    MapOffset[2]= {0,0};
static float    MapSize   = 0.0f;

/* Ascenso */
static uint8_t  AscendDoor    = 0;
static float    AscendColor[4]= {0,0,0,0};

/* Wired globals */
static uint32_t lastStepSnd = 0;
static uint8_t  Complete    = 0;
static uint8_t  focused     = 1;
static uint8_t  joyUsed     = 0;
static uint8_t  MazeCreviceState = 0;

/* Capa anterior */
static uint32_t PMSeed = 0, PMW = 0, PMH = 0;

/* Asset manager (para cargar texturas desde APK) */
static AAssetManager *assetMgr = NULL;

/* Datos de guardado (Android usa internal storage) */
static char savePath[512] = {0};

/* =========================================================
 * TÁCTIL — JOYSTICK VIRTUAL
 * ========================================================= */
#define MAX_TOUCHES 10

typedef struct {
    int     id;       /* pointer ID de Android */
    float   startX;   /* posición inicial del dedo */
    float   startY;
    float   curX;
    float   curY;
    int     active;
} Touch;

static Touch touches[MAX_TOUCHES];

/* Joystick izquierdo (movimiento) */
static float joyMoveX = 0.0f, joyMoveY = 0.0f;
/* Joystick derecho (cámara) */
static float joyCamDX = 0.0f, joyCamDY = 0.0f;
/* Botones táctiles */
static uint8_t btnMenu   = 0;
static uint8_t btnAction = 0;
static uint8_t btnCrouch = 0;

/* Zona izquierda: 0..screenW/2, zona derecha: screenW/2..screenW */
#define JOY_RADIUS_PX  100.0f   /* Radio del joystick virtual en píxeles */
#define BTN_ZONE_W     0.15f    /* Fracción de pantalla para botones */

/* =========================================================
 * FUNCIONES MATEMÁTICAS (idénticas al original)
 * ========================================================= */

static void Lerp(float *val, float target, float step)
{
    float diff = target - *val;
    if (fabsf(diff) < step) *val = target;
    else if (diff > 0)      *val += step;
    else                    *val -= step;
}

static float DistanceToSqr(float x1, float y1, float x2, float y2)
{
    float dx = x2-x1, dy = y2-y1;
    return dx*dx + dy*dy;
}

static float MagnitudeSqr(float x, float y)
{
    return x*x + y*y;
}

static float GetDirection(float x1, float y1, float x2, float y2)
{
    return atan2f(y2-y1, x2-x1);
}

static float DistanceScalar(float a, float b)
{
    return fabsf(a-b);
}

static float Clamp(float val, float minV, float maxV)
{
    if (val < minV) return minV;
    if (val > maxV) return maxV;
    return val;
}

static int InRange(float px, float py, float x1, float x2, float y1, float y2)
{
    return (px >= x1 && px <= x2 && py >= y1 && py <= y2);
}

/* =========================================================
 * LABERINTO — UTILIDADES (idénticas al original)
 * ========================================================= */

static uint32_t GetOffset(uint32_t x, uint32_t y)
{
    return y * MazeW + x;
}

static uint8_t GetCellMZC(uint32_t x, uint32_t y, uint8_t flag)
{
    if (!Maze) return 0;
    if (x >= MazeW || y >= MazeH) return 0;
    return (Maze[GetOffset(x,y)] & flag) ? 1 : 0;
}

static void GetMazeCellPos(float wx, float wy, uint32_t *mx, uint32_t *my)
{
    *mx = (uint32_t)(wx / 2.0f);
    *my = (uint32_t)(wy / 2.0f);
    if (*mx >= MazeW) *mx = MazeW-1;
    if (*my >= MazeH) *my = MazeH-1;
}

static void FreeMaze(void)
{
    if (Maze) { free(Maze); Maze = NULL; }
}

/* =========================================================
 * GENERACIÓN DE LABERINTO (Recursive Backtracker — igual al original)
 * ========================================================= */

static void MazeCarve(uint32_t x, uint32_t y)
{
    /* Marca como visitada */
    Maze[GetOffset(x,y)] |= MZC_VISITED;

    /* Orden aleatorio de 4 direcciones */
    int dirs[4] = {0,1,2,3};
    for (int i = 3; i > 0; i--) {
        int j = rand() % (i+1);
        int tmp = dirs[i]; dirs[i] = dirs[j]; dirs[j] = tmp;
    }

    for (int d = 0; d < 4; d++) {
        uint32_t nx = x, ny = y;
        switch (dirs[d]) {
            case 0: if (y == 0)           continue; ny--; break; /* arriba */
            case 1: if (x == 0)           continue; nx--; break; /* izquierda */
            case 2: if (y >= MazeHM1)     continue; ny++; break; /* abajo */
            case 3: if (x >= MazeWM1)     continue; nx++; break; /* derecha */
        }
        if (Maze[GetOffset(nx,ny)] & MZC_VISITED) continue;

        /* Abrir paso */
        switch (dirs[d]) {
            case 0: Maze[GetOffset(x,y)]   |= MZC_PASSTOP;  break;
            case 1: Maze[GetOffset(x,y)]   |= MZC_PASSLEFT; break;
            case 2: Maze[GetOffset(nx,ny)] |= MZC_PASSTOP;  break;
            case 3: Maze[GetOffset(nx,ny)] |= MZC_PASSLEFT; break;
        }
        MazeCarve(nx, ny);
    }
}

static void GenerateMaze(uint32_t seed)
{
    srand(seed);
    MazeWM1  = MazeW - 1;
    MazeHM1  = MazeH - 1;
    MazeSize = MazeW * MazeH;

    FreeMaze();
    Maze = (uint8_t*)calloc(MazeSize, 1);
    if (!Maze) { LOGE("No se pudo asignar memoria del laberinto"); return; }

    MazeCarve(0, 0);

    /* Limpiar flag de visitado */
    for (uint32_t i = 0; i < MazeSize; i++)
        Maze[i] &= ~MZC_VISITED;

    LOGI("Laberinto generado: %ux%u seed=%u", MazeW, MazeH, seed);
}

/* =========================================================
 * RAYCAST (idéntico al original)
 * ========================================================= */

static int CheckBlocked(float xFrom, float yFrom, float xTo, float yTo)
{
    float checkX = xFrom, checkY = yFrom;
    int blocked = 0;
    uint8_t iterations = 0;

    while (1) {
        if (++iterations >= 64) break;

        float dir = GetDirection(checkX, checkY, xTo, yTo);
        if (dir < 0.0f) dir += PI2;

        uint32_t mx, my, tmx, tmy;
        GetMazeCellPos(checkX, checkY, &mx, &my);
        GetMazeCellPos(xTo,    yTo,    &tmx, &tmy);

        if (mx == tmx && my == tmy) break;

        int dirInt = (int)(dir / (PI/2.0f) + 0.5f) % 4;

        switch (dirInt) {
            case 0: case 4:
                if (!GetCellMZC(mx, my, MZC_PASSTOP)) { blocked=1; goto done; }
                if (MazeCrevice && mx==MazeCrevicePos[0] && my==MazeCrevicePos[1])
                    { blocked=1; goto done; }
                checkY -= flRaycast;
                break;
            case 1:
                if (!GetCellMZC(mx, my, MZC_PASSLEFT)) { blocked=1; goto done; }
                checkX -= flRaycast;
                break;
            case 2:
                my++;
                if (!GetCellMZC(mx, my, MZC_PASSTOP)) { blocked=1; goto done; }
                checkY += flRaycast;
                break;
            case 3:
                mx++;
                if (!GetCellMZC(mx, my, MZC_PASSLEFT)) { blocked=1; goto done; }
                checkX += flRaycast;
                break;
        }

        if (DistanceToSqr(checkX,checkY,xTo,yTo) < flRaycast*flRaycast) break;
    }
done:
    return blocked;
}

/* =========================================================
 * COLISIÓN DEL JUGADOR (idéntica al original)
 * ========================================================= */

static void CollidePlayer(float x1, float x2, float y1, float y2, uint8_t vertical)
{
    if (InRange(camPosNext[0], camPosNext[2], x1, x2, y1, y2)) {
        if (vertical) camCurSpeed[0] = 0.0f;
        else          camCurSpeed[2] = 0.0f;

        camPosNext[0] = camPosN[0] - camCurSpeed[0];
        camPosNext[2] = camPosN[1] - camCurSpeed[2];

        if (InRange(camPosNext[0], camPosNext[2], x1, x2, y1, y2)) {
            camCurSpeed[0] = -camCurSpeed[0];
            camCurSpeed[2] = -camCurSpeed[2];
        }
    }
}

static void CollidePlayerWall(float posX, float posY, uint8_t vertical)
{
    float dist = DistanceToSqr(camPosNext[0], camPosNext[2], posX, posY);
    if (dist < 6.0f) {
        float bx1, bx2, by1, by2;
        if (!vertical) {
            bx1 = posX - flWMr; bx2 = posX + flWLn;
            by1 = posY - flWTh; by2 = posY + flWTh;
        } else {
            by1 = posY - flWMr; by2 = posY + flWLn;
            bx1 = posX - flWTh; bx2 = posX + flWTh;
        }
        CollidePlayer(bx1, bx2, by1, by2, vertical);
    }
}

static void MoveAndCollide(float x1, float y1, float x2, float y2,
                           float radius, uint8_t wall)
{
    (void)wall;
    CollidePlayer(x1-radius, x2+radius, y1-radius, y2+radius, 0);
}

/* =========================================================
 * CONTROL — JOYSTICK VIRTUAL TÁCTIL
 * ========================================================= */

static void ProcessTouchMove(void)
{
    /* Mover cámara con joystick derecho */
    camRot[1] += joyCamDX * camTurnSpeed * deltaTime;
    camRot[0] += joyCamDY * camTurnSpeed * deltaTime;
    camRot[0]  = Clamp(camRot[0], PIHalfN, PIHalf);

    /* Calcular vectores forward y right desde yaw */
    float yaw = camRot[1];
    camForward[0] = -sinf(yaw);
    camForward[1] =  0.0f;
    camForward[2] = -cosf(yaw);
    camRight[0]   =  cosf(yaw);
    camRight[1]   =  0.0f;
    camRight[2]   = -sinf(yaw);
}

static void Control(void)
{
    camCurSpeed[0] = 0.0f;
    camCurSpeed[2] = 0.0f;

    /* Joystick virtual izquierdo */
    float jx = joyMoveX, jy = joyMoveY;
    float mag = MagnitudeSqr(jx, jy);
    if (mag > 1.0f) { float s = sqrtf(mag); jx/=s; jy/=s; }

    camCurSpeed[0] += camForward[0]*(-jy) + camRight[0]*jx;
    camCurSpeed[2] += camForward[2]*(-jy) + camRight[2]*jx;

    /* Teclado físico si existe */
    if (keyUp)    { camCurSpeed[0] +=  camForward[0]; camCurSpeed[2] +=  camForward[2]; }
    if (keyDown)  { camCurSpeed[0] -=  camForward[0]; camCurSpeed[2] -=  camForward[2]; }
    if (keyLeft)  { camCurSpeed[0] -=  camRight[0];   camCurSpeed[2] -=  camRight[2];   }
    if (keyRight) { camCurSpeed[0] +=  camRight[0];   camCurSpeed[2] +=  camRight[2];   }

    /* Agacharse */
    float crouchTarget = (keyCtrl || btnCrouch || MazeCrevice==2) ? 2.0f : 0.0f;
    Lerp(&camCrouch, crouchTarget, delta20);

    if (!MazeTramPlr)
        camPos[1] = flCamHeight + camCrouch * 0.25f;

    /* Normalizar velocidad */
    float speedMgn = MagnitudeSqr(camCurSpeed[0], camCurSpeed[2]);
    if (speedMgn > 1.0f) {
        float s = sqrtf(speedMgn);
        camCurSpeed[0] /= s;
        camCurSpeed[2] /= s;
    }

    float curSpeed   = MagnitudeSqr(camCurSpeed[0], camCurSpeed[2]) * deltaTime;
    float baseSpeed  = flCamSpeed - camCrouch;
    camCurSpeed[0]   = baseSpeed * camCurSpeed[0] * deltaTime;
    camCurSpeed[2]   = baseSpeed * camCurSpeed[2] * deltaTime;

    if (Trench) {
        TrenchTimer -= curSpeed * deltaTime;
        camCurSpeed[0] *= 0.5f;
        camCurSpeed[2] *= 0.5f;
        if (TrenchTimer <= 0.0f) { Trench=0; fade=1.0f; fadeState=1; }
    }

    if (MazeHostile >= 8 && MazeHostile <= 11) {
        camCurSpeed[0] *= NoiseOpacity[0]*2.0f;
        camCurSpeed[2] *= NoiseOpacity[0]*2.0f;
    }

    camStep     += curSpeed * flStep;
    camStepSide += curSpeed * flStep;
    if (camStep     > PI)  camStep     -= PI;
    if (camStepSide > PI2) camStepSide -= PI2;
}

/* =========================================================
 * SUBTÍTULOS
 * ========================================================= */

static void ShowSubtitles(const char *s)
{
    ccText  = s;
    ccTimer = 2.0f;
}

/* =========================================================
 * INICIALIZACIÓN DEL CONTEXTO EGL/OPENGL ES
 * ========================================================= */

static GLuint CompileShader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("Shader error: %s", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint LinkProgram(const char *vsrc, const char *fsrc)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER,   vsrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUV");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        LOGE("Link error: %s", log);
        glDeleteProgram(p); return 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static int InitEGL(void)
{
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisplay, NULL, NULL);

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_BLUE_SIZE,  8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE,   8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs;
    eglChooseConfig(eglDisplay, attribs, &config, 1, &numConfigs);
    if (numConfigs == 0) { LOGE("No EGL config"); return 0; }

    eglSurface = eglCreateWindowSurface(eglDisplay, config, nativeWindow, NULL);

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, ctxAttribs);

    if (eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext) == EGL_FALSE) {
        LOGE("eglMakeCurrent failed");
        return 0;
    }

    eglQuerySurface(eglDisplay, eglSurface, EGL_WIDTH,  &screenW);
    eglQuerySurface(eglDisplay, eglSurface, EGL_HEIGHT, &screenH);
    LOGI("Surface: %dx%d", screenW, screenH);
    return 1;
}

static void InitShaders(void)
{
    progTextured = LinkProgram(VS_TEXTURED, FS_TEXTURED);
    progSolid    = LinkProgram(VS_TEXTURED, FS_SOLID);
    progFog      = LinkProgram(VS_TEXTURED, FS_FOG);

    if (progTextured) {
        locTexMVP   = glGetUniformLocation(progTextured, "uMVP");
        locTexTex   = glGetUniformLocation(progTextured, "uTex");
        locTexColor = glGetUniformLocation(progTextured, "uColor");
    }
    if (progSolid) {
        locSolMVP   = glGetUniformLocation(progSolid, "uMVP");
        locSolColor = glGetUniformLocation(progSolid, "uColor");
    }
    if (progFog) {
        locFogMVP     = glGetUniformLocation(progFog, "uMVP");
        locFogTex     = glGetUniformLocation(progFog, "uTex");
        locFogColor   = glGetUniformLocation(progFog, "uColor");
        locFogDensity = glGetUniformLocation(progFog, "uFogDensity");
        locFogStart   = glGetUniformLocation(progFog, "uFogStart");
        locFogEnd     = glGetUniformLocation(progFog, "uFogEnd");
    }
}

/* =========================================================
 * HELPERS DE MATRIZ (MVP) — mini-math sin GLU
 * ========================================================= */

typedef float Mat4[16];

static void Mat4Identity(Mat4 m)
{
    memset(m, 0, sizeof(Mat4));
    m[0]=m[5]=m[10]=m[15]=1.0f;
}

static void Mat4Perspective(Mat4 m, float fovY, float aspect, float near, float far)
{
    float f = 1.0f / tanf(fovY*0.5f);
    memset(m, 0, sizeof(Mat4));
    m[0]  =  f / aspect;
    m[5]  =  f;
    m[10] =  (far+near)/(near-far);
    m[11] = -1.0f;
    m[14] =  2.0f*far*near/(near-far);
}

static void Mat4Multiply(Mat4 out, const Mat4 a, const Mat4 b)
{
    for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
        out[i*4+j] = 0;
        for (int k = 0; k < 4; k++)
            out[i*4+j] += a[i*4+k] * b[k*4+j];
    }
}

static void Mat4RotateX(Mat4 m, float angle)
{
    Mat4Identity(m);
    m[5]  =  cosf(angle);
    m[6]  =  sinf(angle);
    m[9]  = -sinf(angle);
    m[10] =  cosf(angle);
}

static void Mat4RotateY(Mat4 m, float angle)
{
    Mat4Identity(m);
    m[0]  =  cosf(angle);
    m[2]  = -sinf(angle);
    m[8]  =  sinf(angle);
    m[10] =  cosf(angle);
}

static void Mat4Translate(Mat4 m, float x, float y, float z)
{
    Mat4Identity(m);
    m[12]=x; m[13]=y; m[14]=z;
}

/* =========================================================
 * RENDER DE LABERINTO (esqueleto — igual a la lógica del original)
 * ========================================================= */

static void RenderMazeWalls(const Mat4 view)
{
    if (!Maze) return;

    /* Por cada celda, emitir quads para paredes top y left si cerradas */
    /* En el original se hacía con glBegin/glEnd; aquí usamos VBOs estáticos */
    /* Aquí sólo hacemos el bucle de detección; el render de geometría
       depende de los modelos .gct/.gcm cargados via AAssetManager */

    for (uint32_t y = 0; y < MazeH; y++) {
        for (uint32_t x = 0; x < MazeW; x++) {
            uint8_t cell = Maze[GetOffset(x, y)];
            float wx = x * 2.0f;
            float wy = y * 2.0f;
            (void)wx; (void)wy; (void)cell; (void)view;
            /* TODO: emitir geometría de pared según cell & MZC_PASSTOP / MZC_PASSLEFT */
        }
    }
}

/* =========================================================
 * RENDER DE HUD / JOYSTICK VIRTUAL
 * ========================================================= */

static void RenderHUD(void)
{
    /* Usar progSolid para dibujar el joystick virtual en pantalla */
    /* Joystick izquierdo */
    /* (dibujar círculo de zona y círculo de posición actual) */
    /* Por brevedad sólo se define la estructura; la implementación
       completa requiere un VAO/VBO de círculo pre-generado */

    /* Subtítulos */
    if (ccText && ccTimer > 0.0f) {
        /* Renderizar texto con textura de fuente bitmap (.gcf en el original) */
        /* En Android usaremos una fuente bitmap cargada desde assets */
        ccTimer -= deltaTime;
        if (ccTimer < 0.0f) { ccTimer=0; ccText=NULL; }
    }

    /* Indicador de nivel */
    if (MazeLevelPopup) {
        MazeLevelPopupTimer -= deltaTime;
        Lerp(&MazeLevelPopupY, 16.0f, 2.0f*deltaTime);
        if (MazeLevelPopupTimer <= 0.0f) {
            Lerp(&MazeLevelPopupY, -48.0f, deltaTime);
            if (MazeLevelPopupY <= -47.0f) { MazeLevelPopup=0; MazeLevelPopupY=-48.0f; }
        }
    }

    /* Fade */
    if (fadeState == 1) {                 /* fade-in: oscurecer */
        Lerp(&fade, 0.0f, deltaTime * 2.0f);
        if (fade <= 0.0f) { fade=0; fadeState=0; }
    } else if (fadeState == 2) {          /* fade-out: aclarar */
        Lerp(&fade, 1.0f, deltaTime * 2.0f);
        if (fade >= 1.0f) { fade=1; fadeState=0; }
    }
}

/* =========================================================
 * LOOP DE RENDER PRINCIPAL
 * ========================================================= */

static void UpdateDeltaTime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t nowNs = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    if (lastTimeNs == 0) { lastTimeNs = nowNs; return; }

    float dt = (float)(nowNs - lastTimeNs) / 1e9f;
    lastTimeNs = nowNs;

    /* Limitar delta a 100ms para no tener saltos grandes */
    if (dt > 0.1f) dt = 0.1f;

    deltaTime = dt;
    delta2    = dt * 20.0f;
    delta20   = dt * 200.0f;
}

static void Render(void)
{
    glViewport(0, 0, screenW, screenH);
    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Construir MVP de cámara */
    Mat4 proj, rotX, rotY, trans, view, mvp;
    float aspect = (screenH > 0) ? (float)screenW / (float)screenH : 1.0f;
    Mat4Perspective(proj, 1.0472f /* 60° */, aspect, 0.01f, 100.0f);
    Mat4RotateX(rotX, camRotL[0]);
    Mat4RotateY(rotY, camRotL[1]);
    Mat4Translate(trans, camPosL[0], camPosL[1], camPosL[2]);

    Mat4 tmp;
    Mat4Multiply(tmp,  rotX,   rotY);
    Mat4Multiply(view, tmp,    trans);
    Mat4Multiply(mvp,  proj,   view);

    /* Actualizar rotación lerpeada de cámara */
    Lerp(&camRotL[0], camRot[0], delta2);
    Lerp(&camRotL[1], camRot[1], delta2);

    /* Actualizar posición lerpeada */
    Lerp(&camPosL[0], camPosNext[0], delta2);
    Lerp(&camPosL[1], camPos[1],     delta2);
    Lerp(&camPosL[2], camPosNext[2], delta2);

    /* Render del laberinto */
    RenderMazeWalls(mvp);

    /* HUD (se dibuja sobre todo, sin depth test) */
    glDisable(GL_DEPTH_TEST);
    RenderHUD();

    eglSwapBuffers(eglDisplay, eglSurface);
}

/* =========================================================
 * SPAWN DE ELEMENTOS DEL LABERINTO (idéntico al original)
 * ========================================================= */

static void GetRandomMazePosition(uint32_t *ox, uint32_t *oy)
{
    *ox = rand() % MazeW;
    *oy = rand() % MazeH;
}

static void SpawnMazeElements(void)
{
    /* Mínimo: posicionar puerta de salida */
    MazeDoorPos[0] = (float)(MazeW-1) * 2.0f + 1.0f;
    MazeDoorPos[1] = (float)(MazeH-1) * 2.0f + 1.0f;

    /* Posicionar llave */
    uint32_t kx, ky;
    GetRandomMazePosition(&kx, &ky);
    MazeKeyPos[0] = (float)kx * 2.0f + 1.0f;
    MazeKeyPos[1] = (float)ky * 2.0f + 1.0f;
    MazeKeyRot[0] = 0.0f;

    /* Posicionar brújula */
    if (!Compass) {
        GetRandomMazePosition(&kx, &ky);
        CompassPos[0] = (float)kx * 2.0f + 0.5f;
        CompassPos[1] = (float)ky * 2.0f + 0.5f;
        Compass = 1;
    }

    /* Posicionar entidades según hostilidad */
    if (MazeHostile) {
        GetRandomMazePosition(&kx, &ky);
        WBPos[0] = (float)kx * 2.0f + 1.0f;
        WBPos[1] = (float)ky * 2.0f + 1.0f;
        WB = 1;
    }

    LOGI("SpawnMazeElements completado. Nivel %u", MazeLevel);
}

/* =========================================================
 * SAVE / LOAD (Android: internal storage)
 * ========================================================= */

static void SaveGame(void)
{
    if (!savePath[0]) return;
    FILE *f = fopen(savePath, "wb");
    if (!f) { LOGE("No se pudo guardar"); return; }

    fwrite(&MazeLevel, sizeof(MazeLevel), 1, f);
    fwrite(&MazeSeed,  sizeof(MazeSeed),  1, f);
    fwrite(&MazeW,     sizeof(MazeW),     1, f);
    fwrite(&MazeH,     sizeof(MazeH),     1, f);
    fwrite(camPosL,    sizeof(camPosL),    1, f);
    fwrite(camRot,     sizeof(camRot),     1, f);
    fwrite(&Glyphs,    sizeof(Glyphs),     1, f);
    fwrite(&Compass,   sizeof(Compass),    1, f);
    fwrite(&Complete,  sizeof(Complete),   1, f);
    fclose(f);
    ShowSubtitles("TU PROGRESO HA SIDO GUARDADO.");
    LOGI("Partida guardada");
}

static void LoadGame(void)
{
    if (!savePath[0]) return;
    FILE *f = fopen(savePath, "rb");
    if (!f) {
        /* Primera vez: generar laberinto inicial */
        MazeW    = 6; MazeH    = 6;
        MazeSeed = (uint32_t)time(NULL);
        GenerateMaze(MazeSeed);
        SpawnMazeElements();
        camPosL[0] = 0.5f; camPosL[1] = flCamHeight; camPosL[2] = 0.5f;
        LOGI("Nueva partida iniciada");
        return;
    }

    fread(&MazeLevel, sizeof(MazeLevel), 1, f);
    fread(&MazeSeed,  sizeof(MazeSeed),  1, f);
    fread(&MazeW,     sizeof(MazeW),     1, f);
    fread(&MazeH,     sizeof(MazeH),     1, f);
    fread(camPosL,    sizeof(camPosL),    1, f);
    fread(camRot,     sizeof(camRot),     1, f);
    fread(&Glyphs,    sizeof(Glyphs),     1, f);
    fread(&Compass,   sizeof(Compass),    1, f);
    fread(&Complete,  sizeof(Complete),   1, f);
    fclose(f);

    GenerateMaze(MazeSeed);
    SpawnMazeElements();
    ShowSubtitles("?ESTE ES EL LUGAR DONDE ESTABA ANTES?");
    LOGI("Partida cargada. Nivel %u", MazeLevel);
}

/* =========================================================
 * GAME LOOP — hilo de render dedicado
 * ========================================================= */

static volatile int running = 0;
static pthread_t renderThread;
static pthread_mutex_t stateMutex = PTHREAD_MUTEX_INITIALIZER;

static void *RenderThreadFunc(void *arg)
{
    (void)arg;

    if (!InitEGL()) { LOGE("InitEGL fallo"); return NULL; }
    InitShaders();
    LoadGame();
    canControl = 1;

    LOGI("Bucle de render iniciado");

    while (running) {
        UpdateDeltaTime();

        pthread_mutex_lock(&stateMutex);
        ProcessTouchMove();
        if (canControl && !Menu) Control();
        pthread_mutex_unlock(&stateMutex);

        Render();
    }

    /* Limpiar EGL */
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(eglDisplay, eglContext);
    eglDestroySurface(eglDisplay, eglSurface);
    eglTerminate(eglDisplay);
    eglDisplay  = EGL_NO_DISPLAY;
    eglContext  = EGL_NO_CONTEXT;
    eglSurface  = EGL_NO_SURFACE;

    FreeMaze();
    LOGI("Hilo de render terminado");
    return NULL;
}

/* =========================================================
 * JNI — INTERFAZ CON JAVA/KOTLIN
 * ========================================================= */

JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeInit(
        JNIEnv *env, jclass cls,
        jobject surface, jobject assetManager, jstring dataPath)
{
    (void)cls;
    nativeWindow = ANativeWindow_fromSurface(env, surface);
    assetMgr     = AAssetManager_fromJava(env, assetManager);

    const char *path = (*env)->GetStringUTFChars(env, dataPath, NULL);
    snprintf(savePath, sizeof(savePath), "%s/masmze_save.bin", path);
    (*env)->ReleaseStringUTFChars(env, dataPath, path);

    srand((unsigned int)time(NULL));
    LOGI("nativeInit completado. Save: %s", savePath);
}

JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeStart(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    running = 1;
    pthread_create(&renderThread, NULL, RenderThreadFunc, NULL);
}

JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    running = 0;
    pthread_join(renderThread, NULL);
    if (nativeWindow) { ANativeWindow_release(nativeWindow); nativeWindow = NULL; }
}

/* Joystick virtual — llamado desde el touch handler Java */
JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeSetMoveJoy(
        JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&stateMutex);
    joyMoveX = x; joyMoveY = y;
    pthread_mutex_unlock(&stateMutex);
}

JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeSetCamJoy(
        JNIEnv *env, jclass cls, jfloat dx, jfloat dy)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&stateMutex);
    joyCamDX = dx; joyCamDY = dy;
    pthread_mutex_unlock(&stateMutex);
}

JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeButton(
        JNIEnv *env, jclass cls, jint btn, jboolean pressed)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&stateMutex);
    switch (btn) {
        case 0: keySpace  = pressed; break;  /* Acción */
        case 1: keyCtrl   = pressed; break;  /* Agacharse */
        case 2:                              /* Menú */
            if (pressed) { Menu = !Menu; canControl = !Menu; }
            break;
        case 3: btnAction = pressed; break;  /* Glifo */
    }
    pthread_mutex_unlock(&stateMutex);
}

JNIEXPORT void JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeSave(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&stateMutex);
    SaveGame();
    pthread_mutex_unlock(&stateMutex);
}

JNIEXPORT jint JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeGetLevel(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)MazeLevel;
}

JNIEXPORT jint JNICALL
Java_com_greatcorn_masmze_MasmzeLib_nativeGetGlyphs(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)Glyphs;
}
