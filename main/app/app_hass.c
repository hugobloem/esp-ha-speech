#include <stdio.h>
#include <string.h>
// #include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_mn_models.h"

#include "app_hass.h"
#include "app_sr.h"
#include "ui_net_config.h"

#include "app_api_rest.h"
#include "app_api_mqtt.h"

#include "cJSON.h"

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

void app_hass_add_cmd(char *msg) {
    // Parse message as json
    cJSON *root = cJSON_Parse(msg);
    if (root == NULL) {
        ESP_LOGE(TAG, "Error parsing json");
        cJSON_Delete(root);
        return;
    }
    // Get command to add
    cJSON *sr_txt = cJSON_GetObjectItemCaseSensitive(root, "text");
    cJSON *sr_phn = cJSON_GetObjectItemCaseSensitive(root, "phonetic");
    if (sr_txt == NULL || sr_phn == NULL) {
        ESP_LOGE(TAG, "Error parsing text");
        cJSON_Delete(root);
        return;
    } else {
        // Add sr command to speech recognition
        sr_cmd_t cmd_info = {0};
        cmd_info.cmd = SR_CMD;
        cmd_info.lang = SR_LANG_EN;
        cmd_info.id = 0;
        strcpy(cmd_info.str, sr_txt->valuestring);
        strcpy(cmd_info.phoneme, sr_phn->valuestring);

        app_sr_add_cmd(&cmd_info);
        app_sr_update_cmds();
        ESP_LOGI(TAG, "Added command: %s; %s", cmd_info.str, cmd_info.phoneme);
        cJSON_Delete(root);
        return;
    }
}

void app_hass_rm_all_cmd(char* msg) {
    // Parse message as json
    cJSON *root = cJSON_Parse(msg);
    if (root == NULL) {
        ESP_LOGE(TAG, "Error parsing json");
        cJSON_Delete(root);
        return;
    }
    // Get command to add
    cJSON *sr_txt = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (sr_txt == NULL) {
        ESP_LOGE(TAG, "Error parsing text");
        cJSON_Delete(root);
        return;
    } else if (strcmp(sr_txt->valuestring, "yes") == 0) {
        // remove sr command from speech recognition

        app_sr_remove_all_cmd();
        app_sr_update_cmds();
        ESP_LOGI(TAG, "Removed all commands");
        cJSON_Delete(root);
        return;
    }
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