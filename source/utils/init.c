/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

/*
 * Save bug (log_022): the engine deletes dh3_profile/dh3_000 at boot for a
 * reason not yet known (io.c now logs every remove/rename with its caller).
 * Until fixed, keep a copy of every save from the previous session so
 * progress can be restored by hand: files/X -> savebackup/X.
 */
static void backup_savegames(void) {
    static const char *names[] = {
        "dh3_profile.savegame", "dh3_profile.savegame.bak",
        "dh3_000.savegame", "dh3_001.savegame", "dh3_002.savegame",
        "dh3_000.savegame.bak", "dh3_001.savegame.bak", "dh3_002.savegame.bak",
        "dh2_settings.savegame",
    };
    char src[256], dst[256];
    int n = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(src, sizeof(src), DATA_PATH "files/%s", names[i]);
        uint8_t *buf;
        size_t size;
        if (!file_exists(src) || !file_load(src, &buf, &size)) continue;
        snprintf(dst, sizeof(dst), DATA_PATH "savebackup/%s", names[i]);
        file_mkpath(dst, 0777);
        if (size > 0 && file_save(dst, buf, size)) n++;
        free(buf);
    }
    l_info("[save] %d savegame(s) copied to " DATA_PATH "savebackup/", n);
}

extern so_module so_mod;

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", SO_PATH);
    }

    settings_load();
    l_success("Settings loaded.");

    backup_savegames();

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");
}
