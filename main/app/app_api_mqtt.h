#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_api_mqtt_start(void);
void app_api_mqtt_send_cmd(char *topic, char *payload);

#ifdef __cplusplus
}
#endif
