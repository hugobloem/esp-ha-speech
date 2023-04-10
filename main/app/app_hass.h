#pragma once
#include <esp_err.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_hass_init();
bool app_hass_is_connected(void);

void app_hass_send_cmd(char *cmd);

void app_hass_add_cmd(char *cmd, char *phoneme, bool commit);
void app_hass_add_cmd_from_msg(cJSON *root);
void app_hass_rm_all_cmd(cJSON *root);

#ifdef __cplusplus
}
#endif
