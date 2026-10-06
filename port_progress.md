# Registro de Progreso — Dungeon Hunter 3 (PS Vita)

## Fase 1: Configuración y Preparación (Completada — 2026-08-23)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `Dungeon-Hunter-3-v1-5-2c.apk` copiado y extraído.
- ABI detectada: armeabi-v7a (elegida: armeabi-v7a).
- GLES detectado: valor no estándar en manifest: 0x20000 (declarado en AndroidManifest.xml)

## Fase 2: Decompilación (Completada — 2026-08-23)
- jadx: corrido, resultados en decompiled/apk_jadx/.
- Ghidra (.so): corrido para cada .so.

## Fase 3: Análisis del Motor Real (Completada — 2026-10-01)

Motor confirmado: **mismo Gameloft propietario que DH2** (`Dungeon-Hunter-2-vita`),
no cocos2d-x/Unity/Gamevil. Evidencia cruzada (tres fuentes, nada asumido):
- jadx (`decompiled/apk_jadx/sources/com/gameloft/android/ANMP/GloftD3HM/`):
  actividad `GLGame extends GL2JNIActivity`, **un solo** `System.loadLibrary(getSoLibName())`
  → `libDH3.so` (o `libDH3android8.so` si API<=8 — se ignora en Vita);
  `GL2JNILib.java` declara 35 nativos (`init`, `initGL`, `setPaths`, `resize`,
  `stateChanged`, `step`, `suspend/resumeGame/Vox`, `touchEvent`,
  `nativeAdd/Update/RemoveTouchEvent`, `nativeOnKeyDown/Up`, …);
  `GLGame.java` aporta `Init`, `nativeSplashScreenFunc`, `nativeRetrieveItems`, etc.
  Renderer (`GL2JNIView.java`): surfaceCreated→`stateChanged(true)`,
  surfaceChanged→`resize(max,min)`, onDrawFrame→splash?`nativeSplashScreenFunc`:`step()` @33 ms.
  EGL pedido: RGBA 565, depth 16, stencil 8. Gate de arranque: nada corre hasta
  `GameInstaller.sbStarted` (installer LVL Google + `GDRMPolicy` nativa + DRM coreanas).
- `nm -D libDH3.so`: **81 exports `Java_*` + `JNI_OnLoad`** (el plan decía "ninguno" —
  corregido en PORTING_PLAN.md §3), sin `RegisterNatives` en dinámica.
  `libgeneratorD3HM.so` solo trae `HDIDFV_getNHDIDFV[_Version]` → no se carga.
- `objdump -T libDH3.so`: 337 UND, 100% GLES2 programable, cero fixed-pipeline;
  `NEEDED`: stdc++, GLESv2, log, c, m, dl, **OpenSLES** (DH2 no lo linkeaba).
  `strings`: `FileSystemAndroid::getResourcesPath`, `?shaders.pak`, `.bdae`,
  menús Scaleform `data/menus*/dq*.swf`, `/sdcard/gameloft/games/GloftD3HM`, `.obb`.
  Sin `videoDone`/`ALicense`/`GameRenderer`/`GLSurfaceView` (DH3 no usa ese puente);
  DRM = BSS `LicenseCheck` + `GDRMPolicy_{initNativeAP,nativeAllow,…}` +
  `GameInstaller_{initNative,nativeGetDevID,nativeStart,getPublicKey}`.
- Superficie real nativo→Java (strings exactos, el techo son los 523 candidatos
  autogenerados — no copiarlos a ciegas): `getSaveFolder`, `getSDFolder`,
  `getResource`, `getIMEI`, `getClientID`, `getObbFileName`, `setupPaths`,
  `getTexturePack`, `getColorPack`, `isWifiEnabled`, `Exit`, `notifyTrophy`,
  `openIGP` + `mSUtils_GetPrefs/GetPackage/GetSaveFolder/GetContext`.
- Hueco: `decompiled/libDH3_armeabi-v7a/ghidra/` está **vacío** (el plan Fase 2
  decía "corrido para cada .so" pero no hay pseudo-C de libDH3). Hace falta para
  confirmar firmas JNI y el gate `GameInstaller/nativeStart` antes de la tabla real.

## Fase 4: Bootstrap del Loader (Completada — 2026-10-01, build verde sin consola)

- `CMakeLists.txt`: `SO_PATH` `main.so` → **`libDH3.so`** (era el default del template).
- `lib/falso_jni/`: submódulo vacío → repuestos los 8 ficheros desde DH2 (mismo MIT).
- `source/dynlib.c`: cruce 1:1 contra los 337 UND — gaps cerrados: `modff`,
  `perror`, `pread`, `strptime` → reales; `getuid/euid/gid/egid/pwuid/protobyname`
  → neutros; `writev` → `ret0` (sin `sys/uio.h` en Vita);
  `slCreateEngine`+5 `SL_IID_*` → stubs neutros (¿camino OpenSL real? lo dirá el log);
  `gl*3DOES` → `ret0`; `glBlendColor/glCompressedTexSubImage2D/glDetachShader/
  glValidateProgram` → stubs vacíos locales en `utils/glutil.c` (patrón DH2 exacto;
  la vitaGL de sistema no los declara — el plan DH2 dice "re-wired to real" pero su
  código los tiene como no-ops, no asumir driver real).
- `source/main.c`: secuencia real del motor (`JNI_OnLoad` → `gl_init` →
  `init/initGL/[setPaths]/GLGame.Init/resize/stateChanged` → loop `step()`),
  con `game_log` shim (mismo bug de enlace que DH2-Fase 2). `step()` aún NO se
  llama: `init(F)`/`setPaths` toman objetos Java y el marshaling está sin confirmar.
- Link: `-Wl,--allow-multiple-definition` (la vitaGL de sistema exporta
  `eglQueryString` y colisiona con `reimpl/egl.c`; DH2 no lo sufre porque compila
  vitaGL desde fuente). El log debe confirmar qué EGL corre; fix largo = vendorizar
  vitaGL como DH2.
- **Primer build verificado real** (no solo leído): cmake
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` + `PATH` con `vita-libs-gen` + rsync a
  `/tmp` sin espacios → `eboot.bin` + `dungeonhunter3.vpk` sin errores. Dos bugs de
  build cazados así (headers vitaGL sin esos 4 GL + `game_log` indefinido), no por
  lectura.
- `source/java.c` sigue con tablas **vacías** y `source/patch.c` con `so_patch()`
  vacío a propósito: la tabla real (subset §3 + firmas) y los bypass DRM
  (`nativeAllow→1`, `nativeStart`, `LicenseCheck`) necesitan el pseudo-C o el primer
  log — inventarlos sería adivinar.

## Fase 5: Tabla JNI + Input + Gráficos + Audio/Video + DRM (Completada — 2026-10-01, build verde sin consola)

Firmas exactas extraídas de jadx (todas `static`): `GL2JNILib.getSDFolder():String`,
`getResource(String):byte[]`, `setupPaths():void`, `setPaths(S,S,S)` nativa,
`getTexturePack()/getColorPack():int`, `notifyTrophy(int)`, `openIGP()`,
`getObbFileName[int](int):String` (relativo con `../../../`, absoluto con
`Environment`, o `"../"*i`), `setCurrentContext(int):boolean`,
`SUtils.getSaveFolder()/getSDFolder()/getPackage()/getContext()`,
`nativeGet/SetPreference(Bundle)`, `getPreferenceString(2 y 3 args: colisionan
por nombre en FalsoJNI)`, `playVideo(String,boolean):boolean` (**stub `false`
en el propio APK**), `stopVideo/inGameVideo*: no-ops`, `onVideoFinished()` nativa,
`GLGame.getIMEI()/getClientID()="1674:53123:1.5.2:android:googleplay"/Exit/
isWifiEnabled()/splashScreenFunc()/getSoLibName()`, `GDRMPolicy.nativeAllow(long):int`
(licencia si `==1`), `GameInstaller.nativeStart():void`.

- `source/java.c` reescrito (34 métodos): `getSDFolder/getSaveFolder`→`DATA_PATH`;
  `getResource`→lectura real (`DATA_PATH` + `assets/`, strip de `.` inicial como el
  Java); `setupPaths`→llama al `setPaths` nativo con `DATA_PATH`×3 (patrón DH2);
  `getTexturePack/getColorPack`→0 (Vita no matchea ningún `Build.MODEL`: idéntico
  al comportamiento real); `notifyTrophy`→fiel al original (`androidTrophy.dat`,
  101 bytes); `openIGP/splashScreenFunc`→noop; `getObbFileName*`→literal
  `main.15221...obb`; `setCurrentContext`→1; `getPackage/getClientID/getIMEI/
  getSoLibName`→literales; `Exit`→`sceKernelExitProcess`; `isWifiEnabled/
  playVideo`→0 (playVideo falso = videos saltados **por diseño del APK**, sin gate
  tipo `videoDone` de DH2); prefs→neutros (`""`/0/noop). FalsoJNI matchea por
  NOMBRE (firma ignorada en lookup, verificado en `FalsoJNI.c`) y los boolean se
  registran INT (precedente DH2). RIESGOS anotados en código: `getContext`→NULL,
  `nativeGetPreference`→NULL (sin Bundle), `getObbFileName` relativo con `../`.
- `source/main.c`: loop real — `JNI_OnLoad` → `gl_init` (vitaGL real tras los
  fakes `reimpl/egl.c`, mismo split-brain que DH2) → `init(0)` (`GLGame.F=0`,
  `GLGame.java:92`) → `initGL` → `setPaths` → `GLGame.Init` → `resize(960,544)`
  → `stateChanged(1)` → loop: `PeekBufferPositiveExt2` (+fallback), `sceTouchPeek`
  frontal 960×544 sin escalar → `nativeAdd/Update/RemoveTouchEvent(id,x,y)`
  (nombres autodescriptivos, sin orden de args que adivinar como en
  `touchEvent(int⁴)`), CROSS→23/CIRCLE→4(BACK)/TRIANGLE→82(MENU) vía
  `nativeOnKeyDown/Up(code,0)` (2º arg desconocido: 0 + log), `step()`, `gl_swap`,
  33 ms (throttle de `GL2JNIView.onDrawFrame`); START+SELECT→`suspendGame`+salida.
  Llamadas estilo JNI `fn(&jni,NULL,…)` como DH2. `nativeAllow` existe ×1.
- `source/patch.c`: `GDRMPolicy.nativeAllow→ret1` (licenciado; servidor muerto igual).
  `nativeStart/initNative/initNativeAP` intactos a propósito (pueden inicializar de
  verdad). Sin `sp_counted_base` en este `.so` (otro STL que DH2) → sin parche SWP;
  sin clase `ALicenseCheck` (solo BSS `LicenseCheck`, cubierto por el bypass).
- Audio: `slCreateEngine→ret1` ("sin dispositivo") en vez de éxito falso — con
  éxito falso el motor desreferenciaría un engine NULL. Si el log muestra que el
  motor ignora el error, portar el mixer `audio.cpp` de DH2 (ออกแบบ con
  `data/sounds/` de DH3 para `sounddefs.h`). Video: nada que hacer (APK lo salta).
- **Build verde verificado** (3er `cmake && make` real): solo faltó
  `psp2/kernel/processmgr.h` para `SCE_KERNEL_POWER_TICK_*`. Artefactos en
  `build/` (`eboot.bin` 567 KB + `dungeonhunter3.vpk` 651 KB, TITLEID `PSVDH0003`,
  LiveArea con placeholders del template — reemplazar `icon0/bg0/pic0/startup.png`
  con arte DH3 antes del release).

## Fase 6: Análisis estático profundo de libDH3.so (Completada — 2026-10-01, build verde sin consola)

Sin pseudo-C de Ghidra (`ghidra/` vacío) y con DWARF solo de libgcc, pero con
`.symtab` completo: objdump + anotador propio que resuelve literales PC-relativos
y offsets de la vtable JNIEnv (`extras/scripts/so_annotate.py fn|jni|callers`).
Barrido `jni`: **179 lookups reales** (FindClass/Get*MethodID/Get*FieldID con
literales) — esa es la tabla FalsoJNI verdadera, no los 523 de jadx.

Bugs reales encontrados y corregidos (todos habrían roto el primer arranque):
1. **Ruta de datos** — `AndroidOS::GetSDFolder` (0x311dec) llama
   `getPreferenceString("SDFolder", mPreferencesName)`; con `""` cae a
   `/sdcard/gameloft/games/GloftD3HM` hardcodeado. Ahora responde el root real.
   Además `mPreferencesName` no estaba registrado → FalsoJNI devuelve id 0 =
   `WINDOW_SERVICE` ("window") para TODO campo desconocido; registrados
   `MODEL`, `mPreferencesName`, `SERIAL`.
2. **setPaths** recibía `DATA_PATH`×3; el Java real pasa `(sdFolder, filesDir,
   cacheDir)` → `glf::Fs::SetDataDir/SetHomeDir/SetTempDir` (0x8497c0). Ahora
   `root`, `root/files`, `root/cache` (mkdir), sin `/` final como el Java.
3. **OBB con nombre equivocado** — el archivo real es `main.1501…obb`
   (`getObbFileName`); `main.15221` es el de las variantes 2/3/4. No es zip:
   es pak propio (`FileSystemBase::addCustomPakArchive`, vtable +252) que
   `Application::InitAndroid` monta tras chequear `existFile("data/file000000.dat")`.
   `glf::Fs::ResolvePath` (0x8458e8) solo trata como absoluto `path[1]==':'` o
   lo que empieza por el DataDir → se devuelve absoluto bajo root, eligiendo el
   que exista.
4. **Touch de pantalla por el camino equivocado** — `nativeAdd/Update/RemoveTouchEvent`
   solo se envían con `SOURCE_TOUCHPAD` (pad trasero Xperia Play) y van a
   `Application::UpdateTouchPad`. La pantalla es `touchEvent(action 0=up/1=down/
   2=move, x, y, pointerId)` → `glf::AndroidAddTouchEvent(iiii)` (0x849980).
   Además el panel frontal reporta **1920×1088**, no 960×544 (se divide por 2).
5. **Orden de ciclo de vida** — Java: `init` → `GLGame.Init` → `initGL` +
   `getViewSettings` → `stateChanged(true)` → `resize`. main.c llamaba resize
   antes de stateChanged y nunca getViewSettings.
6. **jclass/thiz NULL** — `init` cachea `NewGlobalRef(clazz)` en
   `glf::sClassGL2JNILib`; ahora se pasan nombres tipo FindClass.

Mejoras con evidencia (no adivinadas):
- **Gamepad nativo** — `glf::AndroidOnKeyDown` (0x849c64) ignora el 2º arg y mapea
  el set Xperia Play/MOGA a `ZeusKeyPad` (tabla de `az.java`): D-pad 19-22,
  ✕→23, ○→1000, □→99, △→100, L/R→102/103, Start/Select→108/109. Se activa con
  `nativeKeyboardEnabled(1,1,0,1)` en cuanto existe `Application::s_instance`
  (exportado). Sticks → pads Xperia emulados (`HUDControls::ProcessZeusGamePad`):
  izquierdo centro (180,180) = movimiento, derecho (780,180) = apuntar a
  distancia, radio 170, deadzone 28. Se quitó START+SELECT=salir (son botones
  del juego).
- **Audio real** — vox elige backend en `DriverAndroid::Init` (0x718c7c):
  API>8 → OpenSL, ≤8 → AudioTrack por JNI. `s_androidAPILevel` solo lo lee ese
  Init → main.c llama `VoxSetAndroidAPILevel(8)` tras `JNI_OnLoad` y java.c
  implementa `android/media/AudioTrack` sobre `sceAudioOut` (puerto BGM 44100 Hz
  estéreo, grain 1024, `write()` bloqueante = pacing de `MODE_STREAM`).
  Protocolo exacto sacado de `_InitAT` + `UpdateThreadedAT` (hilo propio).
  Ya no hace falta portar el mixer de DH2.
- **Idioma** — `GetDeviceLanguage` compara `getISO3Language()` con eng/kor/fra/
  spa/deu/ita/jpn/zho (+`getCountry()=="CN"`): se deriva de
  `SCE_SYSTEM_PARAM_ID_LANG`.
- **Clase de dispositivo** — heurística del propio APK con GPU PowerVR SGX543:
  `isHighPhoneGroup=1`, `isLowPhoneGroup=0`, `checkFirmware=1`;
  `isAndroidOrientationAvailable=0`.
- **Strings nunca NULL** — 16 métodos String del barrido registrados como `""`
  y red de seguridad en FalsoJNI: `GetStringUTFChars(NULL)` → `""` en heap
  (el nativo hace strcpy sin chequear, p.ej. `GetDeviceMAC`).

Descartado con evidencia:
- **EGL**: libDH3 importa **cero** símbolos EGL (Java manejaba EGL) → el
  conflicto `eglQueryString` del `--allow-multiple-definition` es inocuo.
- **DRM**: `LicenseCheck` solo lo tocan `initNative/nativeStart/
  nativeGetSdFolderPath` del installer; ningún código de juego llama al módulo
  DRM. El gate es 100% Java (no se ejecuta). El parche `nativeAllow→1` queda
  pero no es necesario.
- **Texturas**: glitch trae descompresores software ETC1/PVRTC de respaldo.
- **Splash/Welcome**: `BeginWelcomeScreen` es fire-and-forget (Activity promo).
- `UpdateHomeDataDir` (copia data→home) no tiene llamadores.

Otros: `.gitignore` ahora ignora `*.obb` y `/com.gameloft.android.ANMP.GloftD3HM/`
(el OBB de 1 GB estaba sin ignorar). LiveArea con arte DH3 (icono del APK;
el único fondo grande del APK es la marca japonesa "Dark Quest III").
Build verde (`psvita-toolkit build`): `eboot.bin` 580 KB, `dungeonhunter3.vpk` 664 KB,
sin warnings en java.c/main.c.

## Fase 7: Segunda pasada estática — gráficos, input, audio, video, LiveArea (2026-10-01, build verde sin consola)

**Gráficos**
- Cruce de los 98 imports GL contra `libvitaGL.a` del sistema: faltan solo
  `glBlendColor` (sin equivalente en sceGxm → no-op inevitable),
  `glCompressedTexSubImage2D`, `glSampleCoverage` y los `*3DOES` (texturas 3D,
  no existen en Vita). `glMapBufferOES/glUnmapBufferOES` ya iban a
  `glMapBuffer/glUnmapBuffer` reales.
- `glCompressedTexSubImage2D`: lo usa `CTexture::updateData` de glitch para
  re-subir un nivel comprimido entero (offset 0,0). Antes no-op (texturas que no
  se refrescan); ahora re-especifica el nivel con `glCompressedTexImage2D`. Un
  update parcial real se loguea una vez.
- MSAA 4x → **NONE**: el `ConfigChooser` de Android nunca pide `EGL_SAMPLES`
  (RGB565/depth16/stencil8); 4x era VRAM y fill-rate que el original no pagaba.
  DH2 terminó igual. Los umbrales CDRAM/PHYCONT custom de DH2 eran por
  SceAvPlayer (video); DH3 no tiene video.
- **Corrección a lo que decía el plan:** "DH2 demostró que el compilador GLSL ES
  real los acepta" era incorrecto tal como estaba escrito. DH2 pasó por vitaGL
  sin traductor → PVR_PSP2 → y terminó en **vitaGL compilado desde fuente con
  el traductor GLSL** (`cd3791e`, gameplay real en hardware). La `libvitaGL.a` del
  sistema de esta máquina se recompiló el 2026-09-03 y **sí trae el traductor**
  (`glsl_translator_process`, `glsl_inject_mul`, preprocesador). Toda la VitaSDK
  local es softfp (sin `Tag_ABI_VFP_args`), igual que el `.so` → sin desajuste
  de ABI en las funciones GL con floats.
- **Bug real en el caché de shaders del boilerplate** (`DUMP_COMPILED_SHADERS=ON`
  por defecto en el toolkit): guardaba el binario sin mirar si compiló, y en
  modo `VGL_MODE_POSTPONED` (el default de vitaGL, el mismo que usa DH2) la
  compilación real ocurre en `glLinkProgram`, así que `vglGetShaderBinary` tras
  `glCompileShader` no devuelve nada útil → se escribían `.gxp` vacíos/basura
  que el siguiente arranque cargaba con `glShaderBinary` en vez de la fuente.
  Ahora: solo se cachea si compiló y `len > 0`, se crea `gxp/`, y
  `glCompileShader`/`glLinkProgram` loguean `FAILED` con el info log (wrapper de
  link nuevo, cableado en dynlib). Esa línea es la que hay que buscar en el log.
- **Shaders extraídos y revisados**: `extras/scripts/dh3_pak.py` lee el pak
  propio del OBB (formato y cifrado de nombres reconstruidos de
  `CCustomPakReader::scanLocalHeader` + `DECODE_XOR32` + `getRand`: LCG
  1664525/1013904223, clave 0x3857a). 8748 archivos. `shaders.pak` es un zip
  stored con 54 `.glsl` GLSL ES. Contra los 34 de DH2 (probados en hardware):
  22 nuevos, 14 cambiados, y el perfil de construcciones riesgosas es casi
  idéntico (sin `#extension`, `texture2DLod`, derivadas, structs, ni
  `gl_FragData`). Lo único nuevo: un `mat3(9 escalares)` en
  `CharNormalSpec_iPhone_VS` y un `for` de bound `const` en `GameSWFFS_blur`.
- Port del fix de DH2 (crash real en `COnDemandReader`): `fopen`/`open` colapsan
  rutas con la raíz duplicada (`ux0:data/x/ux0:data/x/...`) — probado en host.
- Culling/enemigos invisibles de DH2: no se porta. Causa raíz nunca encontrada,
  el bypass baja el FPS a la mitad, y DH3 usa otra arquitectura
  (`CustomSceneGraphCuller`/`ICuller`). Pista anotada en el plan.
- Fuentes: vienen en el OBB y `Application::ExtractFontFiles` las escribe vía
  glitch → no hay redirect `/system/fonts` como en DH2.

**Input**
- Teclado en pantalla: `OnScreenKeyboard::GetString` (prompts de GameSWF y el
  estado `GSKeyboard`) → Java `OpenInput(maxLen, type, keep)`. Ahora java.c lo
  encola y main.c abre `sceImeDialog` (con máscara si `type==1` = password,
  `maxLen` respetado) y contesta como el EditText de Android:
  `nativeOnEditorEnd(text)` si no está vacío + `nativeHideKeyboard()`. El juego
  sigue renderizando debajo (el swap de vitaGL compone el diálogo) y el input al
  juego se congela mientras está abierto.

**Video:** confirmado que no hay: ningún video en el OBB, ninguna llamada
nativa de reproducción; el `NetStream` de GameSWF está compilado pero sin uso.

**Audio:** sin cambios respecto de Fase 6 (backend AudioTrack de vox sobre
`sceAudioOut`); la grabación por OpenSL (`_InitRecordOSL`) solo se usa con
micrófono y `slCreateEngine` falla limpio.

**LiveArea / deploy:** sin cambios de arte. El usuario armó `ux0_data/dungeonhunter3/`
con el OBB **en una subcarpeta** (`com.gameloft.android.ANMP.GloftD3HM/main.1501…obb`,
tal como sale del zip de cache): `dh3_paths_init` ahora busca el OBB en la raíz,
`<pkg>/`, `obb/<pkg>/` y `Android/obb/<pkg>/`.

Build verde, sin warnings en los archivos tocados.

## Fase 8: Primer arranque en consola (2026-10-01/02)

**Bug 1 — crash en el arranque (confirmado con psp2dmp, sin log).**
`logs/dungeonhunter3-psp2core-1790913164-…psp2dmp`: Data abort en `strcpy`
← `glf::Strcpy_s` ← `glf::AndroidSetPaths+0x28` ← `setPaths` ← `dh3_setup_paths`
(main.c). Destino `R0 = 0x000100e8` = `App::GetFs()` (`this + 0x100e8`, 0x84a908)
con `this == NULL`: la `glf::App` todavía no existía. En Android Java nunca llama
`setPaths` por su cuenta: `getViewSettings()` → `glf::AndroidGetViewSettings`
(0x849668) hace `NewApp()` → `AndroidSetupPaths()` → Java `setupPaths` →
`setPaths` → `App::MyInit()`. **Fix:** main.c ya no llama `dh3_setup_paths()`;
el motor lo dispara solo dentro de `getViewSettings` (nuestro handler
`GL2JNILib_setupPaths` hace el resto).

**Logs a archivo.** El logger solo imprimía por `sceClibPrintf`, así que sin
listener no quedaba nada. Ahora cada arranque abre
`ux0:data/dungeonhunter3/logs/log_NNN.log` (NNN = mayor existente + 1) y escribe
cada línea con `sceIoWrite` sin buffer (sobrevive al crash), con timestamp
`[seg.ms]` y sin códigos de color.

## Fase 9 en adelante: PARADA — hace falta consola real

Nada más se puede verificar sin hardware. Desplegar `ux0_data/dungeonhunter3/`
tal cual a `ux0:data/dungeonhunter3/` + instalar `build/dungeonhunter3.vpk`
(requiere `kubridge` y `libshacccg.suprx`). En el primer log, en orden:
1. `[Java] paths: … obb=…` sin `(MISSING)`.
2. `glLinkProgram(...) FAILED` / `glCompileShader(...) FAILED` → shader a
   mirar con `dh3_pak.py extract <obb> '^shaders\.pak$' <dir>`.
3. `GetStringUTFChars(NULL)` / `method ID … not found` → JNI faltante.
4. `new AudioTrack(...)` + puerto sceAudioOut → audio vivo.
5. `[input] Xperia/Zeus gamepad mode enabled`.
6. `path with doubled root collapsed` → confirma que el bug de DH2 también está.
7. Si aparece pantalla de un color plano sin UI: revisar primero shaders (2),
   luego stencil (GameSWF lo necesita; vitaGL lo trae por defecto).

## Fase 9: segunda corrida en consola (2026-10-02, log_001)

**El psp2dmp de esta corrida NO es de DH3.** `…1790919602…psp2dmp` es de
Gangstar Rio (hilo `PSVGR0001`, módulo `gangstarrio`, rutas
`ux0:/data/gangstarrio/`): la Vita guarda los dumps de todos los juegos en
`ux0:data/` y se bajó el más nuevo. Antes de analizar un dump, confirmar el
nombre del hilo (`PSVDH0003`) / módulo (`dungeonhunter3`). DH3 no dejó dump:
el log se corta a los 18.3 s tras crear el AudioTrack.

**Bug 2 — todos los métodos Java `boolean` eran inalcanzables.** Log:
`methodBooleanCall method ID 12/35/37 not found` = `setCurrentContext`,
`isHighPhoneGroup`, `isLowPhoneGroup`. Estaban registrados como INT
("precedente DH2"), pero este FalsoJNI despacha `CallStaticBooleanMethod*`
SOLO a `methodsBoolean[]` → el nativo recibía siempre `false`. Afectaba a los 7
`()Z`/`(I)Z` que pide el .so (barrido `so_annotate.py jni`): setCurrentContext,
isHighPhoneGroup, isLowPhoneGroup, checkFirmware (¡false = firmware no
soportado!), isWifiEnabled, isAndroidOrientationAvailable, getPreferenceBoolean.
**Fix:** `METHOD_TYPE_BOOLEAN` + `methodsBoolean[]`.

**Contextos GL extra.** `glitch::task::CCpuTaskHandlerThread::Run` (0x887f10)
llama `App::ReserveContext` (0x84a680) → `AndroidSetCurrentContext(i)` → Java
`eglMakeCurrent(ctx[i])` sobre contextos pbuffer compartidos que crea
`ag.java` según `getNumExtraContext()`. vitaGL tiene uno solo. **Fix:** main.c
hace lo que el Java cuando no puede crear extras: `setNumExtraContext(0)` tras
`getViewSettings` → `ReserveContext` devuelve false sin llamar a Java y el hilo
principal toma el contexto 0 en `App::SetAsMainThread` (setCurrentContext(0)
→ true). `setCurrentContext(i>=1)` loguea y devuelve false.

**Diagnóstico:** heartbeat `[loop] frame N alive.` (frames 1, 30, cada 300)
para distinguir cuelgue de carga lenta en el próximo log.

Próximo log, buscar: `[gl] engine wanted N extra GL context(s)`, que no
aparezca ningún `methodBooleanCall … not found`, y hasta qué frame llega.

**Bug 3 — `Unknown symbol "inet_addr"` (fatal de so_util en lazy binding).**
El juego llegó a código de red. Barrido completo `nm -D libDH3.so` (imports
`U`) contra `dynlib.c`: `inet_addr` era el ÚNICO faltante. **Fix:** agregado
(newlib, misma semántica que Bionic).

**Bug 4 — Undefined instruction: SWP (log_003 + psp2dmp 1790968429, éste SÍ
de DH3).** PC `0xa2c7a4` = `glitch::io::CMemoryReadFile::~CMemoryReadFile+0x5c`
= `swp r3, r6, [r5]` (spinlock de un pool de locks hasheado). El .so usa
atómicos ARMv5: Android emula SWP en el kernel, la Vita no. Ocurrió al cargar
`menus854x480.pak`, todavía dentro del primer `step()`. Hay 178 `swp` inline
(glitch/gameswf), todos `swp r3, Rx, [Ry]` incondicionales, sin `swpb`.
**Fix (patch.c `patch_all_swp`):** cada sitio → `b` a un trampolín en el arena
de so_util con `ldrex/strex` + `dmb`, preservando flags (mrs/msr) y scratch
(push/pop). Escaneo por máscara sobre el segmento RX = exactamente los 178 de
objdump (verificado offline); encodings verificados con objdump. Ojo:
`so_mod.shdr` es dangling en `so_patch` (so_util libera la imagen del archivo).
Esperar en el log: `Patched 178 SWP -> LDREX/STREX.`
También del log: `engine wanted 1 extra GL context(s)` y cero
`methodBooleanCall not found` → el fix de Fase 9 funciona.

**Bug 5 — Data abort en `_lseek_r` (log_004 + psp2dmp 1790972837, DH3).**
SWP fix confirmado (`Patched 178 SWP`), pasó `menus854x480.pak`. Stack:
`basic_filebuf::_M_seek` → `std::__basic_file<char>::seekoff` (0xd91078) →
`lseek` (newlib) → `__vita_fd_grab` NULL → crash. Causa: el libstdc++ del .so
inlinea `__basic_file::fd()` como `ldrsh r0,[FILE,#14]` (layout BSD de Bionic,
`_file`); con `USE_SCELIBC_IO` el `FILE*` es de SceLibc (otro layout) → fd
basura. Mismo patrón en `glf::FileStream::Impl::DiskFileStreamImpl::
ReadWithOffset` (0x842b88: `pread(FILE->_file,…)`, lectura de archivos del
motor). Barrido `ldrsh …#14]` en todo el .so: el resto son structs ajenos +
curl file:// + consola OpenSSL (sin uso). El .so no importa `__srget/__swbuf`
→ no hay macros getc/putc inlineados.
**Fix (`source/reimpl/basic_file.c`):** hooks por dirección (símbolos locales,
guardados por primera instrucción) de fd/xsgetn/xsputn/xsputn_2/seekoff/
showmanyc + ReadWithOffset, reimplementados con stdio sobre el mismo FILE*
(pread = seek/read/seek-back bajo mutex). `sync`/`close`/`open` ya usaban
stdio. Se mantiene SceLibc IO (alternativa descartada: pasar todo a newlib,
cuyo FILE sí tiene `_file@14`, pero cambia todo el IO probado).
Esperar: `Hooked 7 FILE->_file users (basic_file/glf) onto stdio.`

**Bug 6 — `strcpy(dst, NULL)` en `glot::TrackingManager::TrackingManager`
(log_005 + psp2dmp 1790973262).** Hooks de FILE y SWP confirmados en el log.
LR `0xcbab90`: `strcpy(this+205, deviceUDID)` con el global `deviceUDID`
(.bss 0xfdd78c, vía GOT 0xfc719c) en NULL; la instrucción siguiente hace
`strcpy(this+238, glot::GetMacAddress())`, que devuelve el global de
`nativeGetMAC` (mismo slot GOT 0xfc71a0) → también NULL. Esos globals los
llena Java en `GLGame.onResume` (GLGame.java:955) con
`nativeGetUDID/GetMAC/GetDeviceModel/GetDeviceFirmare(String)`; cada native
sólo guarda el puntero de `GetStringUTFChars`. **Fix (main.c
`dh3_send_device_info`, tras `GLGame_Init`):** UDID = OpenPSID en hex
(estable por consola), MAC `02:00:00:00:00:00`, modelo `PS Vita_sony`,
firmware `4.4`. Esperar: `[Java] device info sent (udid=…)`.
Revisados los demás natives de ciclo de vida (setNetworkState,
orientationChanged, setCheckNexusDevice): dependen de eventos y su default
0 (offline) sirve.

## Fase 10: primer render loop (2026-10-02, log_006)

Device info OK (`udid=…`). El juego entra al loop (`frame 1` a 25.8 s,
`frame 30` a 40 s), pantalla negra a ~2 FPS, crash a 80 s.

**Bug 7 — crash en `glLinkProgram` (psp2dmp 1790980944).** PC en
`sceGxmProgramGetParameterCount(NULL)` ← vitaGL `glLinkProgram`. Con
`VGL_MODE_POSTPONED` vitaGL traduce+compila los shaders recién en el link;
si shacccg falla `prog` queda NULL y sigue de largo (la libvitaGL.a del
sistema está compilada sin logs: el error no se veía en ningún lado, y el
chequeo de status en `glCompileShader_soloader` nunca podía dispararse).
**Fix:** vitaGL vendorizado en `lib/vitagl` (árbol de DH2) con parche [DH3]
en `compile_shader` (ver `lib/vitagl/VENDORED.md`): loguea
`[shader] … FAILED` + error de shacccg, guarda la fuente CG traducida en
`ux0:data/dungeonhunter3/shader_fail/NNN_{vs,fs}.cg` y compila un sustituto
(magenta) para que el programa linkee. Flags = base de DH2 + HAVE_SHARK_LOG
(incluye HAVE_SHADER_CACHE → `ux0:data/shader_cache/PSVDH0003/`: el 2º
arranque no recompila). `DUMP_COMPILED_SHADERS` forzado OFF (nunca escribió
nada con POSTPONED). EGL faltante (antes venía del egl.c de la vitaGL del
sistema) agregado a `reimpl/egl.c`; libDH3 no importa EGL.
**Próximo log:** buscar `[shader] … FAILED` y traer el `.cg` de
`shader_fail/` para corregir el shader real (fix en `glShaderSource_soloader`).

**Rendimiento — el logging era el cuello de botella.** 886 líneas entre
frame 1 y 30 en 14.2 s ≈ 16 ms/línea: un `sceIoWrite` síncrono por línea a la
memory card. Logger ahora con buffer RAM de 16 KB (flush inmediato en
error/fatal/success, al pasar 12 KB, o cada 250 ms; `log_flush_if_due()`
por frame). FalsoJNI: `method ID N not found` y `GetStringUTFChars(NULL)`
limitados a 20 + 1 cada 1000 por sitio (eran ~1600 líneas).

**Savegames.** libDH3 tiene hardcodeado `/data/data/<pkg>` (perfil
`dh3_profile.savegame[.bak]`, `DebugSwitches.savegame`, también como
`./data/data/...`): ~80 fopen fallidos. `fix_path()` (io.c) lo mapea a
`ux0:data/dungeonhunter3/files` en fopen/open/stat/opendir/mkdir/remove/
rename/unlink/chmod.

## Fase 11: carga de nivel (2026-10-02, log_007)

Llega al loading del personaje (warlord, armas). Guard de shaders OK: 3
fragment shaders fallaron y se usó el sustituto en vez de crashear.

**Bug 8 — `undeclared identifier 'BlendRatio'`.** `GL_Diffuse_L1_iPhone_FS`
y `CharNormalSpec_iPhone_FS`, variantes SP+TB: `BlendRatio`, `Specular2`,
`Specular3` no se declaran en ningún lado (el blend TB es código muerto
también en el cuerpo); ningún compilador GLES lo aceptaría. **Fix:**
`dh3_fix_shader_source()` en `glShaderSource_soloader` borra (con espacios,
mismo largo) esas dos sentencias → queda el único specular que se muestrea.

**Bug 9 — data abort en `COnDemandReader::read` (psp2dmp 1790986661).**
`this->file == NULL`. `CAnimationBlock` re-abre el `.bdae` para streaming
como `dirname(nombre) + "/" + nombre`; un archivo del pak se llama sólo por
su entrada (`openFile(int)` → `createLimitReadFile(entry->name)`), así que
pide `/warlord_male_menu_idle.bdae`. `CCustomPakReader::findFile` con
ignorePaths saltea hasta la última '/' **salvo si está en el índice 0**
(`cmp r4,r7; addne r7,#1`) → no lo encuentra; el fallback a disco hace
`fopen("/warlord_male_menu_idle.bdae")` → NULL. Nombre recuperado de la
memoria del dump (la línea de log seguía en el buffer RAM).
**Fix (`source/reimpl/pak.c`):** `findFile` reimplementado igual que el
original (flags +300/+301, copia en minúsculas, lower_bound + strcmp) pero
quitando también una '/' inicial y con reintento por basename (el pak es
plano). `COnDemandReader::read` (ambas sobrecargas) con guard: sin archivo
→ log + buffer en cero.

**Logger:** los `warning` ahora también se vuelcan al instante (la pista
de este crash se había quedado en RAM). "doubled root collapsed" pasa a
debug (es rutina del motor).

**Video intro.** `res/raw/logo.mp4` (72 KB) viene en el APK pero v1.5.2
nunca lo reproduce (`SUtils.playVideo` = `return false`, nadie referencia
`R.raw.logo`; Android muestra el drawable `static_logo`). A pedido del
usuario se reproduce como splash del port antes de arrancar el motor:
`source/video.cpp` copiado de DH2 (SceAvPlayer + conversión YUV→RGB en GPU
con vitaGL, método de Asphalt 5; el de Shadow Guardian es para PVR_PSP2).
Adaptado: sin `<GLES2/gl2.h>` (choca con vitaGL.h), viewport fijo 960x544
(DH3 no tiene DOWNSAMPLE_RENDER), sin el contador del watchdog de DH2.
Salteable con Cross/Start. `SceAvPlayer_stub` agregado al link.

## Fase 12: límite de FILE de SceLibc (2026-10-02, log_008)

Video arranca, shaders OK (`patched undeclared BlendRatio`, 0 FAILED),
carga hasta trickster/enemigos (giantcrab).

**Bug 10 — data abort en `CGlfReadFile::getPos` (psp2dmp 1790988010).**
`ondemand_read` → `CLimitReadFile::seek` → `CGlfReadFile::getPos` con el
stream interno (`+12`) NULL: el `fopen` del OBB devolvió 0x0. Conteo del
log: **60 FILE vivos, 59 del OBB**. `CCustomPakReader::openFile(int)` abre
el OBB por cada asset (787 fopens) y los lectores de streaming los
retienen; la tabla de FILE de SceLibc es fija (Android: ~1024 fds).
**Fix (`source/reimpl/vfile.c`):** `fopen` de `*.obb` en modo lectura →
handle virtual de un pool estático (512): un único fd `sceIoOpen`
compartido por ruta, lecturas con `sceIoPread` (posicional, thread-safe),
posición/eof/error propios y read-ahead de 16 KB por handle. Todos los
imports stdio del .so que reciben FILE* (fread/fseek/ftell/feof/ferror/
fflush/fgets/fgetc/getc/ungetc/setvbuf/fileno/fwrite/fputc/putc/fputs/
fprintf/vfprintf/fclose + rewind/fseeko/ftello) pasan por dispatchers que
reconocen el handle por rango de dirección; el resto va al stdio real.
`basic_file.c` (incluido `ReadWithOffset`) usa los mismos dispatchers.
De paso: `rewind`/`fseeko`/`ftello` estaban mapeados a newlib con FILEs
de SceLibc (bug latente).

**Video:** `logo.mp4` es MPEG-4 Part 2 Simple Profile → SceAvPlayer lo
rechaza (STATE_STOP tras leer el header, 0 frames). Transcodificado a
`res/raw/logo_vita.mp4` (H.264 Main 960x544, sin audio como el original;
comando en main.c), subido por FTP y en `ux0_data/`.

## Fase 13: stack smash en FileSystemBase + allocator de video SG (2026-10-03, build verde sin consola)

**Bug 11 — stack smash en `FileSystemBase::customCreateAndOpenFile`
(log_009 + psp2dmp 1790991269).** Diagnóstico por desensamblado ARM real
(sin Ghidra para libDH3, con `arm-vita-eabi-objdump` en modo ARM — la zona
es ARM, `force-thumb` la decodifica como basura):
`customCreateAndOpenFile` (0x610eb4) arma la ruta en un buf de 512 bytes
en stack (`sub sp,#524`, buf en sp+4, canario en sp+516) con `strcpy`
(0x610ef8, sin cota) + `strcat` de un sufijo vía GOT solo si el nombre
termina en "swf" (0x61104c, sin cota). A los 33.2 s el canario falló
(`Stack collapsed at 0x98611058` = retorno del `bl __stack_chk_fail`
de 0x611054). La causa visible era engañosa: `__stack_chk_fail_soloader`
solo logueaba y RETORNABA, así que la ejecución se deslizó por el word
de datos 0x611058 hasta `FloorMeshSceneNode::renderCompile+4`
(`ldr r3,[r0]` con r0=0 → el Data abort del dump). Ninguna ruta logueada
pasa de 107 chars, así que la entrada que desborda nunca llega a
nuestros wrappers de fopen/stat — muere dentro de esta función.
**Fix (`source/reimpl/fsbuf.c`, nuevo, llamado desde `so_patch()`):**
los dos `bl` se redirigen (trampolín absoluto en el arena de so_util, un
`bl` directo no llega del .so al loader: ±32 MB) a `dh3_strcpy512` /
`dh3_strcat_lim`, ABI-idénticas a libc pero acotadas a 511 + loguean
`[fsbuf] ... saturated ...` (longitud + cabeza del path) las primeras
8 veces. El motor recibe el mismo "archivo no encontrado" gracioso que
ya tolera en todas partes. Además `__stack_chk_fail_soloader`
(`source/reimpl/sys.c`) ya no retorna: loguea + `abort()`, así el dump
conserva los frames reales en vez de deslizarse.
Esperar en el log: `Guarded customCreateAndOpenFile 512-byte stack buf...`
al arrancar; si el path culpable aparece, una línea `[fsbuf] ...` con su
longitud real (ese dato decide el fix definitivo). Si vuelve `Stack
collapsed` SIN línea `[fsbuf]` previa, el escritor es un callee, no el path.
Build verde (`psvita-toolkit build`), sin warnings en fsbuf.c/video.cpp/sys.c.

**Video intro — técnica Shadow Guardian (misma falla, fix probado).**
`logo_vita.mp4` local (10594 bytes) es válido (ffprobe: H.264 Main
960x544, 76 frames, 3.04 s, yuv420p) — el archivo NO es el problema.
El problema es idéntico al que SG diagnosticó en hardware con su
`logo.m4v`: vitaGL ya consumió la CDRAM antes de `video_play()`, así que
el request de 1.5 MB falla en CDRAM (0x80024309, NO_FREE_PHYSICAL_PAGE)
y el fallback a PHYCONT reuseaba el `opt` de CDRAM → 0x80020005
(INVALID_FLAGS, PHYCONT exige size redondeado a 1 MB y opt NULL) →
AvPlayer aborta con 0 frames. **Fix (`source/video.cpp`):** allocator de
3 niveles copiado de `Shadow-Guardian-vita/source/video.cpp` (SG lo
confirmó en consola: video nítido): CDRAM → PHYCONT (size a 1 MB, opt
NULL) → UNCACHE (patrón SDL Vita GXM) + guard de slots + log del tipo
usado; `VIDEO_DOWNSAMPLE_UPLOAD` 1→0 (el downsample 2x + upscale GL_LINEAR
era el blur del intro en SG; a 960x544 full-res el costo extra de subida
dura ~3 s una sola vez).
Esperar en el log: `video: texture memblock ok (PHYCONT|UNCACHE ...)`
(l_info) y `video_frames > 0` con `first video frame decoded (960x544)`.

Para la próxima corrida en consola: instalar `build/dungeonhunter3.vpk`
de nuevo (cambió el loader) y traer `logs/log_010.log`.

## Fase 14: bug en el trampolín de fsbuf + tier-4 heap para video (2026-10-03, build verde sin consola)

Log_010 confirmó la mitad de Fase 13: `Guarded customCreateAndOpenFile...`
OK y el tier-1 de video ahora da `memblock ok (UNCACHE)` para el request
de 1.5 MB. Pero AvPlayer pide acto seguido un segundo buffer de
**~14 MB con align 1 MB** que falla en los tres pools (0x80024302) →
sigue en 0 frames. Y el juego moría a los 5.27 s, antes que en log_009.

**Bug 12 — typo mío en el trampolín de fsbuf (regresión).** Los dos `ldr`
iban a `[sp,#8]`/`[ip,#8]` (words 0xE59DE008/0xE59CC008, Rn=13/12) en vez
de `[pc,#8]` (Rn=0xF): la primera llamada a customCreateAndOpenFile
cargaba basura en lr/ip → Data abort con PC salvaje (0x97ff1e9c, fuera de
todo módulo) y lr=0. Words corregidos a 0xE59FE008/0xE59FC008 (verificado
contra el `ldr r6,[pc,#404]` = 0xE59F6194 del propio motor), con comentario
de advertencia y log debug con site+tramp para futuros dumps.

**Video, tier 4.** Los logs sanos de SG muestran el patrón de AvPlayer:
1 MB + frames + 5 MB (su video es 480x272), todo servible por UNCACHE.
Nuestro 960x544 pide ~14 MB, que excede UNCACHE. Nuevo último nivel en
`av_alloc_texture()`: heap (`memalign` + `sceGxmMapMemory` OBLIGATORIO —
si el map falla se devuelve NULL como antes, para que la GPU nunca vea un
puntero sin mapear; peor caso = el skip limpio actual). La tabla de slots
lleva `is_heap` y `av_free_texture()` hace unmap+free en ese caso.
Si el heap tampoco sirve (map falla o 0 frames igual), el plan B es
re-transcodificar el logo a 480x272 (tamaños probados por SG) en vez de
pelear por 14 MB contiguos.

Esperar en log_011: `memblock ok (HEAP...)` + `first video frame decoded
(960x544)` con el reporte del plano Y; pasar los 5.27 s sin crash; y la
línea `[fsbuf] ... saturated ...` con el path culpable del Bug 11 (o juego
avanzando más allá de los 33 s de log_009).

## Fase 15: select de personaje sin texturas + elementos faltantes (log_011, build con diag)

Log_011: el juego corre hasta frame 600+ (select de personaje), sin crash
y sin `Stack collapsed` (el trampolín de fsbuf corregido aguanta; ningún
`[fsbuf] saturated` = el path de 512 del Bug 11 no apareció en este
recorrido). Sin ningún `glLinkProgram/glCompileShader FAILED` — shaders OK.
Video: el tier-4 heap se intentó pero `sceGxmMapMemory` lo rechazó
(0x805B0017) → intro saltada como antes (plan B pendiente: logo a 480x272).

Descartado con evidencia que sea falta de archivos: el OBB trae 1968
`.tga` (cero `.pvr/.etc/.dds/.ktx` → `getTexturePack=0` es correcto) y los
`_nm.tga` que faltan también faltarían en Android (el motor los sintetiza
`<material>_nm.tga` y solo existen 2 en todo el OBB) — el motor tolera su
ausencia. Los difusos (`background_selectcharac.tga` 2048x2048x32, etc.)
SÍ están y se resuelven por basename.

Como en DH2, se instrumenta en vez de adivinar (`source/utils/glutil.c`,
temporal): `glTexImage2D/compressed/SubImage/TexParameteri/GenerateMipmap/
PixelStorei` loguean parámetros (format/type/tamaños/filtros, primeros
N + contadores) SIN llamar a `glGetError` — consumirlo rompería la
detección propia del motor en `CTexture::updateData` (0xa78f3c, que drena
el error antes de subir y marca la textura con bit 16 si falla).
Verificado: el motor NO importa `glTexParameterfv` (su `ret0` en dynlib
es inocuo). Remapeos en `source/dynlib.c`. Build verde.

Próximo log (log_012), buscar las líneas `[texdiag]`: formatos/tipos
subidos, si hay niveles > 0 (mipmaps), valores de MIN_FILTER (0x2801:
0x2601 = LINEAR sin mipmaps es lo sano; 0x2700-0x2703 con cero
`glGenerateMipmap` = textura incompleta = negro), y tamaños NPOT.

## Fase 16: cuerpo del personaje sin textura (foto 2026-10-02-222241)

La foto del select (build anterior, sin diag) acota el bug muchísimo:
fondo con textura completo, UI/botones/textos perfectos (stencil y GameSWF
OK), arma del mago CON textura... pero el CUERPO del personaje es geometría
plana gris sin difuso. No es falta de archivos (los `_dif` del trickster/
astromancer existen: 2.8 MB RLE) ni de shaders (nada magenta, ningún FAILED)
ni de culling (la geometría sí se dibuja). Candidatos restantes: la subida
de esa textura falla en vitaGL, o el material multicapa (dif+spec+opa)
muestrea la unidad equivocada. El diag de Fase 15 se amplió con
`glBindTexture` (primeras 40, DH2 cazó así su anomalía de "2 texture IDs")
+ `glActiveTexture` (primeras 10) antes de pedir el log_012.

## Fase 17: crash en createMaterialVertexAttributeMap + ETC1 confirmado (log_012)

El diag funcionó a la primera: el motor sube texturas **ETC1** (0x8D64:
1024x1024→524288 bytes, 256x256, 2048x2048 = justo w*h/2) con MIN_FILTER
LINEAR (0x2601, sin mipmaps — nada de textura incompleta) en unidades 0 y
7. Los `.tga` del OBB son contenedores de ETC1 (por eso `file` los
confunde). ETC1 por hardware en vitaGL queda como hipótesis del cuerpo
gris — pendiente del log con más binds.

**Bug 13 — Data abort en `CColladaFactory::
createMaterialVertexAttributeMap`+0x164 (psp2dmp 1791002796, 43.7 s,
cargando warlord).** `ldr r3,[r0,#8]` con r0=0 (slot [sp+36] que una
llamada virtual vtable+20 dejó en NULL — capa de lightmap ausente:
`atlas_cliff_lightmap2/3.tga` no existen en el OBB). La instrucción
siguiente (`cmp r3,#0` → `beq` a la ruta "capa ausente") prueba que r3==0
es un estado legal para el motor; solo faltaba tolerar r0==0.
**Fix (`source/reimpl/material.c`, llamado desde `so_patch()`):** la
palabra en 0x878378 se cambia por `b` a un trampolín de 4 words sin
literales (`cmp r0,#0` / `ldrne r3,[r0,#8]` / `moveq r3,#0` / vuelta a
0x87837c; solo toca r3+flags, que 0x87837c restablece). Nibbles de
condición verificados contra el propio motor (`addne`/`beq`).
Build verde. Si log_013 pasa los 43.7 s, confirmado.

## Fase 18: crash en CVertexAttributeMap::set (log_013 + psp2dmp 1791003100)

**Bug 14 — Data abort en `CVertexAttributeMap::set`+0x28 (PC 0x9896d018, LR 0x98878448).**
El dump de log_013 confirma que el fix de Fase 17 en 0x878378 funcionó: `r0 == 0`
produjo limpiamente `r3 = 0` y se guardó en `[sp + 32]` como puntero a `CVertexStreams`.
Pero más abajo en esa misma función (`createMaterialVertexAttributeMap` a 0x878444):
itera sobre las técnicas del material y llama directamente a `CVertexAttributeMap::set`
pasando `r1 = sp + 32` (`*r1 == 0`).

Desensamblado de `CVertexAttributeMap::set` (0x96cff0):
```arm
  96cff0: push {r4, r5, r6, r7, r8, r9, sl, fp}
  ...
  96d008: ldr  r9, [r1]        ; r9 = *r1 = 0
  ...
  96d018: ldr  r8, [r9, #16]   ; <- DATA ABORT: dereferencia r9 == 0 (PC 0x9896d018)
```
El propio constructor de `CVertexAttributeMap` (0x96cf58) ya tenía un chequeo explícito:
`ldr r3, [r5]; cmp r3, #0; beq 96cf80` que saltea la llamada a `set()`. Pero
`createMaterialVertexAttributeMap` llama a `set()` directamente sin chequear `*r1 != 0`,
y `set()` no validaba el puntero antes de acceder a `r9[16]`. Al no haber streams,
no hay atributos que mapear y la función debe retornar sin hacer nada (dejando el mapa
con los valores 0xff por defecto inicializados por el constructor `CVertexAttributeMapC1`).

**Fix (`source/reimpl/material.c`, llamado desde `so_patch()`):**
Trampolín de 7 palabras en 0x96cff0 (`so_alloc_arena`):
```arm
  cmp  r1, #0
  bxeq lr
  ldr  r12, [r1]
  cmp  r12, #0
  bxeq lr
  push {r4, r5, r6, r7, r8, r9, sl, fp} ; instrucción original
  b    0x96cff4                         ; vuelve a CVertexAttributeMap::set
```
Si `r1 == NULL` o `*r1 == NULL`, retorna inmediatamente vía `bx lr` sin tocar registros
ni la pila. Si `*r1 != NULL`, ejecuta el `push` original y salta a 0x96cff4.

Build verde (`psvita-toolkit build`), VPK generado y listo para deploy.

## Fase 19: Teclado virtual PS Vita (IME Dialog) y compatibilidad de texturas GLES (log_014)

**Problema 1 — Teclado virtual / IME colgado en la pantalla de selección de nombre (captura 2026-10-03-011411.jpg):**
Al llegar a la selección de personaje, el juego muestra el cartel *"Introduce un nombre para tu personaje"*, pero el teclado virtual de la consola nunca aparecía ni respondía.
Causa raíz:
1. `gl_swap()` en `source/utils/glutil.c:48` llamaba a `vglSwapBuffers(GL_FALSE);` de forma fija. En vitaGL (`lib/vitagl/source/gxm.c:805`), `sceCommonDialogUpdate(&updateParam)` **únicamente** se invoca cuando el parámetro `has_commondialog` es `GL_TRUE`. Al pasar siempre `GL_FALSE`, el sistema operativo de la PS Vita jamás procesaba ni renderizaba el diálogo IME en pantalla.
2. `init_ime_dialog_ex` se invocaba con `""` en lugar de un nombre por defecto.
3. Si el usuario cancelaba o se devolvía un string vacío, `EditorEnd` nunca se llamaba (`if (text[0] && EditorEnd)...`), dejando el motor en espera permanente de confirmación del teclado.
En `asphalt8-vita-main`, `gl_swap(keyboard_blocks_controls)` pasa `keyboard_blocks_controls ? GL_TRUE : GL_FALSE` a `vglSwapBuffers`.

**Fix aplicado:**
- `source/utils/glutil.h` y `source/utils/glutil.c`: `gl_swap(int has_common_dialog)` pasa `has_common_dialog ? GL_TRUE : GL_FALSE` a `vglSwapBuffers`.
- `source/main.c`:
  - Nombre por defecto `"Hero"` configurado como texto inicial en `init_ime_dialog_ex`.
  - Fallback inmediato: si `init_ime_dialog_ex` falla o si el diálogo termina con string vacío, se envía `"Hero"` a `EditorEnd(&jni, OBJ_GLGAME, (jstring)"Hero")` y se llama a `HideKeyboard` para permitir que el juego avance.
  - Mientras `ime_active` esté activo, el bucle llama a `gl_swap(1)`. En frames normales llama a `gl_swap(0)`.
- `source/video.cpp`: llamada a `gl_swap()` actualizada a `gl_swap(0)`.

**Problema 2 — Texturas y compatibilidad GLES (`glTexParameterfv`, `glGetTexParameter*`, `glIsBuffer`):**
En `source/dynlib.c`, `glTexParameterfv`, `glGetTexParameterfv`, `glGetTexParameteriv` y `glIsBuffer` estaban mapeados directamente a `&ret0`. Además, en `glutil.c`, `glCompressedTexImage2D_soloader` tenía un límite de 15 llamadas que censuraba el diagnóstico a partir del segundo 23.
- Se implementó `glTexParameterfv_soloader` redirigiendo a `glTexParameterf(target, pname, params[0])` (idéntico a `asphalt8-vita-main`).
- Se implementaron `glGetTexParameteriv_soloader` y `glGetTexParameterfv_soloader` devolviendo valores válidos según `pname` (`GL_NEAREST_MIPMAP_LINEAR`, `GL_LINEAR`, `GL_REPEAT`).
- Se implementó `glIsBuffer_soloader(GLuint buffer) { return buffer != 0 ? GL_TRUE : GL_FALSE; }`.
- Se removió el tope de 15 llamadas en `glCompressedTexImage2D_soloader` y `glTexImage2D_soloader`, logueando todo upload de nivel 0 con formato, resolución y tamaño en bytes.

Build limpio verificado con `psvita-toolkit build` (`dungeonhunter3.vpk`).



## Fase 20: tutorial bloqueado en el touchpad, video intro sin memoria, ETC1 (log_015)

**Problema 1: el tutorial se queda esperando el "centro del panel táctil".**
En modo Xperia Play el tutorial (`tutorial.spanish`) pide *"Toca el centro del panel
táctil para cambiar a tu otra arma"*. Ese panel es el touchpad del Xperia
(`nativeAdd/Update/RemoveTouchEvent` → `Application::UpdateTouchPad` 0x6b6d4c),
que tiene 3 zonas por x: `<=366` izquierda, `>=600` derecha, `387..575` **centro**
(cambiar de arma). El port solo emulaba los sticks (zonas izq/der); a la zona central
no llegaba nada, y ni el panel trasero ni L2/R2 estaban conectados.
**Fix (`source/main.c`):** el panel trasero se mapea completo al touchpad 966x360
(`rear_to_pad`, ids 2..5, área activa de `sceTouchGetPanelInfo`), así que tocar el
centro de la parte de atrás = cambiar de arma, y los costados = mover/atacar.
L2/R2/L3/R3 (PSTV, o plugins que remapean el panel trasero a L2/R2) = pulsar el
centro del pad (id 9, x=481).

**Problema 2: el video intro no arranca.** AvPlayer pide 14 MB alineados a 1 MB;
CDRAM/PHYCONT/UNCACHE ya son de vitaGL y el fallback a heap fallaba en
`sceGxmMapMemory` con 0x805B0017 (SCE_GXM_ERROR_DRIVER): vitaGL **ya mapeó todo el
heap de newlib** a GXM en `vgl_mem_init` (VGL_MEM_EXTERNAL), y volver a mapear un
subrango falla aunque la memoria sí es visible para la GPU.
**Fix (`source/video.cpp`):** `av_heap_is_gxm_mapped()` comprueba que el bloque esté
dentro del memblock del heap y entonces no se re-mapea (ni se desmapea al liberar).

**Problema 3: texturas rotas.** 108 de las 114 subidas son ETC1 (`0x8d64`, ~35 MB),
que vitaGL mandaba al decodificador ETC1 **por hardware** de GXM.
**Fix:** vitaGL compilado con `SAFE_ETC1=1` (decodificación por CPU) +
`glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST)` en `gl_init()` para recomprimir a
DXT1 (4 bpp, misma memoria que ETC1; sin esto serían RGB888, x6). Parche [DH3] en
`lib/vitagl/source/textures.c`: los mip levels >0 de ese camino le pasaban RGB888
a `dxt_compress` (que espera RGBA8888) y los mips <4 px no generaban ningún bloque;
ahora se convierten y rellenan como el nivel 0. `texdiag` ahora también registra
los primeros uploads con lvl>0.
Pendiente de confirmar en consola: aspecto de las texturas y tiempo de carga
(la recompresión DXT1 corre en CPU al cargar).

Build verde (`psvita-toolkit build`).

## Fase 21: crash en strcmp desde constructGeometry (log_016 + psp2dmp 1791162004)

Ojo: `psvita-toolkit analyze` auto-detectó la base del .so en 0x982eb000 y
simbolizó mal (LR "GameObject::_HasPath"). La base real sigue siendo 0x98000000
(como en Fase 17/18): LR 0x988e9cf0 = `CColladaDatabase::constructGeometry`+0x54,
justo después de `bl strcmp`, con r1 = id de geometría = 0x1.

**Bug 15 — use-after-free del nombre de un .bdae streameado.**
Cadena: `CSkinnedMesh` reabre el .bdae on-demand por el nombre guardado en
`[SCollada+4]` (un `std::string` que `postLoadProcess` 0x8f3d7c crea con `new`).
En log_012/013/016 ese "nombre" es **código fuente de un shader**
("fine add / uniform lowp sampler2d diffusesampler..."), o sea memoria liberada y
reutilizada → el open falla → `COnDemandReader` devuelve ceros → constructGeometry
compara un id basura → data abort. Los offsets pedidos (pos=52340 size=14812) son
idénticos en todos los runs: los datos del modelo están intactos, solo muere el
nombre. Su único `delete` está en `CResFile::releaseObjects` (0x8f2de8, llamado solo
desde `~CResFile`), que no pone el puntero en NULL; un .bdae cargado dos veces
comparte ese slot y el primer `~CResFile` libera el string del sobreviviente.
log_015 pasó de casualidad (la memoria liberada todavía no se había reutilizado).
**Fix (`source/reimpl/pak.c`):** `beq` → `b` en 0x8f2e00: releaseObjects ya no
borra el nombre (se fugan unos bytes por .bdae streameado descargado).

**Video intro (sigue de Fase 20):** con el fallback a heap AvPlayer obtiene los
14 MB pero igual hace STATE_STOP con 0 frames (el decodificador por hardware quiere
memoria físicamente contigua), y ese bloque no se liberaba.
**Fix (`source/utils/glutil.c`, `main.c`):** `vglInitWithCustomThreshold` con
umbral PHYCONT = todo (vitaGL no toma PHYCONT al iniciar); después del video,
`gl_phycont_late_init()` le pasa a vitaGL el PHYCONT libre (`vglPhycontMemLazyInit`).

Build verde.

## Fase 22: el nombre streameado sigue corrupto (log_017 + psp2dmp 1791162883)

Con el `delete` de `releaseObjects` anulado (Fase 21) el crash es idéntico: el
nombre que usa `CSkinnedMesh` para reabrir el .bdae sigue siendo texto de shader.
Así que Fase 21 no era la causa (o no la única). Revisé todas las fuentes posibles
del nombre (`CLimitReadFile`, `CMemoryReadFile`, `CGlfReadFile`) y todas lo
copian; el único `delete` de `[SCollada+4]` en el .so es el ya anulado. El dump
confirma SCollada=0x829c2728 y datos del modelo sanos ("MC_Back_01-mesh", la capa),
pero no incluye la página del string. El id que crashea es `skin->sourceUrl + 1`
(`instanciateMesh` 0x882b00) = 0 + 1 con el skin en ceros.
**Fix (`source/reimpl/pak.c`):**
- Trampolín en `postLoadProcess` 0x8f3d90: `[SCollada+4]` recibe una copia propia
  inmortal (rep COW de libstdc++ con refcount 0x3FFFFFFF, nunca liberada). Loguea
  el nombre y marca con error si ya llega corrupto o distinto de `getFileName()`:
  así el próximo log dice si se corrompe antes o después de la carga.
- Guarda en `constructGeometry` 0x8e9c9c: id < 0x10000 → `*sret = NULL` (la salida
  "no encontrado" del propio motor, que `instanciateMesh` ya maneja).

**Video intro:** con PHYCONT (Fase 21) AvPlayer recibe la memoria pero igual STOP
con 0 frames → el problema era el archivo: H.264 Main con B-frames y sin audio.
Re-transcodificado a baseline, sin B-frames, con AAC silencioso 48 kHz
(receta en `main.c`). Hay que volver a copiar `res/raw/logo_vita.mp4` a la Vita.

Build verde.

## Fase 23: el nombre ya llega corrupto al cargar → emulación de pthread rota (log_019 + psp2dmp 1791164950)

El diagnóstico de Fase 22 dio la respuesta: `streamed name already bad at load` en el
**primer** .bdae streameado. `getFileName()` del propio IReadFile (un CLimitReadFile
del OBB, que copia su nombre al construirse) ya devuelve texto de shader. Es decir:
el string de un objeto **vivo** fue liberado y reutilizado. Todos los caminos del
motor copian el nombre y el refcount COW es atómico (`ldrex/strex`), así que el
problema es de concurrencia y no del motor.

**Causa (`source/reimpl/pthr.c`):** la emulación de mutex/cond de bionic usaba una
tabla global `initializedObjects[1024]`:
- con >1024 mutex/conds vivos (glitch crea uno por objeto), `rememberObject()`
  fallaba en silencio y **cada `lock()` creaba un mutex nuevo** sobre `real_ptr`:
  ningún lock excluía nada;
- la inicialización perezosa era check-then-act: dos hilos podían instalar cada uno
  su propio mutex;
- un objeto nuevo en una dirección reciclada se veía como "inicializado" con
  `real_ptr == NULL` → lock = EINVAL = sin exclusión.
Con los hilos de carga de glitch (CCpuTaskHandler) eso da carreras en el heap /
strings → nombres liberados, skins en cero, crashes en collada.
**Fix:** sin tabla. El puntero al objeto real vive en la palabra de 4 bytes de bionic;
"inicializado" = parece puntero (≥ 0x10000; los inicializadores son 0/0x4000/0x8000).
Init perezoso con CAS atómico (el perdedor libera el suyo); `init` explícito siempre
crea uno nuevo; `destroy` con exchange atómico.
También: `pthread_once` ahora espera a que la rutina **termine** (antes un segundo hilo
seguía con la init a medias), y `sem_timedwait` tenía la condición invertida
(devolvía error justo cuando todavía quedaba tiempo).

**Crash de este dump:** la guarda de `constructGeometry` funcionó; el mismo skin en
cero pegó en `constructController` 0x8ece70 (LR 0x8eced4, r1 = 0x1). Misma guarda
(id < 0x10000 → `*sret = NULL`; `instanciateMesh` maneja NULL en 0x882f70). Ojo: el
camino "no encontrado" propio de constructController desreferencia NULL, por eso la
guarda sale antes del push.

Video: log_019 sigue leyendo el `logo_vita.mp4` viejo (10594 bytes): falta copiar el
re-transcodificado de Fase 22 a la Vita.

Nota toolkit: después del build, `psvita-toolkit build` puede reportar
`build/._dungeonhunter3.vpk` (basura AppleDouble del disco externo);
`psvita-toolkit clean-junk` antes del deploy.

Build verde.

### Fase 23b: revisión (2026-10-05)

Re-triage del mismo log_019 + psp2dmp 1791164950: corresponde a un build **anterior**
a Fase 23 (el log solo muestra la guarda de `constructGeometry`, no la de
`constructController`). Confirmado contra el desensamblado: en `constructController`
r0 = sret y r3 = id, el strcmp de 0x8eced0 recibe r1 = r9 = id = 0x1. La guarda está bien.
Extra en `pthr.c`: `sem_trywait` ya no bloquea 1 ms (`sceKernelPollSema` + `EAGAIN`) y
`sem_getvalue` ya no hace malloc (y pierde el valor) cuando `sval` es NULL.
Build verde. Falta probar en consola (log_020).

## Fase 24: pthread no era la causa; namewatch + reparación del nombre (log_020 + psp2dmp 1791235976)

log_020 (build con Fase 23 completa) sigue dando `streamed name already bad at load` en el
**primer** .bdae streameado (55.05 s): el arreglo de pthread no era la causa.
**Crash:** PC 0x882ba8 en `CSkinnedMesh::instanciateMesh`. Con la guarda de Fase 23,
constructGeometry → NULL (0x882b24 → 0x882f30) y constructController → NULL; `beq 0x882ba4`
(0x882fb0) relee `[this+0x40]` = NULL y llama por su vtable. El camino "no encontrado" del motor
tampoco es seguro: seguir agregando guardas solo tapa síntomas.
(Ojo: `psvita-toolkit analyze` auto-detectó la base del .so en 0x97bc0000 y resolvió mal todo
(“glvc::CSocket::ReceiveFrom”); la base real es fija, `LOAD_ADDRESS` 0x98000000. Quedó
`"so_base": "0x98000000"` en `.psvita-toolkit.json`.)

**Descartado (estático):**
- Todas las clases de archivo copian el nombre al construirse (`basic_string(const char*)` 0xa4afa8):
  CLimitReadFile +12, CMemoryReadFile +32, CGlfReadFile +16, CUnzipReadFile +24.
- `GlitchAlloc/GlitchFree/CustomAlloc/CustomFree` = malloc/free puros; no hay mezcla de allocators.
- `CCustomPakReader::scanLocalHeader`: los nombres viven en un único `new[]` en `[pak+284]`;
  `openFile(int)`: flag 1 = XOR → `createMemoryReadFile(buf, size, entry->name, del=1)`,
  si no `createLimitReadFile(entry->name, file, size)`.
- Sin kuser helpers (0xffff0fc0); el parche SWP es el `spinlock_pool<41>` de boost.
- El parche de shaders (`dh3_fix_shader_source`) trabaja sobre una copia.
- El psp2dmp solo incluye las páginas alrededor de los registros: no trae el heap.

**Fix / instrumentación (`source/reimpl/namewatch.c`, nuevo):**
- Wrappers de `createLimitReadFile` (0xa2c4a4) y `createMemoryReadFile` (0xa2cccc) con trampolín
  (las 2 primeras instrucciones no dependen de PC): registran {file, chars del nombre, copia}.
- El import `free` del .so pasa por `dh3_free`: si se libera el rep de un nombre registrado con su
  file todavía vivo (refcount en file+4 > 0 y el file todavía apunta ahí) → `[namewatch] name of a
  LIVE file freed (kept)` con las direcciones de retorno del .so en el stack, y el bloque **no** se
  libera (fuga de unos bytes en vez de memoria reutilizada).
- `dh3_pin_name` (pak.c): si el nombre llega mal, loguea el header del rep `[len cap refcount]` y lo
  repara con la copia de namewatch (`repaired -> '...'`).
- Respaldo: guarda en 0x882fb0: sin geometría ni controller y `[this+0x40]` NULL → sale de
  `instanciateMesh` por su propio epílogo (malla vacía en vez de data abort).

**Qué mirar en log_021:**
- `name of a LIVE file freed` → use-after-free; los "so callers" dicen quién lo liberó.
- `repaired` sin ese mensaje → sobrescritura en el lugar (overflow de un bloque vecino). Ver si el
  header del rep es ASCII.
- `not repaired` → el archivo no viene de esas dos fábricas: hay que ampliar el seguimiento.

Build verde.

## Fase 25: CAUSA RAÍZ del nombre "texto de shader": GetPath() de glf nunca escrito (log_021 + psp2dmp 1791241478)

namewatch (Fase 24) dio la pista: el string corrupto tenía header `[len 0x414, cap 0x804, refcount 0]`.
Es un string COW **válido** de 1044 caracteres, armado con appends; no es memoria liberada. Y
namewatch no lo tenía registrado porque `postLoadProcess` llama al slot 12 de la vtable (0x30) =
**`getFullPath()`**, no `getFileName()` (slot 10). Ese es el string de +16 (CLimitReadFile), no el
de +12.

Cadena completa:
1. `glf::FileStream::Open` (0x843070): para `.obb`/`.sav`, copia la ruta al stack; si mide **> 80**
   (`cmp sl,#80; bgt 0x84324c`) abre desde ahí y vuelve **sin escribir nunca `FileStream+32`**,
   que es el buffer que devuelve `GetPath()` (0x8441c0).
2. FileStream es `new(0x574)` sin inicializar (ctor de CGlfReadFile, 0xa2af00): `GetPath()` = lo
   que había antes en ese bloque del heap. En newlib: código de shader recién liberado.
3. `CGlfReadFile` lo copia como su fullPath (+20). Cada CLimitReadFile del pak arma
   `fullPath = padre->getFullPath() + "/" + nombre` (`init` 0xa5002c) → 1044 chars, cap 0x804.
4. `postLoadProcess` guarda `getFullPath()` como nombre del .bdae streameado → la reapertura falla →
   skin en ceros → constructGeometry/constructController/instanciateMesh/setMaterial crashean.
   La misma basura (>512 chars) es la que reventaba el buffer de stack de
   `customCreateAndOpenFile` (log_009, fsbuf.c).

Nuestra ruta del OBB (`ux0:data/dungeonhunter3/com.gameloft.../main.1501....obb`) mide ~110. En
Android el heap nuevo venía en ceros → `""` → `"/nombre.bdae"`, que `pak_findFile` ya resuelve.

**Fix (`source/reimpl/pak.c`, `patch_glf_long_path`):** trampolín en 0x84324c:
`mov r3,#0; strb r3,[r5,#32]` antes del `ldr r0,[r5,#1392]` original → GetPath() = "".
- Se quitó namewatch.c (diagnóstico, metía un lock en cada `free`).
- Se quitó la guarda de instanciateMesh (Fase 24): solo movía el crash a
  `CSkinnedMesh::setMaterial` 0x883a84 (`[this+52]` NULL con la malla vacía), que es este psp2dmp.
- Pendiente de confirmar en consola: sin `streamed name already bad at load` ni
  `[fsbuf] strcpy saturated`. Si se confirma, las guardas de Fases 21-23
  (releaseObjects, constructGeometry/Controller, fsbuf) quedan como red de seguridad.

Build verde.

## Fase 26: Fase 25 confirmada + L/R sin mapear (log_022)

**Fase 25 confirmada en consola:** log_022 corre 260 s sin crash y sin `streamed name already bad`,
`[fsbuf] strcpy saturated` ni `COnDemandReader::read with no file`. El personaje y el cementerio
cargan bien (screenshots/cf/2026-10-05/2026-10-05-191808.jpg).

**Bloqueo:** el tutorial "Pulsa el disparador derecho para usar una poción" no avanza con R.
- La cadena del juego está bien: keycode 103 → `glf::AndroidOnKeyDown` (0x849c64) → ZeusKeyPad 14 →
  `HUDControls::ProcessZeusGamePad` 0x4b9fc4 → si HP o MP < 100% → `v2Controller::Cmd_UsePotion`
  (0x4baa68), si no `ResetKey(14)`.
- **Causa (`source/main.c`):** el loop lee con `sceCtrlPeekBufferPositiveExt2`, que reporta los L/R
  de la Vita como `SCE_CTRL_L1/R1` (0x400/0x800). El keymap usaba `LTRIGGER/RTRIGGER` (0x100/0x200 =
  L2/R2), así que ni L ni R llegaban nunca al juego.
- **Fix:** keymap con `SCE_CTRL_L1/R1`; si se cae al `sceCtrlPeekBufferPositive` sin Ext2,
  LTRIGGER/RTRIGGER se pasan a L1/R1. L2/R2/L3/R3 (cambiar arma por el panel central) no cambian.

Build verde.

## Fase 27: rendimiento, joystick, guardado y textura del héroe (reporte de usuario tras log_022)

**Rendimiento ~15 FPS — causa encontrada (`source/main.c`):** el loop dormía `33 ms` FIJOS
después de cada `step()+swap`. Con un frame de ~33 ms el ciclo era de ~66 ms (≈15 fps).
GL2JNIView limita el FRAME COMPLETO a ~33 ms. **Fix:** pacing por tiempo transcurrido (solo duerme
lo que falta hasta `1/setting_fpsCap`); el heartbeat del log muestra los fps reales.
Nota: el loader responde `isHighPhoneGroup=true` (fiel a GLGame.java con "PowerVR"), así que el
juego corre en la calidad máxima. Si sigue lento, la próxima palanca es responder "medio"
(high=false, low=false) en `java.c`.

**Joystick invertido:** el nativo lee el pad del Xperia Play con Y hacia arriba. **Fix:** ejes
configurables en `ux0:data/dungeonhunter3/config.txt` (`source/utils/settings.c`, se crea en el
primer arranque): `setting_invertLeftX/Y`, `setting_invertRightX/Y` (Y=1 por defecto),
`setting_fpsCap` (30). El panel trasero sigue al eje Y izquierdo.

**Guardado (no se conserva entre arranques):** log_022 muestra `dh3_profile.savegame` leído OK a
los 14.88 s y desaparecido a los 15.07 s; `dh3_000.savegame` tampoco existía al arrancar. FTP:
los archivos que escribe la sesión SÍ quedan en `files/` (header `UnHommeCaPleureAussi`, versión
1.5.2c, sanos). Descartado: bundle (`DungeonHunter3.save` nunca se abre), StatsManager (borra por
nombre fijo), `_checkOwnership` (solo compara versión). Candidatos: `Savegame::_cacheFile`
(backupSavefile + deleteSavefile si falla alguna validación) u otro borrado masivo.
- `source/reimpl/io.c`: `remove`/`unlink`/`rename` se loguean con el caller `so+0x...`.
  `rename` ahora reemplaza el destino si existe (semántica POSIX; sceIoRename falla).
- `source/utils/init.c`: al arrancar, copia las partidas a `ux0:data/dungeonhunter3/savebackup/`
  (red de seguridad mientras se encuentra la causa).

**Textura del héroe (maniquí beige, enemigos bien):** las texturas del warlord se suben bien
(dif/spec/opa ETC1 1024², log_022). Sospecha: las guardas de Fases 17/18 en
`createMaterialVertexAttributeMap` (mesh buffer NULL → `CVertexAttributeMap::set` saltado = sin
texcoords = color plano). Esas guardas ahora cuentan sus activaciones:
`[material] NULL-guard hits: ...` en el heartbeat del log.

Build verde.

## Fase 28: FPS y guardado confirmados; bajones y héroe (log_024)

**Confirmado en consola (log_024):** 30 fps estables (Fase 27) y la partida se carga. El log del
guardado muestra el ciclo normal del motor: `backupSavefile` intenta `remove(X.bak)` (-1, no
existe) en cada guardado y no renombra; ningún borrado de las partidas. No hubo más pérdida.

**Bajones de FPS (18-21 fps a los 119 s y 142 s):** coinciden con streaming de assets
(efectos/monstruos). En 11 s: 326 aperturas del OBB (virtuales, baratas) con su línea de log cada
una, y decenas de `stat`/`fopen` a `ux0:data/dungeonhunter3/data/...`, carpeta que no existe en la
Vita (todo está en el OBB). **Fix (`source/reimpl/io.c`):** si `DATA_PATH "data"` no existe
(se verifica una vez; una escritura debajo invalida el resultado), las lecturas debajo fallan con
ENOENT sin syscall ni log. Las aperturas/cierres virtuales del OBB ya no se loguean.

**Héroe "maniquí":** las guardas NULL de material.c nunca se activaron (sin
`[material] NULL-guard hits`): esa hipótesis queda descartada. Extraje del OBB (pak decodificado:
LCG `seed*1664525+1013904223`, rotación + XOR, clave 0x3857a) `astromancer_male.bdae` y un enemigo
(`desertman_cultist.bdae`): los dos usan `Character-fx-profile_GLES2` con dif/spec, así que los
datos están bien. Lo único exclusivo del jugador en el shader es la variante IR de
`CharNormalSpec` (iridiscencia: `irrColor = ird_boost * SH(ird_c*)` sumado a la luz del vértice).
Diag (`source/utils/glutil.c`):
- `[irdiag] CharNormalSpec VS/FS variant: ...` (cada combinación de #define, una vez);
- `[irdiag] prog N ird_xxx = (...)` (valores que el motor sube, máx. 80 líneas);
- experimento `setting_dbgNoIrr 1` en config.txt: quita el término IR del VS.
`config.txt` se reescribe en cada arranque para agregar las claves nuevas.

Build verde.

## Fase 29: héroe "maniquí" = heurística de atributos intercalados de vitaGL (log_025/026)

log_025/026 (descargados por FTP): los `ird_*` son normales (coeficientes SH ~±1, `ird_boost=1`,
`Glossiness=10`): queda descartada la iluminación IR saturada. En ninguna corrida se aplicó
`setting_dbgNoIrr`. La captura ampliada muestra el arma CON textura y el cuerpo con forma e
iluminación correctas pero sin ninguna variación de textura = UV constantes/basura.

**Causa (vitaGL, `custom_shaders.c` draw path, sin STRICT_DRAW_COMPLIANCE):** `is_packed` decide que
TODOS los atributos están intercalados en un mismo bloque mirando solo `attr_map[0]` y
`attr_map[1]` (mismo VBO o, en client arrays, offset de attr1 dentro del stride de attr0). El
cuerpo del héroe usa skinning por CPU: `Vertex`+`Normal` intercalados en el buffer de salida del
skinning, `texcoord0` en otro array. vitaGL lo marca packed y lee las UV con
`offset = ptr(texcoord0) - ptr(Vertex)` desde la copia temporal del primer bloque → fuera de su
array. Los modelos rígidos (arma, escenario) y los enemigos sin ese layout no se ven afectados.
`DRAW_SPEEDHACK=2` solo define `SAFER_DRAW_SPEEDHACK`, así que este camino sí se usaba.

**Fix (`CMakeLists.txt`):** `SAFE_DRAW=1` en los flags de vitaGL (→ `STRICT_DRAW_COMPLIANCE`,
decisión de packed por atributo). vitaGL recompilado. eboot subido por FTP con
`psvita-toolkit deploy --eboot`. Vigilar el costo en FPS (la ruta estricta copia más).

Build verde.

## Fase 30: textura del héroe confirmada; parche puntual en lugar de SAFE_DRAW (log_027)

log_027: el héroe ya muestra sus texturas reales (Fase 29 confirmada), pero con bajones más
frecuentes (27 fps en el heartbeat). `SAFE_DRAW=1` copia atributo por atributo en TODOS los draws.
**Fix:** se volvió al camino rápido sin SAFE_DRAW y se parcheó solo la heurística
(`lib/vitagl/source/custom_shaders.c`, `dh3_attribs_interleaved`, en los 3 sitios: drawArrays,
drawElements x2): packed solo si TODOS los atributos activos caen dentro del stride del primero.
eboot subido por FTP. Esperado: textura del héroe igual + fps como en log_024.

Build verde.
