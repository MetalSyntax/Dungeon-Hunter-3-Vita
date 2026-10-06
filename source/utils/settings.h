/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

// ux0:data/dungeonhunter3/config.txt, one "name value" per line; written
// with the defaults on first boot.
extern bool setting_invertLeftX;   // movement stick
extern bool setting_invertLeftY;
extern bool setting_invertRightX;  // aim stick
extern bool setting_invertRightY;
extern int  setting_fpsCap;
extern bool setting_dbgNoIrr;      // experiment: drop the IR (iridescence) term of CharNormalSpec_VS        // 30 = the Android GL2JNIView throttle; 60 = uncapped (vsync)

void settings_load();
void settings_save();
void settings_reset();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
