#include "esp_log.h"
#include "bsp_board.h"
#include "lvgl.h"
#include "ui_main.h"
#include "ui_helpers.h"
#include "ui_settings_config.h"
#include "settings.h"

static const char *TAG = "ui_settings_config";

const char *sys_param_strs [] = {
    "Brightness",
    "Volume",
    "Wifi ssid",
    "Wifi password",
};

//////// Pages ////////
static lv_obj_t *g_menu_page;
static lv_obj_t *g_setting_select_page = NULL;
static lv_obj_t *g_setting_change_page = NULL;

//////// Widgets ////////
lv_obj_t *ssp_roller = NULL;
lv_obj_t *scp_keyboard = NULL;
lv_obj_t *scp_textarea = NULL;
lv_obj_t *scp_slider = NULL;

static void (*g_settings_config_end_cb)(void) = NULL;

static void ui_app_page_return_click_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_user_data(e);
    if (ui_get_btn_op_group()) {
        lv_group_focus_freeze(ui_get_btn_op_group(), false);
    }
#if CONFIG_BSP_BOARD_ESP32_S3_BOX
    bsp_btn_rm_all_callback(BOARD_BTN_ID_HOME);
#endif
    lv_obj_del(obj);
    if (g_settings_config_end_cb) {
        g_settings_config_end_cb();
    }
}

static void btn_return_down_cb(void *handle, void *arg)
{   
    ESP_LOGI(TAG, "btn_return_down_cb");
    lv_obj_t *obj = (lv_obj_t *)arg;
    ui_acquire();
    g_setting_change_page = NULL;
    g_setting_select_page = NULL;
    _ui_screen_change(g_menu_page, LV_SCR_LOAD_ANIM_MOVE_TOP, 500, 0);
    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
    ui_main_menu(1);
    bsp_btn_rm_all_callback(BOARD_BTN_ID_HOME);
    ui_release();
}

void setting_select_button_cb(lv_event_t *e) 
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);
    

    if (event_code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "setting_select_button_cb");
        uint16_t id = lv_roller_get_selected(ssp_roller);
        ESP_LOGI(TAG, "selected id: %d: %s", id, sys_param_strs[id]);
        sys_param_t *sys_param = settings_get_parameter();

        switch (id) {
            case 0:
                // need hint
                break;
            case 1:
                // volume
                ESP_LOGI(TAG, "volume: %d", sys_param->volume);
                change_number_setting_page(sys_param->volume, 0, 100);
                break;
            case 2:
                // wifi ssid
                ESP_LOGI(TAG, "wifi ssid: %s", sys_param->wifi_ssid);
                change_text_setting_page(sys_param->wifi_ssid);
                break;
            case 3:
                // wifi password
                ESP_LOGI(TAG, "wifi password: %s", sys_param->wifi_password);
                change_text_setting_page(sys_param->wifi_password);
                break;
            default:
                break;
        }

        _ui_screen_change(g_setting_change_page, LV_SCR_LOAD_ANIM_MOVE_LEFT, ANIM_SPD, ANIM_DLY);
    }
}

void setting_change_button_cb(lv_event_t *e) 
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);
    if (event_code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "setting_change_button_cb");
        
        uint16_t id = lv_roller_get_selected(ssp_roller);
        sys_param_t *sys_param = settings_get_parameter();
        switch (id) {
            case 0:
                // need hint
                break;
            case 1:
                // volume
                uint8_t volume = lv_slider_get_value(scp_slider);
                sys_param->volume = volume;
                break;
            case 2:
                // wifi ssid
                char *ssid = lv_textarea_get_text(scp_textarea);
                strcpy(sys_param->wifi_ssid, ssid);
                ESP_LOGI(TAG, "New wifi ssid: %s", sys_param->wifi_ssid);
                break;
            case 3:
                // wifi password
                char *password = lv_textarea_get_text(scp_textarea);
                strcpy(sys_param->wifi_password, password);
                ESP_LOGI(TAG, "New wifi password: %s", sys_param->wifi_password);
                break;
            default:
                break;
        }
        settings_write_parameter_to_nvs();
        _ui_screen_change(g_setting_select_page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, ANIM_SPD, ANIM_DLY);
    }
}

static void setting_change_textarea_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        /*Focus on the clicked text area*/
        if(scp_keyboard != NULL) lv_keyboard_set_textarea(scp_keyboard, ta);
    }

    else if(code == LV_EVENT_READY) {
        LV_LOG_USER("Ready, current text: %s", lv_textarea_get_text(ta));
    }
}

void select_setting_page_init(void)
{
    // Get all settings
    char roller_options [100] = "";
    char *sep = "\n";
    size_t num_settings = sizeof(sys_param_strs) / sizeof(sys_param_strs[0]);
    // Loop over the list of strings
    for (int i = 0; i < num_settings; i++) {
        // Append the current element to the destination string
        strcat (roller_options, sys_param_strs [i]);
        // Append the separator string if not the last element
        if (i < num_settings - 1) {
            strcat (roller_options, sep);
        }
    }

    // Settings select page: ssp
    g_setting_select_page = lv_obj_create(NULL);
    lv_obj_clear_flag(g_setting_select_page, LV_OBJ_FLAG_SCROLLABLE);    /// Flags

    lv_obj_t *ssp_heading = lv_label_create(g_setting_select_page);
    lv_obj_set_width( ssp_heading, lv_pct(100));
    lv_obj_set_height( ssp_heading, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( ssp_heading, LV_ALIGN_TOP_MID );
    lv_label_set_text(ssp_heading,"SETTINGS");
    lv_obj_set_style_text_color(ssp_heading, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_text_opa(ssp_heading, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ssp_heading, 7, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ssp_heading, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ssp_heading, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ssp_heading, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(ssp_heading, 255, LV_PART_MAIN| LV_STATE_DEFAULT);

    ssp_roller = lv_roller_create(g_setting_select_page);
    lv_roller_set_options( ssp_roller, roller_options, LV_ROLLER_MODE_NORMAL );
    lv_obj_set_width( ssp_roller, lv_pct(90));
    lv_obj_set_height( ssp_roller, lv_pct(60));
    lv_obj_set_x( ssp_roller, 0 );
    lv_obj_set_y( ssp_roller, lv_pct(10) );
    lv_obj_set_align( ssp_roller, LV_ALIGN_TOP_MID );

    lv_obj_t *ssp_button = lv_btn_create(g_setting_select_page);
    lv_obj_set_width( ssp_button, 100);
    lv_obj_set_height( ssp_button, 50);
    lv_obj_set_x( ssp_button, 1 );
    lv_obj_set_y( ssp_button, lv_pct(-5) );
    lv_obj_set_align( ssp_button, LV_ALIGN_BOTTOM_MID );
    lv_obj_add_flag( ssp_button, LV_OBJ_FLAG_SCROLL_ON_FOCUS );
    lv_obj_clear_flag( ssp_button, LV_OBJ_FLAG_SCROLLABLE );

    lv_obj_t *ssp_button_label = lv_label_create(ssp_button);
    lv_obj_set_width( ssp_button_label, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( ssp_button_label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( ssp_button_label, LV_ALIGN_CENTER );
    lv_label_set_text(ssp_button_label, "Select");

    lv_obj_add_event_cb(ssp_button, setting_select_button_cb, LV_EVENT_ALL, NULL);
}

void change_text_setting_page(char *setting)
{
    // Settings change page: scp
    g_setting_change_page = lv_obj_create(NULL);
    lv_obj_clear_flag(g_setting_change_page, LV_OBJ_FLAG_SCROLLABLE);    /// Flags

    lv_obj_t *scp_heading = lv_label_create(g_setting_change_page);
    lv_obj_set_width( scp_heading, lv_pct(100));
    lv_obj_set_height( scp_heading, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( scp_heading, LV_ALIGN_TOP_MID );
    lv_label_set_text(scp_heading,"SETTINGS");
    lv_obj_set_style_text_color(scp_heading, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_text_opa(scp_heading, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(scp_heading, 7, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(scp_heading, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(scp_heading, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scp_heading, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(scp_heading, 255, LV_PART_MAIN| LV_STATE_DEFAULT);

    scp_keyboard = lv_keyboard_create(g_setting_change_page);
    lv_obj_set_width( scp_keyboard, 300);
    lv_obj_set_height( scp_keyboard, 120);
    lv_obj_set_x( scp_keyboard, 0 );
    lv_obj_set_y( scp_keyboard, lv_pct(-5) );
    lv_obj_set_align( scp_keyboard, LV_ALIGN_BOTTOM_MID );

    scp_textarea = lv_textarea_create(g_setting_change_page);
    lv_obj_set_height( scp_textarea, 70);
    lv_obj_set_width( scp_textarea, lv_pct(70));
    lv_obj_set_x( scp_textarea, lv_pct(4) );
    lv_obj_set_y( scp_textarea, lv_pct(10) );
    lv_textarea_set_text(scp_textarea, setting);
    lv_obj_add_event_cb(scp_textarea, setting_change_textarea_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *scp_button = lv_btn_create(g_setting_change_page);
    lv_obj_set_height( scp_button, 70);
    lv_obj_set_width( scp_button, lv_pct(20));
    lv_obj_set_x( scp_button, lv_pct(-4) );
    lv_obj_set_y( scp_button, lv_pct(10) );
    lv_obj_set_align( scp_button, LV_ALIGN_TOP_RIGHT );
    lv_obj_add_flag( scp_button, LV_OBJ_FLAG_SCROLL_ON_FOCUS );   /// Flags
    lv_obj_clear_flag( scp_button, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

    lv_obj_t *scp_button_label = lv_label_create(scp_button);
    lv_obj_set_width( scp_button_label, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( scp_button_label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( scp_button_label, LV_ALIGN_CENTER );
    lv_label_set_text(scp_button_label,"Done");

    lv_obj_add_event_cb(scp_button, setting_change_button_cb, LV_EVENT_ALL, NULL);

}

void change_number_setting_page(int value, int min, int max)
{
    g_setting_change_page = lv_obj_create(NULL);
    lv_obj_clear_flag( g_setting_change_page, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

    lv_obj_t *scp_heading = lv_label_create(g_setting_change_page);
    lv_obj_set_width( scp_heading, lv_pct(100));
    lv_obj_set_height( scp_heading, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( scp_heading, LV_ALIGN_TOP_MID );
    lv_label_set_text(scp_heading,"SETTINGS");
    lv_obj_set_style_text_color(scp_heading, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_text_opa(scp_heading, 255, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(scp_heading, 7, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(scp_heading, 0, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(scp_heading, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(scp_heading, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT );
    lv_obj_set_style_bg_opa(scp_heading, 255, LV_PART_MAIN| LV_STATE_DEFAULT);

    scp_slider = lv_slider_create(g_setting_change_page);
    lv_obj_set_width( scp_slider, 280);
    lv_obj_set_height( scp_slider, 20);
    lv_obj_set_align( scp_slider, LV_ALIGN_CENTER );
    lv_slider_set_range(scp_slider, min, max);
    lv_slider_set_value(scp_slider, value, LV_ANIM_OFF);

    lv_obj_t *scp_button = lv_btn_create(g_setting_change_page);
    lv_obj_set_width( scp_button, 100);
    lv_obj_set_height( scp_button, 50);
    lv_obj_set_x( scp_button, 1 );
    lv_obj_set_y( scp_button, lv_pct(-5) );
    lv_obj_set_align( scp_button, LV_ALIGN_BOTTOM_MID );
    lv_obj_add_flag( scp_button, LV_OBJ_FLAG_SCROLL_ON_FOCUS );   /// Flags
    lv_obj_clear_flag( scp_button, LV_OBJ_FLAG_SCROLLABLE );    /// Flags

    lv_obj_t *scp_button_label = lv_label_create(scp_button);
    lv_obj_set_width( scp_button_label, LV_SIZE_CONTENT);  /// 1
    lv_obj_set_height( scp_button_label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_align( scp_button_label, LV_ALIGN_CENTER );
    lv_label_set_text(scp_button_label,"Done");

    lv_obj_add_event_cb(scp_button, setting_change_button_cb, LV_EVENT_ALL, NULL);
}

void ui_settings_config_start(void (*fn)(void))
{
    g_settings_config_end_cb = fn;
    g_menu_page = lv_scr_act();
    select_setting_page_init();
    bsp_btn_register_callback(BOARD_BTN_ID_HOME, BUTTON_PRESS_UP, btn_return_down_cb, NULL);
    _ui_screen_change(g_setting_select_page, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, ANIM_SPD, ANIM_DLY);
}