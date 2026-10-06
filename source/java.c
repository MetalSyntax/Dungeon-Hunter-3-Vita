/*
 * java.c — "Java-side" handlers for Dungeon Hunter 3 (FalsoJNI).
 *
 * Subset real llamado por libDH3.so (confirmado por strings exactos en el
 * .so + firmas jadx, 2026-10-01). Los 523 candidatos de generated_jni_table.h
 * son el techo jadx, NO lo que el nativo pide — no registrar a ciegas.
 * FalsoJNI matchea por NOMBRE (la firma se ignora en el lookup), y los
 * boolean Java se registran como BOOLEAN (methodsBoolean): el precedente DH2
 * de registrarlos como INT NO aplica a este FalsoJNI (ver methodsBoolean).
 */

#include <falso_jni/FalsoJNI_Impl.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

extern so_module so_mod;
extern JNIEnv jni;

/* Literales que el Java real retorna tal cual (jadx). */
#define DH3_PACKAGE_LIT "com.gameloft.android.ANMP.GloftD3HM"
static const char DH3_PACKAGE[]  = DH3_PACKAGE_LIT;
static const char DH3_CLIENTID[] = "1674:53123:1.5.2:android:googleplay";
static const char DH3_IMEI[]     = "000000000000000";
static const char DH3_SOLIB[]    = "DH3";
static const char DH3_EMPTY[]    = "";
static const char DH3_PREFS[]    = "GLGamePrefs";   /* GameInstaller.mPreferencesName */
static const char DH3_MODEL[]    = "PS Vita";
static const char DH3_UA[]       = "Mozilla/5.0 (Linux; U; Android 4.4; PS Vita)";

/*
 * Rutas con la semantica del Java real: SIN '/' final (GL2JNILib.getSDFolder
 * devuelve "/sdcard/gameloft/games/GloftD3HM" y el Java concatena "/x").
 *   root  = SDFolder        -> glf::Fs::SetDataDir  (assets + OBB)
 *   home  = getFilesDir()   -> glf::Fs::SetHomeDir  (saves)
 *   cache = getCacheDir()   -> glf::Fs::SetTempDir
 * glf::Fs::ResolvePath (0x8458e8) trata un path como absoluto solo si
 * path[1]==':' o si empieza por el DataDir; todo lo demas lo une al DataDir
 * con VJoinPath (normaliza '/'). Por eso el OBB va absoluto bajo root.
 */
static char dh3_root[256], dh3_home[256], dh3_cache[256], dh3_obb[320];

/* Nombres reales del OBB: getObbFileName() usa main.1501 (el que se
 * distribuye, el que hay en el repo), getObbFileName2/3/4 usan main.15221.
 * Application::InitAndroid prueba 1 -> 2 -> 3(i) via
 * FileSystemBase::addCustomPakArchive (pak propio, NO zip), y se salta el
 * OBB entero si existe "data/file000000.dat" suelto bajo root. */
static const char *DH3_OBB_NAMES[] = {
    "main.1501.com.gameloft.android.ANMP.GloftD3HM.obb",
    "main.15221.com.gameloft.android.ANMP.GloftD3HM.obb",
};

static void dh3_paths_init(void) {
    if (dh3_root[0]) return;
    snprintf(dh3_root, sizeof(dh3_root), "%s", DATA_PATH);
    size_t n = strlen(dh3_root);
    while (n > 0 && dh3_root[n - 1] == '/') dh3_root[--n] = '\0';
    snprintf(dh3_home, sizeof(dh3_home), "%s/files", dh3_root);
    snprintf(dh3_cache, sizeof(dh3_cache), "%s/cache", dh3_root);
    snprintf(dh3_obb, sizeof(dh3_obb), "%s/%s", dh3_root, DH3_OBB_NAMES[0]);
    /* Subcarpetas donde puede quedar al copiarlo: tal cual sale del zip de
     * cache (<pkg>/), o los layouts Android que arma el propio Java
     * (obb/<pkg>/, Android/obb/<pkg>/). */
    static const char *dirs[] = {
        "", "/" DH3_PACKAGE_LIT, "/obb/" DH3_PACKAGE_LIT, "/Android/obb/" DH3_PACKAGE_LIT,
    };
    int found = 0;
    for (unsigned d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && !found; d++) {
        for (unsigned i = 0; i < sizeof(DH3_OBB_NAMES) / sizeof(DH3_OBB_NAMES[0]); i++) {
            char p[320];
            snprintf(p, sizeof(p), "%s%s/%s", dh3_root, dirs[d], DH3_OBB_NAMES[i]);
            if (access(p, F_OK) == 0) {
                snprintf(dh3_obb, sizeof(dh3_obb), "%s", p);
                found = 1;
                break;
            }
        }
    }
    mkdir(dh3_home, 0777);
    mkdir(dh3_cache, 0777);
    l_info("[Java] paths: root=%s home=%s cache=%s obb=%s%s", dh3_root,
           dh3_home, dh3_cache, dh3_obb,
           access(dh3_obb, F_OK) == 0 ? "" : " (MISSING)");
}

/* Equivalente a GL2JNILib.setupPaths(): setPaths(sd, files, cache). */
void dh3_setup_paths(void) {
    static void (* setPaths)(JNIEnv *, void *, jstring, jstring, jstring) = NULL;
    dh3_paths_init();
    if (!setPaths) {
        setPaths = (void *) so_symbol(&so_mod,
            "Java_com_gameloft_android_ANMP_GloftD3HM_GL2JNILib_setPaths");
    }
    if (setPaths) {
        setPaths(&jni, (void *) "com/gameloft/android/ANMP/GloftD3HM/GL2JNILib",
                 (jstring) dh3_root, (jstring) dh3_home, (jstring) dh3_cache);
        l_success("[Java] setPaths(root, home, cache) done.");
    } else {
        l_warn("[Java] setPaths symbol missing.");
    }
}

/*
 * Resuelve un recurso pedido por getResource(): el Java real lo busca primero
 * como recurso de classpath (classes.dex) y SUtils como asset APK. En Vita
 * todo vive bajo DATA_PATH: se prueba tal cual y bajo assets/.
 * El Java real quita UN '.' inicial si existe (GL2JNILib.java:372).
 */
static int dh3_resolve_resource(const char *name, char *out, size_t out_size) {
    if (!name || !*name) return 0;
    if (name[0] == '.') name++;
    snprintf(out, out_size, DATA_PATH "%s", name);
    if (access(out, F_OK) == 0) return 1;
    snprintf(out, out_size, DATA_PATH "assets/%s", name);
    return access(out, F_OK) == 0;
}

static jobject dh3_read_file(const char *what, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    JavaDynArray *jda = jda_alloc((jsize) st.st_size, FIELD_TYPE_BYTE);
    if (!jda) {
        fclose(f);
        return NULL;
    }
    fread(jda->array, 1, (size_t) st.st_size, f);
    fclose(f);
    l_info("[Java] %s: %ld bytes (%s)", what, (long) st.st_size, path);
    return (jobject) jda;
}

/* ---- GL2JNILib (todo static) ---- */

jobject GL2JNILib_getSDFolder(jmethodID id, va_list args) {
    (void) id; (void) args;
    /* Real: pref SDFolder o /sdcard/gameloft/games/GloftD3HM (sin '/' final). */
    dh3_paths_init();
    return (jobject) dh3_root;
}

jobject GL2JNILib_getResource(jmethodID id, va_list args) {
    const char *name = (const char *) va_arg(args, jstring);
    char path[512];
    (void) id;
    if (!dh3_resolve_resource(name, path, sizeof(path))) {
        l_warn("[Java] GL2JNILib.getResource(%s): not found", name ? name : "(null)");
        return NULL;
    }
    return dh3_read_file("GL2JNILib.getResource", path);
}

void GL2JNILib_setupPaths(jmethodID id, va_list args) {
    /* Real: sDFolder=getSDFolder(), files, cache, mkdirs, setPaths(sd,home,temp).
     * glf::AndroidSetPaths -> Fs::SetDataDir/SetHomeDir/SetTempDir (0x8497c0). */
    (void) id; (void) args;
    dh3_setup_paths();
}

jint GL2JNILib_getTexturePack(jmethodID id, va_list args) {
    /* Real: 1 solo en HTC Desire/M9300/LG-P970/DROIDX... Vita no matchea -> 0. */
    (void) id; (void) args;
    return 0;
}

jint GL2JNILib_getColorPack(jmethodID id, va_list args) {
    /* Real: mapea Build.MODEL (i9000=1, i9100=2, ...). Vita no matchea -> 0. */
    (void) id; (void) args;
    return 0;
}

void GL2JNILib_notifyTrophy(jmethodID id, va_list args) {
    /* Fiel al real (GL2JNILib.java:465): array de 101 bytes en
     * getSDFolder()/androidTrophy.dat, arr[i]=1. */
    jint trophy = va_arg(args, jint);
    char path[512];
    (void) id;
    snprintf(path, sizeof(path), DATA_PATH "androidTrophy.dat");
    unsigned char arr[101];
    memset(arr, 0x7F, sizeof(arr));
    FILE *f = fopen(path, "r+b");
    if (f) {
        fread(arr, 1, sizeof(arr), f);
    } else {
        f = fopen(path, "w+b");
    }
    if (!f) {
        l_warn("[Java] notifyTrophy(%d): cannot open trophy file", (int) trophy);
        return;
    }
    if (trophy >= 0 && trophy < 101) arr[trophy] = 1;
    fseek(f, 0, SEEK_SET);
    fwrite(arr, 1, sizeof(arr), f);
    fclose(f);
    l_info("[Java] notifyTrophy(%d) recorded.", (int) trophy);
}

void GL2JNILib_openIGP(jmethodID id, va_list args) {
    /* Real: lanza IGPFreemiumActivity (tienda). Sin IGP en Vita: no-op. */
    (void) id; (void) args;
    l_info("[Java] GL2JNILib.openIGP: ignored (no IGP on Vita).");
}

jobject GL2JNILib_getObbFileName(jmethodID id, va_list args) {
    /* Real: "../../../Android/obb/<pkg>/main.1501...obb" relativo al SDFolder.
     * En Vita el OBB vive en root: absoluto (glf lo usa tal cual porque
     * empieza por el DataDir). Las 4 variantes devuelven el que exista. */
    (void) id; (void) args;
    dh3_paths_init();
    return (jobject) dh3_obb;
}

jobject GL2JNILib_getObbFileName2(jmethodID id, va_list args) {
    (void) id; (void) args;
    dh3_paths_init();
    return (jobject) dh3_obb;
}

jobject GL2JNILib_getObbFileName3(jmethodID id, va_list args) {
    (void) id; (void) args;
    dh3_paths_init();
    return (jobject) dh3_obb;
}

jobject GL2JNILib_getObbFileName4(jmethodID id, va_list args) {
    (void) id; (void) args;
    dh3_paths_init();
    return (jobject) dh3_obb;
}

jboolean GL2JNILib_setCurrentContext(jmethodID id, va_list args) {
    /* Real: GL2JNIView.a(i) = eglMakeCurrent(ctx[i]) (i<0 = soltar).
     * Vita tiene UN contexto (vitaGL) y main.c anuncia 0 extra
     * (setNumExtraContext(0)), asi que solo se piden 0 (SetAsMainThread)
     * y -1 (ReleaseContext): ambos true. Un i>=1 no deberia llegar; si
     * llega, false = "no se pudo", como un eglMakeCurrent fallido. */
    jint ctx = va_arg(args, jint);
    (void) id;
    if (ctx >= 1) {
        l_warn("[Java] setCurrentContext(%d): no extra GL contexts on Vita -> false", ctx);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/* ---- GLUtils/SUtils (todo static) ---- */

jobject SUtils_getSaveFolder(jmethodID id, va_list args) {
    /* Real: getFilesDir() (sin '/' final). */
    (void) id; (void) args;
    dh3_paths_init();
    return (jobject) dh3_home;
}

jobject SUtils_getPackage(jmethodID id, va_list args) {
    (void) id; (void) args;
    return (jobject) DH3_PACKAGE;
}

jobject SUtils_getContext(jmethodID id, va_list args) {
    /* Sin Activity real no hay Context. NULL + warn: si el motor lo
     * desreferencia, el primer log lo mostrará (RIESGO anotado). */
    (void) id; (void) args;
    l_warn("[Java] SUtils.getContext: NULL (no Android Context on Vita).");
    return NULL;
}

jobject SUtils_nativeGetPreference(jmethodID id, va_list args) {
    /* Real: lee un Bundle y devuelve otro con npResult. Sin soporte Bundle
     * en FalsoJNI: NULL + warn (RIESGO anotado, lo dirá el log). */
    (void) id; (void) args;
    l_warn("[Java] SUtils.nativeGetPreference: Bundle unsupported, NULL.");
    return NULL;
}

void SUtils_nativeSetPreference(jmethodID id, va_list args) {
    (void) id; (void) args;
    l_info("[Java] SUtils.nativeSetPreference: ignored.");
}

jobject SUtils_getPreferenceString(jmethodID id, va_list args) {
    /* El nativo solo resuelve el overload (String key, String prefsName)
     * (AndroidOS::Init, 0x311b6c) y lo usa en AndroidOS::GetSDFolder
     * (0x311dec) con key="SDFolder": si devuelve "" cae al fallback
     * hardcodeado "/sdcard/gameloft/games/GloftD3HM" -> hay que contestar
     * el root real. El resto de claves: "" (= pref inexistente). */
    const char *key = (const char *) va_arg(args, jstring);
    (void) id;
    if (key && strcmp(key, "SDFolder") == 0) {
        dh3_paths_init();
        return (jobject) dh3_root;
    }
    l_info("[Java] SUtils.getPreferenceString(%s): \"\"", key ? key : "(null)");
    return (jobject) DH3_EMPTY;
}

jboolean SUtils_getPreferenceBoolean(jmethodID id, va_list args) {
    (void) id; (void) args;
    return 0;
}

jint SUtils_getPreferenceInt(jmethodID id, va_list args) {
    (void) id; (void) args;
    return 0;
}

jlong SUtils_getPreferenceLong(jmethodID id, va_list args) {
    (void) id; (void) args;
    return 0;
}

void SUtils_setPreference(jmethodID id, va_list args) {
    (void) id; (void) args;
}

void SUtils_removePreference(jmethodID id, va_list args) {
    (void) id; (void) args;
}

/* ---- Video (SUtils): por diseño sin videos en DH3 ----
 * playVideo() real retorna false SIEMPRE (stub en el propio APK) y
 * startVideoPlayerIfNeeded() igual. No hay gate tipo videoDone de DH2:
 * el motor ya contempla el camino "sin video". */

jint SUtils_playVideo(jmethodID id, va_list args) {
    (void) id; (void) args;
    l_info("[Java] SUtils.playVideo: skipped (false, same as stock APK).");
    return 0;
}

void SUtils_stopVideo(jmethodID id, va_list args) {
    (void) id; (void) args;
}

void SUtils_inGameVideoOnPause(jmethodID id, va_list args) {
    (void) id; (void) args;
}

void SUtils_inGameVideoOnResume(jmethodID id, va_list args) {
    (void) id; (void) args;
}

void SUtils_inGameVideoSetSkipEnabled(jmethodID id, va_list args) {
    (void) id; (void) args;
}

/* ---- GLGame (todo static) ---- */

jobject GLGame_getIMEI(jmethodID id, va_list args) {
    (void) id; (void) args;
    return (jobject) DH3_IMEI;
}

jobject GLGame_getClientID(jmethodID id, va_list args) {
    (void) id; (void) args;
    return (jobject) DH3_CLIENTID;
}

void GLGame_Exit(jmethodID id, va_list args) {
    /* Real: killProcess + System.exit(0). */
    (void) id; (void) args;
    l_info("[Java] GLGame.Exit.");
    sceKernelExitProcess(0);
}

jboolean GLGame_isWifiEnabled(jmethodID id, va_list args) {
    /* Sin ConnectivityManager: false (igual que DH2 isWifiAlive=0). */
    (void) id; (void) args;
    return 0;
}

void GLGame_splashScreenFunc(jmethodID id, va_list args) {
    /* Real: guarda estado D=4/C=str. Sin UI Java: no-op. */
    (void) id; (void) args;
    l_info("[Java] GLGame.splashScreenFunc: ignored.");
}

jobject GLGame_getSoLibName(jmethodID id, va_list args) {
    (void) id; (void) args;
    return (jobject) DH3_SOLIB;
}

/* ---- Teclado en pantalla (GLGame.OpenInput) ----
 * OnScreenKeyboard::GetString -> Application::ShowKeyboard ->
 * AndroidShowKeyboard -> OpenInput(maxLen, type, keepText) (GLGame.java:246).
 * El Java muestra un EditText y al confirmar llama nativeOnEditorEnd(text)
 * (solo si no esta vacio) + nativeHideKeyboard() (az.java / ar.java).
 * Aca solo se encola; main.c abre el sceImeDialog y contesta igual. */
volatile int dh3_ime_pending = 0;
int dh3_ime_maxlen = 0, dh3_ime_type = 0;

void GLGame_OpenInput(jmethodID id, va_list args) {
    jint maxlen = va_arg(args, jint), type = va_arg(args, jint), keep = va_arg(args, jint);
    (void) id;
    l_info("[Java] GLGame.OpenInput(maxLen=%d, type=%d, keep=%d)", (int) maxlen, (int) type, (int) keep);
    if (maxlen <= 0) return;   /* el Java real tambien ignora i<=0 */
    dh3_ime_maxlen = maxlen;
    dh3_ime_type = type;
    dh3_ime_pending = 1;
}

/* ---- Clase de dispositivo (Device::SetupDeviceSpecific / MenuManager) ----
 * Fiel a la heuristica del propio APK (GLGame.java:442-480) evaluada con el
 * GPU de Vita (GL_RENDERER "PowerVR SGX 543MP4+"): contiene "powervr" y no
 * 530/531/535/540 -> isHighPhoneGroup=true, isLowPhoneGroup=false.
 * checkFirmware = SDK_INT>=11 -> true. Orientacion: Vita fija -> false. */

/* Real (GLGame.java:442): GL_RENDERER "powervr" sin 530/531/535/540 -> high.
 * El SGX543MP4+ de la Vita cae ahi: high=true, low=false. */
jboolean GLGame_isHighPhoneGroup(jmethodID id, va_list args) { (void) id; (void) args; return JNI_TRUE; }
jboolean GLGame_isLowPhoneGroup(jmethodID id, va_list args)  { (void) id; (void) args; return JNI_FALSE; }
/* Real: SDK_INT >= 11. */
jboolean GLGame_checkFirmware(jmethodID id, va_list args)    { (void) id; (void) args; return JNI_TRUE; }
jboolean GL2JNIActivity_isAndroidOrientationAvailable(jmethodID id, va_list args) {
    (void) id; (void) args;
    return 0;
}

jobject Device_getUserAgent(jmethodID id, va_list args) {
    (void) id; (void) args;
    return (jobject) DH3_UA;
}

/*
 * Strings que el nativo pide y luego pasa directo a GetStringUTFChars +
 * strcpy/std::string (p.ej. GetDeviceMAC, glot::GetDeviceIdentifier, IGP,
 * push, GLLive, Facebook). Un metodo no registrado devuelve NULL en FalsoJNI
 * -> strcpy(NULL) en el nativo. En Android real nunca son NULL: "".
 */
jobject Generic_emptyString(jmethodID id, va_list args) {
    (void) args;
    l_info("[Java] String method id %d -> \"\"", (int) id);
    return (jobject) DH3_EMPTY;
}

/* ---- java.util.Locale (GetDeviceLanguage, 0x30bf8c) ----
 * El nativo compara getISO3Language() con eng/kor/fra/spa/deu/ita/jpn/zho
 * (+ getCountry()=="CN" para chino simplificado). Se deriva del idioma de
 * sistema de la Vita. */
#include <psp2/apputil.h>
#include <psp2/system_param.h>

static int dh3_vita_lang(void) {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) < 0)
        lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    return lang;
}

jobject Locale_getDefault(jmethodID id, va_list args) {
    (void) id; (void) args;
    return (jobject) "java/util/Locale";
}

jobject Locale_getISO3Language(jmethodID id, va_list args) {
    (void) id; (void) args;
    switch (dh3_vita_lang()) {
        case SCE_SYSTEM_PARAM_LANG_FRENCH:     return (jobject) "fra";
        case SCE_SYSTEM_PARAM_LANG_SPANISH:    return (jobject) "spa";
        case SCE_SYSTEM_PARAM_LANG_GERMAN:     return (jobject) "deu";
        case SCE_SYSTEM_PARAM_LANG_ITALIAN:    return (jobject) "ita";
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:   return (jobject) "jpn";
        case SCE_SYSTEM_PARAM_LANG_KOREAN:     return (jobject) "kor";
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S:
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T:  return (jobject) "zho";
        case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
        case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return (jobject) "por";
        case SCE_SYSTEM_PARAM_LANG_RUSSIAN:    return (jobject) "rus";
        default:                               return (jobject) "eng";
    }
}

jobject Locale_getCountry(jmethodID id, va_list args) {
    (void) id; (void) args;
    switch (dh3_vita_lang()) {
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S: return (jobject) "CN";
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T: return (jobject) "TW";
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:  return (jobject) "JP";
        case SCE_SYSTEM_PARAM_LANG_KOREAN:    return (jobject) "KR";
        default:                              return (jobject) "US";
    }
}

/* ---- android.media.AudioTrack sobre sceAudioOut ----
 * vox::DriverAndroid elige backend por s_androidAPILevel (>8 -> OpenSL,
 * <=8 -> AudioTrack, DriverAndroid::Init 0x718c7c). main.c fuerza 8 con
 * VoxSetAndroidAPILevel: el motor mezcla todo y solo nos entrega PCM.
 * Protocolo real (DriverAndroid::_InitAT 0x718ce8 + UpdateThreadedAT
 * 0x71a0b0, hilo propio):
 *   getNativeOutputSampleRate(STREAM_MUSIC=3) -> rate
 *   getMinBufferSize(rate, CHANNEL_OUT_STEREO=12, PCM_16BIT=2) -> bytes
 *   new AudioTrack(3, rate, 12, 2, bytes, MODE_STREAM=1); play();
 *   NewByteArray(bytes); loop { fill; write(arr, 0, len) }; stop; release.
 * Los handlers FalsoJNI no reciben `this`: hay un unico AudioTrack (vox). */
#include <psp2/audioout.h>

#define DH3_AT_RATE   44100
#define DH3_AT_GRAIN  1024   /* frames por sceAudioOutOutput (mult. de 64) */

static int dh3_at_port = -1;
static int dh3_at_playing = 0;
static int16_t dh3_at_buf[2][DH3_AT_GRAIN * 2];
static int dh3_at_cur = 0, dh3_at_fill = 0;  /* fill en frames */

jint AudioTrack_getNativeOutputSampleRate(jmethodID id, va_list args) {
    (void) id; (void) args;
    return DH3_AT_RATE;
}

jint AudioTrack_getMinBufferSize(jmethodID id, va_list args) {
    jint rate = va_arg(args, jint), ch = va_arg(args, jint), fmt = va_arg(args, jint);
    (void) id;
    l_info("[Java] AudioTrack.getMinBufferSize(%d, %d, %d) -> %d",
           (int) rate, (int) ch, (int) fmt, DH3_AT_GRAIN * 4);
    return DH3_AT_GRAIN * 4;  /* stereo s16 */
}

jobject AudioTrack_init(jmethodID id, va_list args) {
    jint stream = va_arg(args, jint), rate = va_arg(args, jint);
    jint ch = va_arg(args, jint), fmt = va_arg(args, jint);
    jint bytes = va_arg(args, jint), mode = va_arg(args, jint);
    (void) id;
    l_info("[Java] new AudioTrack(stream=%d rate=%d ch=%d fmt=%d bytes=%d mode=%d)",
           (int) stream, (int) rate, (int) ch, (int) fmt, (int) bytes, (int) mode);
    if (dh3_at_port < 0) {
        dh3_at_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                          DH3_AT_GRAIN, rate,
                                          SCE_AUDIO_OUT_MODE_STEREO);
        if (dh3_at_port < 0) {
            l_error("[Java] sceAudioOutOpenPort(%d Hz) failed: 0x%08X",
                    (int) rate, dh3_at_port);
        } else {
            int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
            sceAudioOutSetVolume(dh3_at_port,
                SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
            l_success("[Java] AudioTrack -> sceAudioOut port %d.", dh3_at_port);
        }
    }
    dh3_at_fill = 0;
    /* Objeto opaco: el nativo le hace NewGlobalRef y al final DeleteGlobalRef
     * (= free() en FalsoJNI), asi que tiene que ser memoria de heap. */
    return (jobject) malloc(16);
}

void AudioTrack_play(jmethodID id, va_list args)    { (void) id; (void) args; dh3_at_playing = 1; }
void AudioTrack_pause(jmethodID id, va_list args)   { (void) id; (void) args; dh3_at_playing = 0; }
void AudioTrack_stop(jmethodID id, va_list args)    { (void) id; (void) args; dh3_at_playing = 0; dh3_at_fill = 0; }

void AudioTrack_release(jmethodID id, va_list args) {
    (void) id; (void) args;
    dh3_at_playing = 0;
    if (dh3_at_port >= 0) {
        sceAudioOutReleasePort(dh3_at_port);
        dh3_at_port = -1;
    }
}

jint AudioTrack_getPlayState(jmethodID id, va_list args) {
    (void) id; (void) args;
    return dh3_at_playing ? 3 /* PLAYSTATE_PLAYING */ : 1 /* STOPPED */;
}

jint AudioTrack_write(jmethodID id, va_list args) {
    /* write(byte[] audioData, int offsetInBytes, int sizeInBytes): bloquea
     * como MODE_STREAM real (sceAudioOutOutput espera al grain previo), que
     * es el pacing que asume el hilo de vox. */
    jbyteArray arr = va_arg(args, jbyteArray);
    jint off = va_arg(args, jint), len = va_arg(args, jint);
    (void) id;
    JavaDynArray *jda = jda_find((void *) arr);
    if (!jda || dh3_at_port < 0 || len <= 0) return len;
    const uint8_t *src = (const uint8_t *) jda->array + off;
    int frames = len / 4;
    while (frames > 0) {
        int n = DH3_AT_GRAIN - dh3_at_fill;
        if (n > frames) n = frames;
        memcpy(&dh3_at_buf[dh3_at_cur][dh3_at_fill * 2], src, (size_t) n * 4);
        dh3_at_fill += n;
        src += n * 4;
        frames -= n;
        if (dh3_at_fill == DH3_AT_GRAIN) {
            sceAudioOutOutput(dh3_at_port, dh3_at_buf[dh3_at_cur]);
            dh3_at_cur ^= 1;
            dh3_at_fill = 0;
        }
    }
    return len;
}

/*
 * JNI Methods
 */

enum {
    M_getSDFolder = 1,
    M_getResource,
    M_setupPaths,
    M_getTexturePack,
    M_getColorPack,
    M_notifyTrophy,
    M_openIGP,
    M_getObbFileName,
    M_getObbFileName2,
    M_getObbFileName3,
    M_getObbFileName4,
    M_setCurrentContext,
    M_getSaveFolder,
    M_getPackage,
    M_getContext,
    M_nativeGetPreference,
    M_nativeSetPreference,
    M_getPreferenceString,
    M_getPreferenceBoolean,
    M_getPreferenceInt,
    M_getPreferenceLong,
    M_setPreference,
    M_removePreference,
    M_playVideo,
    M_stopVideo,
    M_inGameVideoOnPause,
    M_inGameVideoOnResume,
    M_inGameVideoSetSkipEnabled,
    M_getIMEI,
    M_getClientID,
    M_Exit,
    M_isWifiEnabled,
    M_splashScreenFunc,
    M_getSoLibName,
    /* clase de dispositivo / orientacion */
    M_isHighPhoneGroup,
    M_OpenInput,
    M_isLowPhoneGroup,
    M_checkFirmware,
    M_isAndroidOrientationAvailable,
    M_getUserAgent,
    /* java.util.Locale */
    M_Locale_getDefault,
    M_Locale_getISO3Language,
    M_Locale_getCountry,
    /* android.media.AudioTrack (vox backend AT) */
    M_AudioTrack_init,
    M_AudioTrack_getMinBufferSize,
    M_AudioTrack_getNativeOutputSampleRate,
    M_AudioTrack_play,
    M_AudioTrack_pause,
    M_AudioTrack_stop,
    M_AudioTrack_release,
    M_AudioTrack_write,
    M_AudioTrack_getPlayState,
    /* Strings sin efecto en Vita: "" (nunca NULL, ver Generic_emptyString) */
    M_getMAC,
    M_getPandoraURL,
    M_getOpref,
    M_getGLLiveUsername,
    M_getGLLivePassword,
    M_GetIGPInstalledGames,
    M_getSharedValue,
    M_GetJanusToken,
    M_GetRegistrationID,
    M_GetTokenID,
    M_GetUserID,
    M_SendPush,
    M_SendPushToMyself,
    M_getIdentifier,
    M_AccessToken,
    M_getUserID,
};

NameToMethodID nameToMethodId[] = {
        { M_getSDFolder,             "getSDFolder",             METHOD_TYPE_OBJECT },
        { M_getResource,             "getResource",             METHOD_TYPE_OBJECT },
        { M_setupPaths,              "setupPaths",              METHOD_TYPE_VOID },
        { M_getTexturePack,          "getTexturePack",          METHOD_TYPE_INT },
        { M_getColorPack,            "getColorPack",            METHOD_TYPE_INT },
        { M_notifyTrophy,            "notifyTrophy",            METHOD_TYPE_VOID },
        { M_openIGP,                 "openIGP",                 METHOD_TYPE_VOID },
        { M_getObbFileName,          "getObbFileName",          METHOD_TYPE_OBJECT },
        { M_getObbFileName2,         "getObbFileName2",         METHOD_TYPE_OBJECT },
        { M_getObbFileName3,         "getObbFileName3",         METHOD_TYPE_OBJECT },
        { M_getObbFileName4,         "getObbFileName4",         METHOD_TYPE_OBJECT },
        { M_setCurrentContext,       "setCurrentContext",       METHOD_TYPE_BOOLEAN },
        { M_getSaveFolder,           "getSaveFolder",           METHOD_TYPE_OBJECT },
        { M_getPackage,              "getPackage",              METHOD_TYPE_OBJECT },
        { M_getContext,              "getContext",              METHOD_TYPE_OBJECT },
        { M_nativeGetPreference,     "nativeGetPreference",     METHOD_TYPE_OBJECT },
        { M_nativeSetPreference,     "nativeSetPreference",     METHOD_TYPE_VOID },
        { M_getPreferenceString,     "getPreferenceString",     METHOD_TYPE_OBJECT },
        { M_getPreferenceBoolean,    "getPreferenceBoolean",    METHOD_TYPE_BOOLEAN },
        { M_getPreferenceInt,        "getPreferenceInt",        METHOD_TYPE_INT },
        { M_getPreferenceLong,       "getPreferenceLong",       METHOD_TYPE_LONG },
        { M_setPreference,           "setPreference",           METHOD_TYPE_VOID },
        { M_removePreference,        "removePreference",        METHOD_TYPE_VOID },
        { M_playVideo,               "playVideo",               METHOD_TYPE_INT },
        { M_stopVideo,               "stopVideo",               METHOD_TYPE_VOID },
        { M_inGameVideoOnPause,      "inGameVideoOnPause",      METHOD_TYPE_VOID },
        { M_inGameVideoOnResume,     "inGameVideoOnResume",     METHOD_TYPE_VOID },
        { M_inGameVideoSetSkipEnabled, "inGameVideoSetSkipEnabled", METHOD_TYPE_VOID },
        { M_getIMEI,                 "getIMEI",                 METHOD_TYPE_OBJECT },
        { M_getClientID,             "getClientID",             METHOD_TYPE_OBJECT },
        { M_Exit,                    "Exit",                    METHOD_TYPE_VOID },
        { M_isWifiEnabled,           "isWifiEnabled",           METHOD_TYPE_BOOLEAN },
        { M_splashScreenFunc,        "splashScreenFunc",        METHOD_TYPE_VOID },
        { M_getSoLibName,            "getSoLibName",            METHOD_TYPE_OBJECT },
        { M_isHighPhoneGroup,        "isHighPhoneGroup",        METHOD_TYPE_BOOLEAN },
        { M_OpenInput,               "OpenInput",               METHOD_TYPE_VOID },
        { M_isLowPhoneGroup,         "isLowPhoneGroup",         METHOD_TYPE_BOOLEAN },
        { M_checkFirmware,           "checkFirmware",           METHOD_TYPE_BOOLEAN },
        { M_isAndroidOrientationAvailable, "isAndroidOrientationAvailable", METHOD_TYPE_BOOLEAN },
        { M_getUserAgent,            "getUserAgent",            METHOD_TYPE_OBJECT },
        { M_Locale_getDefault,       "getDefault",              METHOD_TYPE_OBJECT },
        { M_Locale_getISO3Language,  "getISO3Language",         METHOD_TYPE_OBJECT },
        { M_Locale_getCountry,       "getCountry",              METHOD_TYPE_OBJECT },
        /* <init> se busca como "<clase FindClass>/<init>" (FalsoJNI.c:285) */
        { M_AudioTrack_init,         "android/media/AudioTrack/<init>", METHOD_TYPE_OBJECT },
        { M_AudioTrack_getMinBufferSize, "getMinBufferSize",    METHOD_TYPE_INT },
        { M_AudioTrack_getNativeOutputSampleRate, "getNativeOutputSampleRate", METHOD_TYPE_INT },
        { M_AudioTrack_play,         "play",                    METHOD_TYPE_VOID },
        { M_AudioTrack_pause,        "pause",                   METHOD_TYPE_VOID },
        { M_AudioTrack_stop,         "stop",                    METHOD_TYPE_VOID },
        { M_AudioTrack_release,      "release",                 METHOD_TYPE_VOID },
        { M_AudioTrack_write,        "write",                   METHOD_TYPE_INT },
        { M_AudioTrack_getPlayState, "getPlayState",            METHOD_TYPE_INT },
        { M_getMAC,                  "getMAC",                  METHOD_TYPE_OBJECT },
        { M_getPandoraURL,           "getPandoraURL",           METHOD_TYPE_OBJECT },
        { M_getOpref,                "getOpref",                METHOD_TYPE_OBJECT },
        { M_getGLLiveUsername,       "getGLLiveUsername",       METHOD_TYPE_OBJECT },
        { M_getGLLivePassword,       "getGLLivePassword",       METHOD_TYPE_OBJECT },
        { M_GetIGPInstalledGames,    "GetIGPInstalledGames",    METHOD_TYPE_OBJECT },
        { M_getSharedValue,          "getSharedValue",          METHOD_TYPE_OBJECT },
        { M_GetJanusToken,           "GetJanusToken",           METHOD_TYPE_OBJECT },
        { M_GetRegistrationID,       "GetRegistrationID",       METHOD_TYPE_OBJECT },
        { M_GetTokenID,              "GetTokenID",              METHOD_TYPE_OBJECT },
        { M_GetUserID,               "GetUserID",               METHOD_TYPE_OBJECT },
        { M_SendPush,                "SendPush",                METHOD_TYPE_OBJECT },
        { M_SendPushToMyself,        "SendPushToMyself",        METHOD_TYPE_OBJECT },
        { M_getIdentifier,           "getIdentifier",           METHOD_TYPE_OBJECT },
        { M_AccessToken,             "AccessToken",             METHOD_TYPE_OBJECT },
        { M_getUserID,               "getUserID",               METHOD_TYPE_OBJECT },
};

/* OJO: Java `boolean` VA ACA, no en methodsInt. CallStaticBooleanMethod*
 * de FalsoJNI despacha solo a methodBooleanCall -> methodsBoolean; un Z
 * registrado como INT nunca se ejecuta y el nativo recibe false
 * (log_001 2026-10-02: "methodBooleanCall method ID 12/35/37 not found"). */
MethodsBoolean methodsBoolean[] = {
        { M_setCurrentContext,       GL2JNILib_setCurrentContext },
        { M_getPreferenceBoolean,    SUtils_getPreferenceBoolean },
        { M_isWifiEnabled,           GLGame_isWifiEnabled },
        { M_isHighPhoneGroup,        GLGame_isHighPhoneGroup },
        { M_isLowPhoneGroup,         GLGame_isLowPhoneGroup },
        { M_checkFirmware,           GLGame_checkFirmware },
        { M_isAndroidOrientationAvailable, GL2JNIActivity_isAndroidOrientationAvailable },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

MethodsInt methodsInt[] = {
        { M_getTexturePack,          GL2JNILib_getTexturePack },
        { M_getColorPack,            GL2JNILib_getColorPack },
        { M_getPreferenceInt,        SUtils_getPreferenceInt },
        { M_playVideo,               SUtils_playVideo },
        { M_AudioTrack_getMinBufferSize, AudioTrack_getMinBufferSize },
        { M_AudioTrack_getNativeOutputSampleRate, AudioTrack_getNativeOutputSampleRate },
        { M_AudioTrack_write,        AudioTrack_write },
        { M_AudioTrack_getPlayState, AudioTrack_getPlayState },
};

MethodsLong methodsLong[] = {
        { M_getPreferenceLong,       SUtils_getPreferenceLong },
};

MethodsObject methodsObject[] = {
        { M_getSDFolder,             GL2JNILib_getSDFolder },
        { M_getResource,             GL2JNILib_getResource },
        { M_getObbFileName,          GL2JNILib_getObbFileName },
        { M_getObbFileName2,         GL2JNILib_getObbFileName2 },
        { M_getObbFileName3,         GL2JNILib_getObbFileName3 },
        { M_getObbFileName4,         GL2JNILib_getObbFileName4 },
        { M_getSaveFolder,           SUtils_getSaveFolder },
        { M_getPackage,              SUtils_getPackage },
        { M_getContext,              SUtils_getContext },
        { M_nativeGetPreference,     SUtils_nativeGetPreference },
        { M_getPreferenceString,     SUtils_getPreferenceString },
        { M_getIMEI,                 GLGame_getIMEI },
        { M_getClientID,             GLGame_getClientID },
        { M_getSoLibName,            GLGame_getSoLibName },
        { M_getUserAgent,            Device_getUserAgent },
        { M_Locale_getDefault,       Locale_getDefault },
        { M_Locale_getISO3Language,  Locale_getISO3Language },
        { M_Locale_getCountry,       Locale_getCountry },
        { M_AudioTrack_init,         AudioTrack_init },
        { M_getMAC,                  Generic_emptyString },
        { M_getPandoraURL,           Generic_emptyString },
        { M_getOpref,                Generic_emptyString },
        { M_getGLLiveUsername,       Generic_emptyString },
        { M_getGLLivePassword,       Generic_emptyString },
        { M_GetIGPInstalledGames,    Generic_emptyString },
        { M_getSharedValue,          Generic_emptyString },
        { M_GetJanusToken,           Generic_emptyString },
        { M_GetRegistrationID,       Generic_emptyString },
        { M_GetTokenID,              Generic_emptyString },
        { M_GetUserID,               Generic_emptyString },
        { M_SendPush,                Generic_emptyString },
        { M_SendPushToMyself,        Generic_emptyString },
        { M_getIdentifier,           Generic_emptyString },
        { M_AccessToken,             Generic_emptyString },
        { M_getUserID,               Generic_emptyString },
};

MethodsShort methodsShort[] = {};

MethodsVoid methodsVoid[] = {
        { M_setupPaths,              GL2JNILib_setupPaths },
        { M_notifyTrophy,            GL2JNILib_notifyTrophy },
        { M_openIGP,                 GL2JNILib_openIGP },
        { M_nativeSetPreference,     SUtils_nativeSetPreference },
        { M_setPreference,           SUtils_setPreference },
        { M_removePreference,        SUtils_removePreference },
        { M_stopVideo,               SUtils_stopVideo },
        { M_inGameVideoOnPause,      SUtils_inGameVideoOnPause },
        { M_inGameVideoOnResume,     SUtils_inGameVideoOnResume },
        { M_inGameVideoSetSkipEnabled, SUtils_inGameVideoSetSkipEnabled },
        { M_Exit,                    GLGame_Exit },
        { M_splashScreenFunc,        GLGame_splashScreenFunc },
        { M_AudioTrack_play,         AudioTrack_play },
        { M_OpenInput,               GLGame_OpenInput },
        { M_AudioTrack_pause,        AudioTrack_pause },
        { M_AudioTrack_stop,         AudioTrack_stop },
        { M_AudioTrack_release,      AudioTrack_release },
};

/*
 * JNI Fields
 */

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

/*
 * OJO: un campo NO registrado devuelve id 0 (getFieldIdByName -> NULL), y el
 * id 0 es WINDOW_SERVICE: cualquier GetStatic*Field desconocido leeria
 * "window". Por eso se registran todos los que el .so pide (barrido de
 * GetStaticFieldID con literales, 2026-10-01): MODEL (getAndroidPhoneModel),
 * mPreferencesName (AndroidOS::GetSDFolder -> clave de getPreferenceString),
 * SERIAL (installer nativeGetDevID, no se llama).
 */
NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT }, 
		{ 1, "SDK_INT", FIELD_TYPE_INT },
		{ 2, "MODEL", FIELD_TYPE_OBJECT },
		{ 3, "mPreferencesName", FIELD_TYPE_OBJECT },
		{ 4, "SERIAL", FIELD_TYPE_OBJECT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
		{ 2, (jobject) DH3_MODEL },
		{ 3, (jobject) DH3_PREFS },
		{ 4, (jobject) "0" },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
