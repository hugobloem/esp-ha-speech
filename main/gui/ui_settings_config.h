#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANIM_SPD 500
#define ANIM_DLY 0

void ui_settings_config_start(void (*fn)(void));

#ifdef __cplusplus
}
#endif