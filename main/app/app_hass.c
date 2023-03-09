#include <stdio.h>
#include <string.h>
// #include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "esp_log.h"
#include "esp_event.h"

#include "app_hass.h"
#include "ui_net_config.h"

#include "app_api_rest.h"
#include "app_api_mqtt.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

#include "secrets.h"

#define CONV_API_PATH "/api/conversation/process"

static const char *TAG = "app_hass";
static bool hass_connected = false;


static void app_api_rest_test(void *pvParameters) {

    char response[MAX_HTTP_OUTPUT_BUFFER] = {0};
    app_api_rest_get("/api/", response);

    if (strcmp(response, "{\"message\":\"API running\"}")) {
        ui_net_config_update_cb(UI_NET_EVT_CLOUD_CONNECTED, NULL);
        hass_connected = true;
    } else {
        ui_net_config_update_cb(UI_NET_EVT_WIFI_CONNECTED, NULL);
        hass_connected = false;
    }

    ESP_LOGI(TAG, "Connected");

    vTaskDelete(NULL);
}

void app_hass_send_cmd(char *cmd)
{
#if NLU_MODE == NLU_RHASSPY

    ESP_LOGI(TAG, "Sending command to Rhasspy");
    app_api_mqtt_send_cmd("hermes/nlu/query", cmd);

#elif NLU_MODE == NLU_HASS

    ESP_LOGI(TAG, "Sending command to Home Assistant");
    char response[MAX_HTTP_OUTPUT_BUFFER] = {0};
    char *message = malloc(strlen(cmd) + 100);
    sprintf(message, "{\"text\": \"%s\"}", cmd);
    app_api_rest_post("/api/conversation/process", response, message);

#endif
}


void app_hass_init(void) 
{
#if NLU_MODE == NLU_RHASSPY

    ESP_LOGI(TAG, "Starting up");
    app_api_mqtt_start();

#elif NLU_MODE == NLU_HASS

    ESP_LOGI(TAG, "Starting up");
    xTaskCreate(&app_api_rest_test, "rest_test_task", 8192, NULL, 5, NULL);

#endif
}

bool app_hass_is_connected(void)
{
    return hass_connected;
}