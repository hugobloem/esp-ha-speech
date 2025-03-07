/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp_storage.h"
#include "settings.h"
#include "app_led.h"
#include "app_sr.h"
#include "app_wifi.h"
#include "audio_player.h"
#include "file_iterator.h"
#include "gui/ui_main.h"

#include "bsp_board.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "main";

file_iterator_instance_t *file_iterator;

#define MEMORY_MONITOR 0

#if MEMORY_MONITOR
static void monitor_task(void *arg)
{
    (void) arg;
    const int STATS_TICKS = pdMS_TO_TICKS(2 * 1000);

    while (true) {
        ESP_LOGI(TAG, "System Info Trace");
        printf("\tDescription\tInternal\tSPIRAM\n");
        printf("Current Free Memory\t%d\t\t%d\n",
               heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
               heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        printf("Largest Free Block\t%d\t\t%d\n",
               heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
               heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        printf("Min. Ever Free Size\t%d\t\t%d\n",
               heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
               heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

        vTaskDelay(STATS_TICKS);
    }

    vTaskDelete(NULL);
}

static void sys_monitor_start(void)
{
    BaseType_t ret_val = xTaskCreatePinnedToCore(monitor_task, "Monitor Task", 4 * 1024, NULL, configMAX_PRIORITIES - 3, NULL, 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT((pdPASS == ret_val) ? ESP_OK : ESP_FAIL);
}
#endif

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.
    static int last_volume;
    bsp_codec_config_t *codec_handle = bsp_board_get_codec_handle();

    sys_param_t *param = settings_get_parameter();
    if (param->volume != 0) {
        last_volume = param->volume;
    }

    codec_handle->mute_set_fn(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE) {
        codec_handle->volume_set_fn(param->volume, NULL);
    }

    ESP_LOGI(TAG, "mute setting %d, volume:%d", setting, last_volume);

    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Compile time: %s %s", __DATE__, __TIME__);
    /* Initialize NVS. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(settings_read_parameter_from_nvs());
#if !SR_RUN_TEST && MEMORY_MONITOR
    sys_monitor_start(); // Logs should be reduced during SR testing
#endif
    bsp_spiffs_mount();

    bsp_i2c_init();
    bsp_display_start();
    bsp_board_init();

    sys_param_t *param = settings_get_parameter();
    ESP_LOGI(TAG, "Display LVGL demo");
    bsp_display_backlight_on();
    bsp_display_brightness_set(param->brightness);
    ESP_ERROR_CHECK(ui_main_start());

    bsp_codec_config_t *codec_handle = bsp_board_get_codec_handle();
    file_iterator = file_iterator_new("/spiffs/mp3");
    assert(file_iterator != NULL);
    audio_player_config_t config = { .mute_fn = audio_mute_function,
                                     .write_fn = codec_handle->i2s_write_fn,
                                     .clk_set_fn = codec_handle->i2s_reconfig_clk_fn,
                                     .priority = 5
                                   };
    ESP_ERROR_CHECK(audio_player_new(config));

    const board_res_desc_t *brd = bsp_board_get_description();
    app_pwm_led_init(brd->PMOD2->row1[1], brd->PMOD2->row1[2], brd->PMOD2->row1[3]);
    ESP_LOGI(TAG, "speech recognition start");
    app_sr_start(false);

    /* Initialize Wi-Fi. */
    app_wifi_init();
    err = app_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Wifi");
    }
}
