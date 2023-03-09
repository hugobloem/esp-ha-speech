#pragma once
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_hass_init();
bool app_hass_is_connected(void);

void app_hass_send_cmd(char *cmd);

#ifdef __cplusplus
}
#endif
