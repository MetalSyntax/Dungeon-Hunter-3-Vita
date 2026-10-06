# Plan de Port — Dungeon Hunter 3 (PS Vita)

> Generado por psvita-port-toolkit el 2026-08-23. Punto de partida con lo detectado automáticamente --
confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** Dungeon Hunter 3
- **Paquete Java:** com.gameloft.android.ANMP.GloftD3HM
- **APK original:** `Dungeon-Hunter-3-v1-5-2c.apk`
- **TITLEID asignado:** `PSVDH0003`

**¿Motor conocido?** SÍ — confirmado 2026-10-01: es el mismo motor Gameloft
propietario que el port hermano `Dungeon-Hunter-2-vita` (no cocos2d-x, no Unity).
Evidencia: mismo paquete `com.gameloft.android.ANMP.GloftD3HM` vs
`com.gameloft.android.GAND.GloftD2SS`, mismo patrón `GL2JNILib`/`GLGame`,
mismos assets (`FileSystemAndroid::getResourcesPath`, `?shaders.pak`,
`data/*.bdae`, menús `data/menus*/dq*.swf` Scaleform/GFx, `gameswf_effects`),
misma carpeta SD (`/sdcard/gameloft/games/GloftD3HM`), `.so` no-stripped con
`debug_info` en ambos. Diferencias reales: DH3 carga UN solo .so
(`System.loadLibrary(getSoLibName())` → `libDH3.so`, o `libDH3android8.so` en
API<=8 — ese segundo .so se ignora: Vita equivale a API moderna), su loop es
`GL2JNILib.init/initGL/resize/stateChanged/step` (sin clase `GameRenderer` ni
`GameGLSurfaceView`), y linkea `libOpenSLES.so` directo (DH2 iba por stubs
Java AudioTrack). Todo el loader DH2 es reutilizable con prefijo JNI cambiado.

## 1. Detección automática

- **ABI(s):** armeabi-v7a
- **ABI elegida:** armeabi-v7a
- **Nota de arquitectura:** armeabi-v7a presente -> ARMv7 (hard-float/NEON disponible). El CPU de Vita (Cortex-A9) corre esto sin traducción.
- **Versión de GLES:** valor no estándar en manifest: 0x20000 (declarado en AndroidManifest.xml)

## 2. .so encontrados (ABI armeabi-v7a)

- `dungeonhunter3_extract/lib/armeabi-v7a/._libDH3.so` (4 KB)
- `dungeonhunter3_extract/lib/armeabi-v7a/._libDH3android8.so` (4 KB)
- `dungeonhunter3_extract/lib/armeabi-v7a/._libgeneratorD3HM.so` (4 KB)
- `dungeonhunter3_extract/lib/armeabi-v7a/libDH3.so` (21763 KB)
- `dungeonhunter3_extract/lib/armeabi-v7a/libDH3android8.so` (21751 KB)
- `dungeonhunter3_extract/lib/armeabi-v7a/libgeneratorD3HM.so` (437 KB)

**Decisión 2026-10-01 (confirmada con nm/objdump/strings, no asumida):**
- Solo se carga **`libDH3.so`** (`SO_PATH=ux0:data/dungeonhunter3/libDH3.so`).
  `libDH3android8.so` es la variante para Android API<=8 (`GLGame.getSoLibName()`,
  `GLGame.java:432-436`) — irrelevante en Vita. `libgeneratorD3HM.so` (stripped,
  437 KB) solo exporta `HDIDFV_getNHDIDFV[_Version]` (ID de dispositivo para DRM)
  — no se carga, igual que DH2 no carga `libStormGLOFT`/`libnativeinterface`.
- `libDH3.so`: ELF32 ARM, **no-stripped con debug_info** (21,7 MB, ago-2014),
  `SONAME libAndroid.so`, `NEEDED libstdc++.so libGLESv2.so liblog.so libc.so
  libm.so libdl.so libOpenSLES.so`. 337 imports UND — todos GLES2 programable
  (`glCreateShader/CompileShader/LinkProgram/UseProgram/VertexAttribPointer`,
  `glCompressedTexImage2D`, OES puntuales `...3DOES/MapBufferOES`), cero
  fixed-pipeline (`glMatrixMode`, `glEnableClientState`, etc. ausentes).
  Estrategia GL: vitaGL del sistema con su traductor GLSL→CG (la lib local se
  recompiló 2026-09-03 y lo incluye; softfp como el `.so`) + shaders GLSL de
  `shaders.pak` tal cual. Es el mismo camino con el que DH2 llegó a gameplay
  (vitaGL `cd3791e` desde fuente). Ver port_progress.md Fase 7.


## 3. Exports JNI (convención `Java_*`)

81 exports `Java_*` + `JNI_OnLoad` (`nm -D`, 2026-10-01; sin `RegisterNatives`
en dinámica — el registro ocurre dentro de `JNI_OnLoad`). Paquete base
`com.gameloft.android.ANMP.GloftD3HM`:

- **`GL2JNILib`** (37, el loop): `init`, `initGL`, `setPaths`, `resize`,
  `stateChanged`, `step`, `suspendGame`/`resumeGame`, `suspendVox`/`resumeVox`,
  `touchEvent`, `nativeAdd/Update/RemoveTouchEvent`, `nativeOnKeyDown/Up`,
  `keyEvent`, `accelerometerEvent`, `orientationChanged`, `setNetworkState`,
  `setCheckNexusDevice`, `setNumExtraContext`/`getNumExtraContext`,
  `isNativeLoaded`, `destroy`, `getViewSettings`, `nativeColorCorrection`,
  `nativeIsInActionPhase/LoadingFlow/ClickedIGP/Beam*`, `nativeKeyboardEnabled`,
  `processTouchpadAsPointer`, `nativeIsPowerAConnected`, `touchEvent`…
- **`GLGame`** (18): `_Init`, `nativeBonus`, `nativeSplashScreenFunc`,
  `nativeRetrieveItems`, `nativeOnMultiplayer/EditorEnd/Headset*`,
  `nativeHideKeyboard`, `nativeLogBITimePause`, `nativeTTVDActiveIGM`,
  `nativeGetDeviceFirmare(Model/MAC/UDID)` (sic, typo original de Gameloft).
- **Resto** (27): `DataSharing_nativeInit`,
  `PushNotification.C2DMAndroidUtils_nativeInit/nativeCallBack`,
  `SplashScreenDialog_nativeTrackWSQuit`,
  `FacebookAndroidGLSocialLib_nativeInit/nativeOnFB*` (6),
  `iab.InAppBilling_nativeInit/SendData/SetContext/SetIABObject` + 5 `iab.s_*`
  ofuscados, `installer.GDRMPolicy_{initNativeAP,nativeAllow,processServer,
  setConst,setLongConst,setTime,getConstString}` (7, el DRM),
  `installer.GameInstaller_{initNative,nativeGetDevID,nativeStart,
  getPublicKey}` (4, el gate de arranque). BSS global `LicenseCheck` (1 byte).

**Superficie real nativo→Java** (strings exactos en el .so, 2026-10-01 — los
523 candidatos de `generated_jni_table.h` son el techo jadx, NO lo que el .so
pide; misma lección que DH2: solo ~13 + 4): `getSaveFolder`, `getSDFolder`,
`getResource`, `getIMEI`, `getClientID`, `getObbFileName`, `setupPaths`,
`getTexturePack`, `getColorPack`, `isWifiEnabled`, `Exit`, `notifyTrophy`,
`openIGP` + `mSUtils_GetPrefs/GetPackage/GetSaveFolder/GetContext` (IDs
cacheados de `GLUtils/SUtils`). `FileSystemAndroid::getResourcesPath` resuelve
rutas en C++ (alimentado por `setPaths`), con fallback
`/sdcard/gameloft/games/GloftD3HM`. La tabla FalsoJNI real (`source/java.c`,
hoy vacía) debe construirse método a método contra estos strings + jadx, con
firmas confirmadas — NO copiar los 523 stubs a ciegas.


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (2026-10-01: motor hermano de DH2 confirmado
  con nm/objdump/strings/jadx — ver §0-3; pseudo-C de Ghidra de libDH3
  pendiente: `decompiled/libDH3_armeabi-v7a/ghidra/` está vacío).
- [x] Bootstrap del loader (2026-10-01: `SO_PATH`→`libDH3.so`, `main.c` con la
  secuencia real `init/initGL/[setPaths]/Init/resize/stateChanged/step`,
  `dynlib.c` sin gaps contra los 337 UND, `lib/falso_jni` repuesto desde DH2,
  `game_log` shim como en DH2-Fase 2, primer build verde `eboot.bin` +
  `dungeonhunter3.vpk` — ver port_progress.md Fase 4).
- [x] Tabla JNI (FalsoJNI) (2026-10-01: `source/java.c` con el subset real de
  §3 — 34 métodos con firmas jadx y cuerpos con efecto real donde el APK es
  inequívoco; ver port_progress.md Fase 5).
- [x] Bootstrap input+gráficos+DRM (2026-10-01: `main.c` loop táctil+botones,
  `gl_init` vitaGL tras los fakes EGL, `patch.c` con `nativeAllow→1`).
- [x] Análisis estático profundo (2026-10-01, port_progress.md Fase 6): barrido
  JNI real (179 lookups), 6 bugs de arranque corregidos (SDFolder, setPaths,
  nombre/ruta del OBB, touch de pantalla + escala 1920×1088, orden de ciclo de
  vida, jclass NULL), gamepad Xperia/MOGA nativo + sticks, audio AudioTrack
  sobre sceAudioOut, idioma de sistema. Herramienta: `extras/scripts/so_annotate.py`.
- [ ] Primer arranque en consola real.
- [x] Gráficos, lado estático (Fase 7): imports GL cruzados contra vitaGL,
  `glCompressedTexSubImage2D` real para re-subidas de nivel completo, sin MSAA,
  caché de shaders arreglado + log de compile/link, shaders extraídos del OBB
  y comparados con los probados de DH2, fix de ruta duplicada de DH2 portado.
  Pista si hay enemigos invisibles: DH2 lo resolvió desactivando culling (sin
  causa raíz); en DH3 el equivalente sería `CustomSceneGraphCuller::update`.
- [x] Input: teclado en pantalla vía `sceImeDialog` (Fase 7).
- [x] Video: no hay (confirmado, Fase 7).
  El splash `res/raw/logo_vita.mp4` (H.264, añadido del port, main.c:226)
  usa desde Fase 13 el allocator de 3 niveles de Shadow-Guardian-vita
  (CDRAM→PHYCONT→UNCACHE) + subida a resolución completa — ver
  port_progress.md Fase 13; el crash de Fase 13 (stack smash en
  `FileSystemBase::customCreateAndOpenFile` 0x610eb4, buf de 512 en stack)
  se contiene con `source/reimpl/fsbuf.c` (strcpy/strcat acotados).
- [ ] Gráficos en consola real.
- [x] Audio: backend AudioTrack de vox forzado (`VoxSetAndroidAPILevel(8)`) e
  implementado en java.c sobre `sceAudioOut` — sin mixer DH2. Falta oírlo.
- [x] Assets: staging en `ux0_data/dungeonhunter3/` (libDH3.so + OBB
  `main.1501…obb`, pak propio; se acepta en la raíz o en `<pkg>/`, `obb/<pkg>/`,
  `Android/obb/<pkg>/`). Herramienta: `extras/scripts/dh3_pak.py`.
- [x] LiveArea con arte DH3 (icono del APK) en `extras/livearea/`.
- [ ] Pruebas en hardware real.

## 6. Notas de bootstrap (2026-10-01, todo verificado con build real)

- `lib/falso_jni/` era submódulo vacío → repuesto copiando los 8 ficheros
  vendorizados de DH2 (mismo MIT FalsoJNI). Si se re-clona el repo, recordar
  `git submodule update --init` o repetir la copia.
- Build: `cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5` (VitaSDK viejo vs CMake
  nuevo, igual que DH2) + `PATH=$HOME/vitasdk/bin:$PATH` (`vita-libs-gen`) +
  compilar desde copia sin espacios (`rsync` a `/tmp`, mismo workaround que DH2).
- `glBlendColor/glCompressedTexSubImage2D/glDetachShader/glValidateProgram`:
  la vitaGL de sistema no los declara → stubs vacíos locales en
  `source/utils/glutil.c` (copia exacta del patrón DH2, que los tiene igual).
  OJO: DH2 los marcó "re-wired to real" en su plan pero su código muestra que
  siguen siendo no-ops — no asumir que existen en el driver.
- `writev`: sin `sys/uio.h` en Vita → `ret0` temporal. `slCreateEngine` +
  `SL_IID_*` → stubs neutros hasta que el log diga si hay camino OpenSL real.
  `gl*3DOES` → `ret0` (pipeline 2D/GFx no debería tocar texturas 3D).
- Link: `-Wl,--allow-multiple-definition` porque la vitaGL de sistema exporta
  `eglQueryString` y colisiona con el fake `source/reimpl/egl.c`. DH2 no lo
  sufre (compila vitaGL desde `lib/vitagl`). El log de arranque debe confirmar
  qué `eglGetDisplay` corre realmente.
- (Histórico) `step()` ya se llama desde Fase 5; la secuencia y los args de
  `init/setPaths/touchEvent/nativeOnKeyDown` quedaron confirmados por
  desensamblado en Fase 6 (port_progress.md).

## 5. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).

## Auto-detected lifecycle methods (psvita-toolkit)

Native methods whose name matches a well-known Android/GL app lifecycle hook --
these are the ones `main.c`/the loader most likely needs to call directly to
drive the game (there's no real Android `Activity`/`GLSurfaceView` calling them
for you).

- `com.gameloft.android.ANMP.GloftD3HM.DataSharing.nativeInit(void)`
- `com.gameloft.android.ANMP.GloftD3HM.GLUtils.Device.nativeInit(void)`
- `com.gameloft.android.ANMP.GloftD3HM.GLUtils.SUtils.nativeInit(void)`
- `com.gameloft.android.ANMP.GloftD3HM.IGPFreemiumActivity.nativeInit(void)`
- `com.gameloft.android.ANMP.GloftD3HM.PushNotification.C2DMAndroidUtils.nativeInit(void)`
- `com.gameloft.android.ANMP.GloftD3HM.glsociallib.facebook.FacebookAndroidGLSocialLib.nativeInit(void)`
- `com.gameloft.android.ANMP.GloftD3HM.iab.InAppBilling.nativeInit(Context)`
