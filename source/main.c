#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "utils/settings.h"
#include "video.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/openpsid.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

// Same shim DH2 needed (its PORTING_PLAN.md Phase 2.5): the vendored
// FalsoJNI_Logger.c calls game_log(), which only exists in the Zenonia
// lineage's main.c -- forward to this loader's l_info().
void game_log(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    l_info("%s", buf);
}

// DH3 JNI entry points (package com.gameloft.android.ANMP.GloftD3HM).
// Symbol names confirmed via `nm -D libDH3.so` 2026-10-01 (81 Java_*
// exports + JNI_OnLoad). JNI_OnLoad = GetEnv + AndroidOS::Init (FindClass
// SUtils/Device/GameInstaller; exit() if any is NULL) + VoxSetJavaVM +
// VoxSetAndroidAPILevel(9) + InAppBilling_setJavaVM. No RegisterNatives.
// Lifecycle order transcribed from jadx: GL2JNIActivity.a():
// GL2JNILib.init(F=0) -> GLGame.Init() -> EGL chooseConfig: initGL() +
// getViewSettings() -> onSurfaceCreated: stateChanged(true) ->
// onSurfaceChanged: resize(max,min) -> onDrawFrame: step() @~33ms.
// Called as JNI funcs: fn(&jni, clazz, args...) (same as DH2 port).

#define SCREEN_W 960
#define SCREEN_H 544

// GL2JNILib.init caches NewGlobalRef(clazz) as glf::sClassGL2JNILib and
// GLGame.Init calls GetObjectClass(thiz): hand them FindClass-style names
// instead of NULL so nothing downstream sees a NULL class.
#define CLS_GL2JNILIB ((void *) "com/gameloft/android/ANMP/GloftD3HM/GL2JNILib")
#define OBJ_GLGAME    ((void *) "com/gameloft/android/ANMP/GloftD3HM/GLGame")

// Android keycodes understood by glf::AndroidOnKeyDown/Up (0x849c64): the
// Xperia Play / MOGA gamepad set, mapped to Application::ZeusKeyPad. The
// 2nd arg (scanCode) is ignored by the native. Same table the MOGA listener
// builds (az.java): A->23, B->1000, X->99, Y->100. Xperia's Circle sends
// BACK+Alt which GL2JNIActivity.onKeyDown turns into 1000 as well.
#define AK_DPAD_UP     19
#define AK_DPAD_DOWN   20
#define AK_DPAD_LEFT   21
#define AK_DPAD_RIGHT  22
#define AK_DPAD_CENTER 23   // Zeus 7  (Cross)
#define AK_ZEUS_CIRCLE 1000 // Zeus 6  (GLGame.ad)
#define AK_BUTTON_X    99   // Zeus 5  (Square)
#define AK_BUTTON_Y    100  // Zeus 4  (Triangle)
#define AK_BUTTON_L1   102  // Zeus 13
#define AK_BUTTON_R1   103  // Zeus 14
#define AK_BUTTON_START  108 // Zeus 8
#define AK_BUTTON_SELECT 109 // Zeus 9

// Xperia Play touchpad (966x360), read by HUDControls::ProcessZeusGamePad
// (0x4b97b0): left pad centred at (180,180) = move stick, right pad
// centred at (780,180) = ranged-attack aim; Y is flipped by the native
// (rsb y,#180). Application::UpdateTouchPad zones: x<=366 left, >=600 right.
#define PAD_LEFT_CX   180
#define PAD_RIGHT_CX  780
#define PAD_CY        180
#define PAD_RADIUS    170
#define STICK_DEADZONE 28
// Middle pad (Application::UpdateTouchPad zone 2: 387 <= x <= 575) = swap
// weapon. The Xperia tutorial blocks on it ("Toca el centro del panel
// tactil para cambiar a tu otra arma"), and no stick can reach it.
#define PAD_MID_CX    481
#define PAD_W         966
#define PAD_H         360
// Finger ids on the emulated pad: 0/1 = sticks, 2..5 = rear panel, 9 = button.
#define PAD_ID_REAR   2
#define PAD_REAR_MAX  4
#define PAD_ID_BUTTON 9

void material_report(void);  // reimpl/material.c

static void *so_sym_or_warn(const char *name) {
    void *addr = (void *) so_symbol(&so_mod, name);
    if (!addr) l_warn("Symbol %s not found.", name);
    return addr;
}

#define DH3_SYM(n) so_sym_or_warn("Java_com_gameloft_android_ANMP_GloftD3HM_" n)

static void *DH3_SYM_DYN(const char *n) {
    char full[128];
    snprintf(full, sizeof(full), "Java_com_gameloft_android_ANMP_GloftD3HM_%s", n);
    return so_sym_or_warn(full);
}

// GLGame.onResume (GLGame.java:955): nativeGetUDID/MAC/DeviceModel/
// DeviceFirmare(String). Each native just stores GetStringUTFChars() in a
// global (deviceUDID & co.) that glot::TrackingManager's ctor strcpy()s
// unchecked: never sending them = strcpy(dst, NULL) (psp2dmp 2026-10-02,
// log_005, TrackingManager::TrackingManager 0xcbab8c). glot::GetMacAddress
// returns the nativeGetMAC global (same GOT slot 0xfc71a0).
static void dh3_send_device_info(void) {
    // Device.getDeviceId(): IMEI/serial on Android -> OpenPSID here: stable
    // per console, which is what the tracking/online UDID is for.
    static char udid[2 * sizeof(SceKernelOpenPsId) + 1] = "000000000000000";
    SceKernelOpenPsId psid;
    if (sceKernelGetOpenPsId(&psid) >= 0) {
        for (unsigned i = 0; i < sizeof(psid.id); i++)
            snprintf(udid + 2 * i, 3, "%02x", psid.id[i]);
    }
    static const struct { const char *sym; const char *value; } info[] = {
        { "GLGame_nativeGetUDID",          NULL },
        { "GLGame_nativeGetMAC",           "02:00:00:00:00:00" }, // Android's "no MAC" value
        { "GLGame_nativeGetDeviceModel",   "PS Vita_sony" },      // MODEL + "_" + manufacturer
        { "GLGame_nativeGetDeviceFirmare", "4.4" },               // VERSION.RELEASE (SDK_INT 19)
    };
    for (unsigned i = 0; i < sizeof(info) / sizeof(info[0]); i++) {
        void (* fn)(JNIEnv *, void *, jstring) = DH3_SYM_DYN(info[i].sym);
        if (fn) fn(&jni, OBJ_GLGAME, (jstring) (info[i].value ? info[i].value : udid));
    }
    l_info("[Java] device info sent (udid=%s).", udid);
}

// GLGame.OpenInput request queued by java.c (on-screen keyboard).
extern volatile int dh3_ime_pending;
extern int dh3_ime_maxlen, dh3_ime_type;

static void (* PadAdd)(JNIEnv *, void *, int, int, int);
static void (* PadUpdate)(JNIEnv *, void *, int, int, int);
static void (* PadRemove)(JNIEnv *, void *, int);

// Analog stick -> emulated Xperia touchpad finger `id`.
// inv_x/inv_y: config.txt. The native reads the pad with Y growing upward
// (Xperia Play touchpad), so a raw stick Y moved the hero the wrong way
// (log_022); setting_invert*Y defaults to on.
static void stick_to_pad(int id, int cx, int ax, int ay, int inv_x, int inv_y, int *active) {
    int dx = ax - 128, dy = ay - 128;
    if (inv_x) dx = -dx;
    if (inv_y) dy = -dy;
    if (dx * dx + dy * dy < STICK_DEADZONE * STICK_DEADZONE) {
        if (*active) {
            PadRemove(&jni, CLS_GL2JNILIB, id);
            *active = 0;
        }
        return;
    }
    int x = cx + dx * PAD_RADIUS / 128;
    int y = PAD_CY + dy * PAD_RADIUS / 128;
    if (!*active) {
        PadAdd(&jni, CLS_GL2JNILIB, id, x, y);
        *active = 1;
    } else {
        PadUpdate(&jni, CLS_GL2JNILIB, id, x, y);
    }
}

// Rear touch panel -> emulated Xperia touchpad, scaled to its 966x360 so
// left/middle/right thirds land in the same zones the native expects.
static void rear_to_pad(const SceTouchData *rear, const SceTouchPanelInfo *info,
                        int *ids, int *xs, int *ys) {
    int seen[PAD_REAR_MAX] = { 0 };
    int w = info->maxAaX - info->minAaX, h = info->maxAaY - info->minAaY;
    if (w <= 0 || h <= 0) return;
    for (int t = 0; t < rear->reportNum && t < PAD_REAR_MAX; t++) {
        int slot = rear->report[t].id % PAD_REAR_MAX;
        int x = (rear->report[t].x - info->minAaX) * (PAD_W - 1) / w;
        int y = (rear->report[t].y - info->minAaY) * (PAD_H - 1) / h;
        x = x < 0 ? 0 : (x >= PAD_W ? PAD_W - 1 : x);
        y = y < 0 ? 0 : (y >= PAD_H ? PAD_H - 1 : y);
        if (setting_invertLeftY) y = PAD_H - 1 - y;   // same axis convention as the sticks
        seen[slot] = 1;
        if (ids[slot] < 0) {
            ids[slot] = rear->report[t].id;
            PadAdd(&jni, CLS_GL2JNILIB, PAD_ID_REAR + slot, x, y);
        } else if (xs[slot] != x || ys[slot] != y) {
            PadUpdate(&jni, CLS_GL2JNILIB, PAD_ID_REAR + slot, x, y);
        }
        xs[slot] = x;
        ys[slot] = y;
    }
    for (int s = 0; s < PAD_REAR_MAX; s++) {
        if (ids[s] >= 0 && !seen[s]) {
            PadRemove(&jni, CLS_GL2JNILIB, PAD_ID_REAR + s);
            ids[s] = -1;
        }
    }
}

int main() {
    soloader_init_all();

    int (* JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    if (!JNI_OnLoad) {
        l_fatal("JNI_OnLoad not found in libDH3.so");
        sceKernelExitDeleteThread(0);
    }
    JNI_OnLoad(&jvm);
    l_success("JNI_OnLoad done.");

    // Audio: vox::DriverAndroid::Init picks OpenSL if API>8, else the
    // AudioTrack backend (pure JNI, implemented over sceAudioOut in java.c).
    // JNI_OnLoad just set 9; s_androidAPILevel is read only by that Init.
    void (* VoxSetAndroidAPILevel)(int) = (void *) so_symbol(&so_mod, "VoxSetAndroidAPILevel");
    if (VoxSetAndroidAPILevel) {
        VoxSetAndroidAPILevel(8);
        l_success("vox: forced AudioTrack backend (API level 8).");
    } else {
        l_warn("VoxSetAndroidAPILevel not found: vox will try OpenSL.");
    }

    void (* GL_init)(JNIEnv *, void *, int) = DH3_SYM("GL2JNILib_init");
    void (* GL_initGL)(JNIEnv *, void *) = DH3_SYM("GL2JNILib_initGL");
    void (* GL_getViewSettings)(JNIEnv *, void *) = DH3_SYM("GL2JNILib_getViewSettings");
    void (* GL_resize)(JNIEnv *, void *, int, int) = DH3_SYM("GL2JNILib_resize");
    void (* GL_stateChanged)(JNIEnv *, void *, int) = DH3_SYM("GL2JNILib_stateChanged");
    void (* GL_step)(JNIEnv *, void *) = DH3_SYM("GL2JNILib_step");
    void (* Game_Init)(JNIEnv *, void *) = DH3_SYM("GLGame_Init");
    int (* GL_getNumExtraContext)(JNIEnv *, void *) = DH3_SYM("GL2JNILib_getNumExtraContext");
    void (* GL_setNumExtraContext)(JNIEnv *, void *, int) = DH3_SYM("GL2JNILib_setNumExtraContext");
    // Screen: touchEvent(action 0=up/1=down/2=move, x, y, pointerId)
    // -> glf::AndroidAddTouchEvent(iiii) 0x849980 (GL2JNIActivity.java:589).
    // NOT nativeAdd/Update/RemoveTouchEvent: those are only sent for
    // SOURCE_TOUCHPAD (Xperia Play pad) and feed Application::UpdateTouchPad.
    void (* TouchEvent)(JNIEnv *, void *, int, int, int, int) = DH3_SYM("GL2JNILib_touchEvent");
    void (* KeyDown)(JNIEnv *, void *, int, int) = DH3_SYM("GL2JNILib_nativeOnKeyDown");
    void (* KeyUp)(JNIEnv *, void *, int, int) = DH3_SYM("GL2JNILib_nativeOnKeyUp");
    void (* KeyboardEnabled)(JNIEnv *, void *, int, int, int, int) = DH3_SYM("GL2JNILib_nativeKeyboardEnabled");
    PadAdd = DH3_SYM("GL2JNILib_nativeAddTouchEvent");
    PadUpdate = DH3_SYM("GL2JNILib_nativeUpdateTouchEvent");
    PadRemove = DH3_SYM("GL2JNILib_nativeRemoveTouchEvent");
    void **app_instance = (void **) so_symbol(&so_mod, "_ZN11Application10s_instanceE");
    void (* EditorEnd)(JNIEnv *, void *, jstring) = DH3_SYM("GLGame_nativeOnEditorEnd");
    void (* HideKeyboard)(JNIEnv *, void *) = DH3_SYM("GLGame_nativeHideKeyboard");

    if (!GL_init || !GL_step) {
        l_fatal("Core loop symbols (init/step) missing -- aborting.");
        sceKernelExitDeleteThread(0);
    }

    // The .so imports no EGL at all (Java owned it): just bring up vitaGL.
    gl_init();

    // Intro: res/raw/logo.mp4 (Gameloft logo, 3 s) ships in the APK but
    // v1.5.2's Java never plays it (SUtils.playVideo is `return false`, no
    // reference to R.raw.logo; Android shows the static_logo drawable
    // instead). Played here, before the engine boots, as the port's splash:
    // SceAvPlayer + vitaGL YUV->RGB (video.cpp, Shadow Guardian method:
    // tiered CDRAM -> PHYCONT -> UNCACHE texture allocator + full-res upload).
    // Skippable with Cross/Start; a missing/broken file just returns.
    // The APK's logo.mp4 is MPEG-4 Part 2 Simple Profile, which SceAvPlayer
    // can't decode (log_008: STATE_STOP after the header, 0 frames). The
    // port ships an H.264 960x544 transcode next to it. Main profile with
    // B-frames and no audio track STOPs after the header with 0 frames even
    // with PHYCONT memory (log_017), so: baseline, no B-frames, silent AAC.
    //   ffmpeg -i logo.mp4 -f lavfi -i anullsrc=r=48000:cl=stereo
    //     -vf scale=960:544,setsar=1 -c:v libx264 -profile:v baseline
    //     -level 3.1 -bf 0 -g 25 -pix_fmt yuv420p -b:v 1500k -maxrate 2000k
    //     -bufsize 2000k -c:a aac -b:a 64k -ar 48000 -ac 2 -shortest
    //     -movflags +faststart logo_vita.mp4
    video_init();
    video_play("res/raw/logo_vita.mp4");
    video_shutdown();
    gl_phycont_late_init();

    GL_init(&jni, CLS_GL2JNILIB, 0);              // GLGame.F = 0 (GLGame.java:92)
    // NO llamar setupPaths/setPaths aca: glf::AndroidSetPaths escribe en
    // App::GetFs() (= App + 0x100e8) y la App todavia no existe -> crash
    // real en consola (psp2dmp 2026-10-01: strcpy a 0x000100e8). El motor lo
    // hace solo: getViewSettings() -> NewApp() -> AndroidSetupPaths() ->
    // Java setupPaths (nuestro handler) -> setPaths -> App::MyInit().
    if (Game_Init) Game_Init(&jni, OBJ_GLGAME);
    dh3_send_device_info();
    if (GL_initGL) GL_initGL(&jni, CLS_GL2JNILIB);   // stores JNIEnv in TLS
    if (GL_getViewSettings) GL_getViewSettings(&jni, CLS_GL2JNILIB);  // creates glf::App
    else l_fatal("getViewSettings missing: glf::App will never be created.");
    // EGL context creation (ag.java:20, onSurfaceCreated): Java asks
    // getNumExtraContext() for the shared pbuffer contexts the engine wants
    // (glitch CCpuTaskHandlerThread workers -> App::ReserveContext) and calls
    // setNumExtraContext(created) when it gets fewer. vitaGL has exactly one
    // context, so report 0 like a device whose extra eglCreateContext failed:
    // ReserveContext then returns false without touching Java and the
    // workers hand GL work back to the main thread, which owns context 0
    // (App::SetAsMainThread -> setCurrentContext(0)).
    if (GL_getNumExtraContext && GL_setNumExtraContext) {
        int wanted = GL_getNumExtraContext(&jni, CLS_GL2JNILIB);
        GL_setNumExtraContext(&jni, CLS_GL2JNILIB, 0);
        l_info("[gl] engine wanted %d extra GL context(s), Vita gives 0.", wanted);
    }
    if (GL_stateChanged) GL_stateChanged(&jni, CLS_GL2JNILIB, 1);
    if (GL_resize) GL_resize(&jni, CLS_GL2JNILIB, SCREEN_W, SCREEN_H);

    l_success("Starting main loop...");
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    SceTouchPanelInfo rear_info;
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rear_info) < 0) {
        // Vita 1000/2000 rear panel active area.
        rear_info.minAaX = 0;   rear_info.maxAaX = 1919;
        rear_info.minAaY = 108; rear_info.maxAaY = 889;
    }
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    static const struct { unsigned int btn; int code; } keymap[] = {
        { SCE_CTRL_UP,       AK_DPAD_UP },
        { SCE_CTRL_DOWN,     AK_DPAD_DOWN },
        { SCE_CTRL_LEFT,     AK_DPAD_LEFT },
        { SCE_CTRL_RIGHT,    AK_DPAD_RIGHT },
        { SCE_CTRL_CROSS,    AK_DPAD_CENTER },
        { SCE_CTRL_CIRCLE,   AK_ZEUS_CIRCLE },
        { SCE_CTRL_SQUARE,   AK_BUTTON_X },
        { SCE_CTRL_TRIANGLE, AK_BUTTON_Y },
        // Ext2 reports the Vita's L/R (and a DS3's L1/R1) as L1/R1, not
        // LTRIGGER/RTRIGGER (= L2/R2): log_022, the potion tutorial's
        // "right trigger" never fired.
        { SCE_CTRL_L1,       AK_BUTTON_L1 },
        { SCE_CTRL_R1,       AK_BUTTON_R1 },
        { SCE_CTRL_START,    AK_BUTTON_START },
        { SCE_CTRL_SELECT,   AK_BUTTON_SELECT },
    };

    SceCtrlData pad;
    SceTouchData touch, rear;
    unsigned int old_buttons = 0;
    int rear_id[PAD_REAR_MAX], rear_x[PAD_REAR_MAX], rear_y[PAD_REAR_MAX];
    for (int i = 0; i < PAD_REAR_MAX; i++) rear_id[i] = -1;
    // front panel is 960x544 = resize() size: pass-through, no scaling.
    int touch_id[8], touch_x[8], touch_y[8];
    for (int i = 0; i < 8; i++) touch_id[i] = -1;
    int lpad_active = 0, rpad_active = 0, old_held_mid = 0;
    int zeus_sent = 0;
    unsigned int frame = 0;
    int ime_active = 0;
    SceUInt64 frame_start = sceKernelGetProcessTimeWide(), fps_t0 = frame_start;
    unsigned int fps_frames = 0, fps_shown = 0;
    l_info("[config] invert L(x=%d y=%d) R(x=%d y=%d), fps cap %d (ux0:data/dungeonhunter3/config.txt)",
           setting_invertLeftX, setting_invertLeftY, setting_invertRightX, setting_invertRightY,
           setting_fpsCap);

    while (1) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) <= 0) {
            // Non-Ext2 read: L/R come as LTRIGGER/RTRIGGER; move them to
            // L1/R1 so the keymap (and L2/R2 = swap weapon) see Ext2 layout.
            sceCtrlPeekBufferPositive(0, &pad, 1);
            unsigned int lr = pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
            pad.buttons &= ~lr;
            if (lr & SCE_CTRL_LTRIGGER) pad.buttons |= SCE_CTRL_L1;
            if (lr & SCE_CTRL_RTRIGGER) pad.buttons |= SCE_CTRL_R1;
        }
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1);

        // Present as an Xperia Play with the gamepad slid open, as
        // GL2JNIActivity does: nativeKeyboardEnabled(isZeus, kbOpen,
        // xoSwapped=false, true). Application::SetKeyboardEnabled only runs
        // once Application::s_instance exists (created inside step()).
        if (!zeus_sent && KeyboardEnabled && app_instance && *app_instance) {
            KeyboardEnabled(&jni, CLS_GL2JNILIB, 1, 1, 0, 1);
            zeus_sent = 1;
            l_success("[input] Xperia/Zeus gamepad mode enabled (frame %u).", frame);
        }

        // On-screen keyboard: mirrors GLGame's EditText flow. The game keeps
        // rendering underneath (vitaGL's swap composites the dialog); game
        // input is frozen while the IME owns the screen.
        if (dh3_ime_pending && !ime_active) {
            dh3_ime_pending = 0;
            const char *default_name = "Hero";
            if (init_ime_dialog_ex("Dungeon Hunter 3", default_name,
                                   dh3_ime_maxlen > 0 ? dh3_ime_maxlen : 8,
                                   dh3_ime_type == 1) >= 0) {
                ime_active = 1;
            } else {
                l_warn("[ime] sceImeDialogInit failed, fallback to default '%s'", default_name);
                if (EditorEnd) EditorEnd(&jni, OBJ_GLGAME, (jstring) default_name);
                if (HideKeyboard) HideKeyboard(&jni, OBJ_GLGAME);
            }
        }
        if (ime_active) {
            char *text = get_ime_dialog_result();
            if (text) {
                ime_active = 0;
                const char *chosen = (text[0] != '\0') ? text : "Hero";
                l_info("[ime] result text='%s'", chosen);
                if (EditorEnd) EditorEnd(&jni, OBJ_GLGAME, (jstring) chosen);
                if (HideKeyboard) HideKeyboard(&jni, OBJ_GLGAME);
                old_buttons = pad.buttons;  // don't replay the confirm press
            }
            GL_step(&jni, CLS_GL2JNILIB);
            gl_swap(1);
            frame++;
            sceKernelDelayThread(33 * 1000);
            continue;
        }

        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;

        if (KeyDown && KeyUp) {
            for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
                if (pressed & keymap[i].btn) KeyDown(&jni, CLS_GL2JNILIB, keymap[i].code, 0);
                if (released & keymap[i].btn) KeyUp(&jni, CLS_GL2JNILIB, keymap[i].code, 0);
            }
        }

        if (zeus_sent && PadAdd && PadUpdate && PadRemove) {
            stick_to_pad(0, PAD_LEFT_CX, pad.lx, pad.ly,
                         setting_invertLeftX, setting_invertLeftY, &lpad_active);
            stick_to_pad(1, PAD_RIGHT_CX, pad.rx, pad.ry,
                         setting_invertRightX, setting_invertRightY, &rpad_active);
            rear_to_pad(&rear, &rear_info, rear_id, rear_x, rear_y);
            // L2/R2/L3/R3 (PSTV pads, or rear-touch remap plugins) = a
            // press on the middle pad = swap weapon.
            const unsigned int mid_btns = SCE_CTRL_L2 | SCE_CTRL_R2 | SCE_CTRL_L3 | SCE_CTRL_R3;
            int held_mid = (pad.buttons & mid_btns) != 0;
            if (held_mid && !old_held_mid)
                PadAdd(&jni, CLS_GL2JNILIB, PAD_ID_BUTTON, PAD_MID_CX, PAD_CY);
            else if (!held_mid && old_held_mid)
                PadRemove(&jni, CLS_GL2JNILIB, PAD_ID_BUTTON);
            old_held_mid = held_mid;
        }

        if (TouchEvent) {
            int seen[8] = { 0 };
            for (int t = 0; t < touch.reportNum && t < 8; t++) {
                int id = touch.report[t].id;
                int x = touch.report[t].x / 2;   // touch panel is 1920x1088
                int y = touch.report[t].y / 2;
                int slot = id % 8;
                seen[slot] = 1;
                if (touch_id[slot] < 0) {
                    touch_id[slot] = id;
                    touch_x[slot] = x;
                    touch_y[slot] = y;
                    TouchEvent(&jni, CLS_GL2JNILIB, 1, x, y, slot);
                } else if (touch_x[slot] != x || touch_y[slot] != y) {
                    touch_x[slot] = x;
                    touch_y[slot] = y;
                    TouchEvent(&jni, CLS_GL2JNILIB, 2, x, y, slot);
                }
            }
            for (int s = 0; s < 8; s++) {
                if (touch_id[s] >= 0 && !seen[s]) {
                    TouchEvent(&jni, CLS_GL2JNILIB, 0, touch_x[s], touch_y[s], s);
                    touch_id[s] = -1;
                }
            }
        }

        // onDrawFrame: splash?nativeSplashScreenFunc:step(), throttled ~33ms
        // (GL2JNIView.java). Splash path omitted: engine boots to step().
        GL_step(&jni, CLS_GL2JNILIB);
        gl_swap(0);
        frame++;
        log_flush_if_due();
        // Heartbeat: tells a hang (log stops, no psp2dmp) from a slow load.
        if (frame == 1 || frame == 30 || frame % 300 == 0) {
            l_info("[loop] frame %u alive, %u fps.", frame, fps_shown);
            material_report();
        }

        // Frame pacing: GL2JNIView caps a FRAME at ~33 ms. This loop used
        // to sleep 33 ms on top of step()+swap, so a 33 ms frame took 66 ms
        // (~15 fps, user report after log_022). Sleep only what's left.
        SceUInt64 now = sceKernelGetProcessTimeWide();
        SceUInt64 frame_us = 1000000u / (unsigned) setting_fpsCap;
        if (now - frame_start < frame_us)
            sceKernelDelayThread((SceUInt32) (frame_us - (now - frame_start)));
        frame_start = sceKernelGetProcessTimeWide();
        fps_frames++;
        if (frame_start - fps_t0 >= 1000000u) {
            fps_shown = fps_frames;
            fps_frames = 0;
            fps_t0 = frame_start;
        }
    }
}
