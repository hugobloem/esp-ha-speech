/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_sr.h"

#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "app_sr_handler.h"
#include "model_path.h"
#include "bsp_board.h"
#include "settings.h"

static const char *TAG = "app_sr";

typedef struct {
    sr_language_t lang;
    model_iface_data_t *model_data;
    const esp_mn_iface_t *multinet;
    const esp_afe_sr_iface_t *afe_handle;
    esp_afe_sr_data_t *afe_data;
    int16_t *afe_in_buffer;
    int16_t *afe_out_buffer;
    SLIST_HEAD(sr_cmd_list_t, sr_cmd_t) cmd_list;
    uint8_t cmd_num;
    TaskHandle_t feed_task;
    TaskHandle_t detect_task;
    TaskHandle_t handle_task;
    QueueHandle_t result_que;
    EventGroupHandle_t event_group;

    FILE *fp;
    bool b_record_en;
} sr_data_t;

static esp_afe_sr_iface_t *afe_handle = NULL;
static srmodel_list_t *models = NULL;

static sr_data_t *g_sr_data = NULL;

#define I2S_CHANNEL_NUM     (2)
#define NEED_DELETE BIT0
#define FEED_DELETED BIT1
#define DETECT_DELETED BIT2

/**
 * @brief all default commands
 */
static const sr_cmd_t g_default_cmd_info[] = {
    // English
    {SR_CMD,  SR_LANG_EN, 0, "Turn On the Light",  "TkN nN jc LiT", {NULL}},
    {SR_CMD, SR_LANG_EN, 0, "Turn Off the Light", "TkN eF jc LiT", {NULL}},
};

static void audio_feed_task(void *arg)
{
    size_t bytes_read = 0;
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *) arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = 3;
    ESP_LOGI(TAG, "audio_chunksize=%d, feed_channel=%d", audio_chunksize, feed_channel);

    /* Allocate audio buffer and check for result */
    int16_t *audio_buffer = heap_caps_malloc(audio_chunksize * sizeof(int16_t) * feed_channel, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (NULL == audio_buffer) {
        esp_system_abort("No mem for audio buffer");
    }
    g_sr_data->afe_in_buffer = audio_buffer;

    while (true) {
        if (NEED_DELETE && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, FEED_DELETED);
            vTaskDelete(NULL);
        }

        /* Read audio data from I2S bus */
        bsp_codec_config_t *codec_handle = bsp_board_get_codec_handle();
        codec_handle->i2s_read_fn((char *)audio_buffer, audio_chunksize * I2S_CHANNEL_NUM * sizeof(int16_t), &bytes_read, portMAX_DELAY);

        /* Save audio data to file if record enabled */
        if (g_sr_data->b_record_en && (NULL != g_sr_data->fp)) {
            fwrite(audio_buffer, 1, audio_chunksize * I2S_CHANNEL_NUM * sizeof(int16_t), g_sr_data->fp);
        }

        /* Channel Adjust */
        for (int  i = audio_chunksize - 1; i >= 0; i--) {
            audio_buffer[i * 3 + 2] = 0;
            audio_buffer[i * 3 + 1] = audio_buffer[i * 2 + 1];
            audio_buffer[i * 3 + 0] = audio_buffer[i * 2 + 0];
        }

        /* Feed samples of an audio stream to the AFE_SR */
        afe_handle->feed(afe_data, audio_buffer);
    }
}

static void audio_detect_task(void *arg)
{
    bool detect_flag = false;
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    //int nch = afe_handle->get_channel_num(afe_data);

    int mu_chunksize = g_sr_data->multinet->get_samp_chunksize(g_sr_data->model_data);
    assert(mu_chunksize == afe_chunksize);
    ESP_LOGI(TAG, "------------detect start------------\n");

    while (true) {
        if (NEED_DELETE && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, DETECT_DELETED);
            vTaskDelete(g_sr_data->handle_task);
            vTaskDelete(NULL);
        }

        afe_fetch_result_t* res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, LOG_BOLD(LOG_COLOR_GREEN) "wakeword detected");
            sr_result_t result = {
                .wakenet_mode = WAKENET_DETECTED,
                .state = ESP_MN_STATE_DETECTING,
                .command_id = 0,
            };
            xQueueSend(g_sr_data->result_que, &result, 0);
        }
        else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            detect_flag = true;
            g_sr_data->afe_handle->disable_wakenet(afe_data);
            ESP_LOGI(TAG, LOG_BOLD(LOG_COLOR_GREEN) "AFE_FETCH_CHANNEL_VERIFIED, channel index: %d\n", res->trigger_channel_id);
        }

        if (true == detect_flag) {
            /* Save audio data to file if record enabled */
            if (g_sr_data->b_record_en && (NULL != g_sr_data->fp)) {
                fwrite(res->data, 1, afe_chunksize * sizeof(int16_t), g_sr_data->fp);
            }

            esp_mn_state_t mn_state = ESP_MN_STATE_DETECTING;
            if (false == sr_echo_is_playing()) {
                mn_state = g_sr_data->multinet->detect(g_sr_data->model_data, res->data);
            } else {
                continue;
            }

            if (ESP_MN_STATE_DETECTING == mn_state) {
                continue;
            }

            if (ESP_MN_STATE_TIMEOUT == mn_state) {
                ESP_LOGW(TAG, "Time out");
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = mn_state,
                    .command_id = 0,
                };
                xQueueSend(g_sr_data->result_que, &result, 0);
                g_sr_data->afe_handle->enable_wakenet(afe_data);
                detect_flag = false;
                continue;
            }

            if (ESP_MN_STATE_DETECTED == mn_state) {
                esp_mn_results_t *mn_result = g_sr_data->multinet->get_results(g_sr_data->model_data);
                for (int i = 0; i < mn_result->num; i++) {
                    printf("TOP %d, command_id: %d, phrase_id: %d, prob: %f\n",
                        i + 1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->prob[i]);
                }

                int sr_command_id = mn_result->command_id[0];
                ESP_LOGI(TAG, "Deteted command : %d", sr_command_id);
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = mn_state,
                    .command_id = sr_command_id,
                };
                xQueueSend(g_sr_data->result_que, &result, 0);
#if !SR_CONTINUE_DET
                g_sr_data->afe_handle->enable_wakenet(afe_data);
                detect_flag = false;
#endif

                if (g_sr_data->b_record_en && (NULL != g_sr_data->fp)) {
                    ESP_LOGI(TAG, "File saved");
                    fclose(g_sr_data->fp);
                    g_sr_data->fp = NULL;
                }
                continue;
            }
            ESP_LOGE(TAG, "Exception unhandled");
        }
    }
    /* Task never returns */
    vTaskDelete(NULL);
}

esp_err_t app_sr_set_language(sr_language_t new_lang)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");

    if (new_lang == g_sr_data->lang) {
        ESP_LOGW(TAG, "nothing to do");
        return ESP_OK;
    } else {
        g_sr_data->lang = new_lang;
    }

    ESP_LOGW(TAG, "Set language to %s", SR_LANG_EN == g_sr_data->lang ? "EN" : "CN");
    if (g_sr_data->model_data) {
        g_sr_data->multinet->destroy(g_sr_data->model_data);
    }

    // remove all command
    app_sr_remove_all_cmd();
    esp_mn_commands_free();

    esp_mn_commands_alloc();
    g_sr_data->cmd_num = 0;

    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, (SR_LANG_EN == g_sr_data->lang ? "hiesp" : "hilexin"));
    g_sr_data->afe_handle->set_wakenet(g_sr_data->afe_data, wn_name);
    ESP_LOGI(TAG, "load wakenet:%s", wn_name);

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ((SR_LANG_EN == g_sr_data->lang) ? ESP_MN_ENGLISH : ESP_MN_CHINESE));
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 5760);
    g_sr_data->multinet = multinet;
    g_sr_data->model_data = model_data;
    ESP_LOGI(TAG, "load multinet:%s,%d,%d", mn_name, sizeof(esp_mn_iface_t), sizeof(esp_mn_iface_t));

    uint8_t cmd_number = 0;
    // count command number
    for (size_t i = 0; i < sizeof(g_default_cmd_info) / sizeof(sr_cmd_t); i++) {
        if (g_default_cmd_info[i].lang == g_sr_data->lang) {
            app_sr_add_cmd(&g_default_cmd_info[i]);
            cmd_number++;
        }
    }
    ESP_LOGI(TAG, "cmd_number=%d", cmd_number);
    return app_sr_update_cmds();/* Reset command list */
}

esp_err_t app_sr_start(bool record_en)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(NULL == g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR already running");

    g_sr_data = heap_caps_calloc(1, sizeof(sr_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_NO_MEM, TAG, "Failed create sr data");

    g_sr_data->result_que = xQueueCreate(3, sizeof(sr_result_t));
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->result_que, ESP_ERR_NO_MEM, err, TAG, "Failed create result queue");

    g_sr_data->event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->event_group, ESP_ERR_NO_MEM, err, TAG, "Failed create event_group");

    SLIST_INIT(&g_sr_data->cmd_list);

    /* Create file if record to SD card enabled*/
    g_sr_data->b_record_en = record_en;
    if (record_en) {
        char file_name[32];
        for (size_t i = 0; i < 100; i++) {
            sprintf(file_name, "/sdcard/Record_%02d.pcm", i);
            g_sr_data->fp = fopen(file_name, "r");
            fclose(g_sr_data->fp);
            if (NULL == g_sr_data->fp) {
                break;
            }
        }
        g_sr_data->fp = fopen(file_name, "w");
        ESP_GOTO_ON_FALSE(NULL != g_sr_data->fp, ESP_FAIL, err, TAG, "Failed create record file");
        ESP_LOGI(TAG, "File created at %s", file_name);
    }

    BaseType_t ret_val;

    models = esp_srmodel_init("model");
    afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();

    afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config.aec_init = false;

    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);
    g_sr_data->afe_handle = afe_handle;
    g_sr_data->afe_data = afe_data;

    sys_param_t *param = settings_get_parameter();
    g_sr_data->lang = SR_LANG_MAX;
    ret = app_sr_set_language(param->sr_lang);
    ESP_GOTO_ON_FALSE(ESP_OK == ret, ESP_FAIL, err, TAG,  "Failed to set language");

    ret_val = xTaskCreatePinnedToCore(&audio_feed_task, "Feed Task", 4 * 1024, (void*)afe_data, 5, &g_sr_data->feed_task, 0);
    ESP_GOTO_ON_FALSE(pdPASS == ret_val, ESP_FAIL, err, TAG,  "Failed create audio feed task");

    ret_val = xTaskCreatePinnedToCore(&audio_detect_task, "Detect Task", 8 * 1024, (void*)afe_data, 5, &g_sr_data->detect_task, 1);
    ESP_GOTO_ON_FALSE(pdPASS == ret_val, ESP_FAIL, err, TAG,  "Failed create audio detect task");

    ret_val = xTaskCreatePinnedToCore(&sr_handler_task, "SR Handler Task", 6 * 1024, NULL, configMAX_PRIORITIES - 1, &g_sr_data->handle_task, 0);
    ESP_GOTO_ON_FALSE(pdPASS == ret_val, ESP_FAIL, err, TAG,  "Failed create audio handler task");

    return ESP_OK;
err:
    app_sr_stop();
    return ret;
}

esp_err_t app_sr_stop(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");

    /**
     * Waiting for all task stoped
     * TODO: A task creation failure cannot be handled correctly now
     * */
    xEventGroupSetBits(g_sr_data->event_group, NEED_DELETE);
    xEventGroupWaitBits(g_sr_data->event_group, NEED_DELETE | FEED_DELETED | DETECT_DELETED, 1, 1, portMAX_DELAY);

    if (g_sr_data->result_que) {
        vQueueDelete(g_sr_data->result_que);
        g_sr_data->result_que = NULL;
    }

    if (g_sr_data->event_group) {
        vEventGroupDelete(g_sr_data->event_group);
        g_sr_data->event_group = NULL;
    }

    if (g_sr_data->fp) {
        fclose(g_sr_data->fp);
        g_sr_data->fp = NULL;
    }

    if (g_sr_data->model_data) {
        g_sr_data->multinet->destroy(g_sr_data->model_data);
    }

    if (g_sr_data->afe_data) {
        g_sr_data->afe_handle->destroy(g_sr_data->afe_data);
    }

    sr_cmd_t *it;
    while (!SLIST_EMPTY(&g_sr_data->cmd_list)) {
        it = SLIST_FIRST(&g_sr_data->cmd_list);
        SLIST_REMOVE_HEAD(&g_sr_data->cmd_list, next);
        heap_caps_free(it);
    }

    if (g_sr_data->afe_in_buffer) {
        heap_caps_free(g_sr_data->afe_in_buffer);
    }

    if (g_sr_data->afe_out_buffer) {
        heap_caps_free(g_sr_data->afe_out_buffer);
    }

    heap_caps_free(g_sr_data);
    g_sr_data = NULL;
    return ESP_OK;
}

esp_err_t app_sr_get_result(sr_result_t *result, TickType_t xTicksToWait)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");

    xQueueReceive(g_sr_data->result_que, result, xTicksToWait);
    return ESP_OK;
}

esp_err_t app_sr_add_cmd(const sr_cmd_t *cmd)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    ESP_RETURN_ON_FALSE(NULL != cmd, ESP_ERR_INVALID_ARG, TAG, "pointer of cmd is invaild");
    ESP_RETURN_ON_FALSE(cmd->lang == g_sr_data->lang, ESP_ERR_INVALID_ARG, TAG, "cmd lang error");
    ESP_RETURN_ON_FALSE(ESP_MN_MAX_PHRASE_NUM >= g_sr_data->cmd_num, ESP_ERR_INVALID_STATE, TAG, "cmd is full");

    sr_cmd_t *item = (sr_cmd_t *)heap_caps_calloc(1, sizeof(sr_cmd_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(NULL != item, ESP_ERR_NO_MEM, TAG, "memory for sr cmd is not enough");
    memcpy(item, cmd, sizeof(sr_cmd_t));
    item->next.sle_next = NULL;
#if 1 // insert after
    sr_cmd_t *last = SLIST_FIRST(&g_sr_data->cmd_list);
    if (last == NULL) {
        SLIST_INSERT_HEAD(&g_sr_data->cmd_list, item, next);
    } else {
        sr_cmd_t *it;
        while ((it = SLIST_NEXT(last, next)) != NULL) {
            last = it;
        }
        SLIST_INSERT_AFTER(last, item, next);
    }
#else  // insert head
    SLIST_INSERT_HEAD(&g_sr_data->cmd_list, it, next);
#endif
    esp_mn_commands_add(g_sr_data->cmd_num, (char *)cmd->phoneme);
    g_sr_data->cmd_num++;
    return ESP_OK;
}

esp_err_t app_sr_modify_cmd(uint32_t id, const sr_cmd_t *cmd)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    ESP_RETURN_ON_FALSE(NULL != cmd, ESP_ERR_INVALID_ARG, TAG, "pointer of cmd is invaild");
    ESP_RETURN_ON_FALSE(id < g_sr_data->cmd_num, ESP_ERR_INVALID_ARG, TAG, "cmd id out of range");
    ESP_RETURN_ON_FALSE(cmd->lang == g_sr_data->lang, ESP_ERR_INVALID_ARG, TAG, "cmd lang error");

    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (it->id == id) {
            ESP_LOGI(TAG, "modify cmd [%d] from %s to %s", id, it->str, cmd->str);
            esp_mn_commands_modify(it->phoneme, (char *)cmd->phoneme);
            memcpy(it, cmd, sizeof(sr_cmd_t));
            break;
        }
    }
    ESP_RETURN_ON_FALSE(NULL != it, ESP_ERR_NOT_FOUND, TAG, "can't find cmd id:%d", cmd->id);
    return ESP_OK;
}

esp_err_t app_sr_remove_cmd(uint32_t id)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    ESP_RETURN_ON_FALSE(id < g_sr_data->cmd_num, ESP_ERR_INVALID_ARG, TAG, "cmd id out of range");
    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (it->id == id) {
            ESP_LOGI(TAG, "remove cmd id [%d]", it->id);
            SLIST_REMOVE(&g_sr_data->cmd_list, it, sr_cmd_t, next);
            heap_caps_free(it);
            g_sr_data->cmd_num--;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(NULL != it, ESP_ERR_NOT_FOUND, TAG, "can't find cmd id:%d", id);
    return ESP_OK;
}

esp_err_t app_sr_remove_all_cmd(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    sr_cmd_t *it;
    while (!SLIST_EMPTY(&g_sr_data->cmd_list)) {
        it = SLIST_FIRST(&g_sr_data->cmd_list);
        SLIST_REMOVE_HEAD(&g_sr_data->cmd_list, next);
        heap_caps_free(it);
    }
    SLIST_INIT(&g_sr_data->cmd_list);
    g_sr_data->cmd_num = 0;
    return ESP_OK;
}

esp_err_t app_sr_update_cmds(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");

    uint32_t count = 0;
    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        it->id = count++;
    }

    esp_mn_error_t *err_id = esp_mn_commands_update(g_sr_data->multinet, g_sr_data->model_data);
    if(err_id){
        for (int i = 0; i < err_id->num; i++) {
            ESP_LOGE(TAG, "err cmd id:%d", err_id->phrase_idx[i]);
        }
    }
    esp_mn_commands_print();

    return ESP_OK;
}

uint8_t app_sr_search_cmd_from_user_cmd(sr_user_cmd_t user_cmd, uint8_t *id_list, uint16_t max_len)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, 0, TAG, "SR is not running");

    uint8_t cmd_num = 0;
    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (user_cmd == it->cmd) {
            if (id_list) {
                id_list[cmd_num] = it->id;
            }
            if (++cmd_num >= max_len) {
                break;
            }
        }
    }
    return cmd_num;
}

uint8_t app_sr_search_cmd_from_phoneme(const char *phoneme, uint8_t *id_list, uint16_t max_len)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, 0, TAG, "SR is not running");

    uint8_t cmd_num = 0;
    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (0 == strcmp(phoneme, it->phoneme)) {
            if (id_list) {
                id_list[cmd_num] = it->id;
            }
            if (++cmd_num >= max_len) {
                break;
            }
        }
    }
    return cmd_num;
}

bool app_sr_is_phoneme_exists(const char *phoneme)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, false, TAG, "SR is not running");

    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (0 == strcmp(phoneme, it->phoneme)) {
            return true;
        }
    }
    return false;
}

sr_cmd_t *app_sr_get_cmd_from_id(uint32_t id)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, NULL, TAG, "SR is not running");
    ESP_RETURN_ON_FALSE(id < g_sr_data->cmd_num, NULL, TAG, "cmd id out of range");

    sr_cmd_t *it;
    SLIST_FOREACH(it, &g_sr_data->cmd_list, next) {
        if (id == it->id) {
            return it;
        }
    }
    ESP_RETURN_ON_FALSE(NULL != it, NULL, TAG, "can't find cmd id:%d", id);
    return NULL;
}