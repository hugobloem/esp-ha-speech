#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_api_mqtt_start(void);
esp_err_t app_api_mqtt_send_recognised_cmd(char *payload);

#ifdef __cplusplus
}
#endif
