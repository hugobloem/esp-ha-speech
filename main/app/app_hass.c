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
#include "esp_mn_speech_commands.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_hass.h"
#include "app_sr.h"
#include "ui_net_config.h"

#include "app_api_rest.h"
#include "app_api_mqtt.h"

#include "cJSON.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

#define NAME_SPACE "sr_cmds"

#include "secrets.h"

#define CONV_API_PATH "/api/conversation/process"

static const char *TAG = "app_hass";
static bool hass_connected = false;
static uint32_t keynum = 0;

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

esp_err_t app_hass_write_cmd_to_nvs(uint32_t keynum, char *cmd, char *phoneme)
{
    ESP_LOGI(TAG, "Saving cmd %d to NVS", keynum);
    nvs_handle_t my_handle = {0};
    esp_err_t err = nvs_open(NAME_SPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        char cmd_key[10];
        char phoneme_key[10];
        sprintf(cmd_key, "cmd%d", keynum);
        sprintf(phoneme_key, "pho%d", keynum);
        err = nvs_set_str(my_handle, cmd_key, cmd);
        err = nvs_set_str(my_handle, phoneme_key, phoneme);
        ESP_LOGI(TAG, "Saving cmd %d to NVS", keynum);
        err |= nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    return ESP_OK == err ? ESP_OK : ESP_FAIL;
}

esp_err_t app_hass_write_cmds_to_nvs(void)
{
    ESP_LOGI(TAG, "Saving cmds to NVS");
    // settings_check(&g_sys_param);
    int keynum = 0;
    esp_err_t err = ESP_OK;
    while (keynum < 201) {
        sr_cmd_t *cmd_info = app_sr_get_cmd_from_id(keynum);
        if (cmd_info == NULL) {
            break;
        }
        err = app_hass_write_cmd_to_nvs(keynum, cmd_info->str, cmd_info->phoneme);
        keynum++;
    }
    return ESP_OK == err ? ESP_OK : ESP_FAIL;
}

esp_err_t app_hass_read_cmds_from_nvs(void)
{
    ESP_LOGI(TAG, "Reading cmds from NVS");
    nvs_handle_t my_handle = {0};
    esp_err_t err = nvs_open(NAME_SPACE, NVS_READWRITE, &my_handle);
    uint32_t keynum = 0;
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        while (keynum < 201) {
            char cmd[SR_CMD_STR_LEN_MAX];
            char phoneme[SR_CMD_PHONEME_LEN_MAX];
            size_t cmd_len = sizeof(cmd);
            size_t phoneme_len = sizeof(phoneme);
            char cmd_key[10];
            char phoneme_key[10];
            sprintf(cmd_key, "cmd%d", keynum);
            sprintf(phoneme_key, "pho%d", keynum);
            err = nvs_get_str(my_handle, cmd_key, cmd, &cmd_len);
            err = nvs_get_str(my_handle, phoneme_key, phoneme, &phoneme_len);
            if (err == ESP_OK) {
                // add cmd to sr
                ESP_LOGI(TAG, "Read cmd %d from NVS", keynum);
                app_hass_add_cmd(cmd, phoneme, false);
            }
            keynum++;
        }
    }
    app_sr_update_cmds();
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t app_hass_rm_cmds_from_nvs(void)
{
    ESP_LOGI(TAG, "Removing cmds from NVS");
    nvs_handle_t my_handle = {0};
    esp_err_t err = nvs_open(NAME_SPACE, NVS_READWRITE, &my_handle);
    uint32_t keynum = 0;
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        while (keynum < 201) {
            char cmd_key[10];
            char phoneme_key[10];
            sprintf(cmd_key, "cmd%d", keynum);
            sprintf(phoneme_key, "pho%d", keynum);
            err = nvs_erase_key(my_handle, cmd_key);
            err = nvs_erase_key(my_handle, phoneme_key);
            keynum++;
        }
        ESP_LOGI(TAG, "Removed %d cmds from NVS", keynum);
        err |= nvs_commit(my_handle);
        nvs_close(my_handle);
    }
    return ESP_OK;
}

void app_hass_add_cmd(char *cmd, char *phoneme, bool commit)
{
    sr_cmd_t cmd_info = {0};
    cmd_info.cmd = SR_CMD;
    cmd_info.lang = SR_LANG_EN;
    cmd_info.id = 0;
    memcpy(cmd_info.str, cmd, strlen(cmd));
    memcpy(cmd_info.phoneme, phoneme, strlen(phoneme));
    app_sr_add_cmd(&cmd_info);
    ESP_LOGI(TAG, "Added cmd %d to sr", cmd_info.id);
    ESP_LOGI(TAG, "\tcmd: %d", cmd_info.cmd);
    ESP_LOGI(TAG, "\tstr: %s", cmd_info.str);
    ESP_LOGI(TAG, "\tpho: %s", cmd_info.phoneme);
    if (commit) {
        app_sr_update_cmds();
    }
}

void app_hass_add_cmd_from_msg(cJSON *root) {
    // Get command to add
    cJSON *sr_txt = cJSON_GetObjectItemCaseSensitive(root, "text");
    cJSON *sr_phn = cJSON_GetObjectItemCaseSensitive(root, "phonetic");
    if (sr_txt == NULL || sr_phn == NULL) {
        ESP_LOGE(TAG, "Error parsing text");
        cJSON_Delete(root);
        return;
    } else {
        // Add sr command to speech recognition
        app_hass_add_cmd(sr_txt->valuestring, sr_phn->valuestring, true);

        app_hass_write_cmd_to_nvs(keynum, sr_txt->valuestring, sr_phn->valuestring);
        ESP_LOGI(TAG, "Added command: %s; %s", sr_txt->valuestring, sr_phn->valuestring);
        cJSON_Delete(root);
        return;
    }
}

void app_hass_rm_all_cmd(cJSON *root) {
    // Get command to add
    cJSON *sr_txt = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (sr_txt == NULL) {
        ESP_LOGE(TAG, "Error parsing text");
        cJSON_Delete(root);
        return;
    } else if (strcmp(sr_txt->valuestring, "yes") == 0) {
        // remove sr command from speech recognition
        app_sr_remove_all_cmd();
        esp_mn_commands_free();
        esp_mn_commands_alloc();
        app_sr_update_cmds();

        // remove commands from nvs
        app_hass_rm_cmds_from_nvs();

        ESP_LOGI(TAG, "Removed all commands");
        cJSON_Delete(root);
        return;
    }
}


void app_hass_init(void) 
{
    // Load default speech commands or load them
    nvs_handle_t nvs_handle = {0};
    size_t required_size = 100;
    esp_err_t ret = nvs_open(NAME_SPACE, NVS_READWRITE, &nvs_handle);
    ret = nvs_get_str(nvs_handle, "cmd0", NULL, &required_size);
    ESP_LOGW(TAG, "NVS ret = %d", ret);
    nvs_close(nvs_handle);
    if (ESP_ERR_NVS_NOT_FOUND == ret) {
        ESP_LOGW(TAG, "Cmd NVS not found, creating new one");
        app_hass_write_cmds_to_nvs();
    } else if (ESP_OK == ret) {
        app_sr_remove_all_cmd();
        esp_mn_commands_free();
        esp_mn_commands_alloc();
        app_hass_read_cmds_from_nvs();
    } else {
        ESP_LOGE(TAG, "Error opening NVS (%s)", esp_err_to_name(ret));
    }

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