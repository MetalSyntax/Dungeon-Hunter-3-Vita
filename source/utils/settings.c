/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"

#define CONFIG_FILE_PATH DATA_PATH"config.txt"

bool setting_invertLeftX;
bool setting_invertLeftY;
bool setting_invertRightX;
bool setting_invertRightY;
int  setting_fpsCap;
bool setting_dbgNoIrr;

void settings_reset() {
    // log_022: the stick's Y came out inverted in game (the Xperia Play
    // touchpad's Y grows upward), so the default flips it.
    setting_invertLeftX  = false;
    setting_invertLeftY  = true;
    setting_invertRightX = false;
    setting_invertRightY = true;
    setting_fpsCap       = 30;
    setting_dbgNoIrr     = false;
}

void settings_load() {
    settings_reset();

    char buffer[32];
    int value;

    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        while (fscanf(config, "%31s %d", buffer, &value) == 2) {
            if      (strcmp("setting_invertLeftX", buffer) == 0)  setting_invertLeftX  = value != 0;
            else if (strcmp("setting_invertLeftY", buffer) == 0)  setting_invertLeftY  = value != 0;
            else if (strcmp("setting_invertRightX", buffer) == 0) setting_invertRightX = value != 0;
            else if (strcmp("setting_invertRightY", buffer) == 0) setting_invertRightY = value != 0;
            else if (strcmp("setting_fpsCap", buffer) == 0)       setting_fpsCap = value;
            else if (strcmp("setting_dbgNoIrr", buffer) == 0)     setting_dbgNoIrr = value != 0;
        }
        fclose(config);
    }
    if (setting_fpsCap < 15) setting_fpsCap = 15;
    if (setting_fpsCap > 60) setting_fpsCap = 60;
    settings_save();   // (re)write: adds keys introduced by newer builds
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "%s %d\n", "setting_invertLeftX",  (int) setting_invertLeftX);
        fprintf(config, "%s %d\n", "setting_invertLeftY",  (int) setting_invertLeftY);
        fprintf(config, "%s %d\n", "setting_invertRightX", (int) setting_invertRightX);
        fprintf(config, "%s %d\n", "setting_invertRightY", (int) setting_invertRightY);
        fprintf(config, "%s %d\n", "setting_fpsCap",       setting_fpsCap);
        fprintf(config, "%s %d\n", "setting_dbgNoIrr",     (int) setting_dbgNoIrr);
        fclose(config);
    }
}
