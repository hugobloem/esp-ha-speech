#include <string.h>
#include "esp_system.h"
#include "esp_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "app_api_mqtt.h"
#include "app_hass.h"
#include "ui_net_config.h"
#include "secrets.h"

static const char *TAG = "app_api_mqtt";
static esp_mqtt_client_handle_t client = NULL;
static bool mqtt_connected = false;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}


esp_err_t data_handler(char *topic_, char *data, int topic_len, int data_len)
{
    // Limit topic and data to 64 and 128 chars max
    char topic[512];
    strncpy(topic, topic_, topic_len);
    topic[topic_len] = '\0';

    cJSON *jData = cJSON_ParseWithLength(data, data_len);
    if (jData == NULL) {
        ESP_LOGE(TAG, "Error parsing json");
        cJSON_Delete(jData);
        return ESP_FAIL;
    }

    // TODO: change to strtok
    // topic starts with "hermes/"
    if (strncmp(topic, "hermes/", 7) == 0) {
        // Handle hermes messages
        ESP_LOGI(TAG, "hermes message: %s", jData);
        // app_hass_handle_hermes(topic, jData);


    } else if (strncmp(topic, "esp-ha-speech/", 7) == 0) {
        // Handle esp-ha messages
        ESP_LOGI(TAG, "esp-ha message: %s", jData);

        if (strncmp(topic, "esp-ha/config/add_cmd", 21) == 0) {
            // Handle config messages
            ESP_LOGI(TAG, "adding command");
            app_hass_add_cmd_from_msg(jData);

        } else if (strncmp(topic, "esp-ha/config/rm_all", 24) == 0) {
            // Handle config messages
            ESP_LOGI(TAG, "removing all commands");
            app_hass_rm_all_cmd(jData);
        }
    }
    return ESP_OK;
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        ui_net_config_update_cb(UI_NET_EVT_CLOUD_CONNECTED, NULL);
        // Set connected flag
        mqtt_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        ui_net_config_update_cb(UI_NET_EVT_WIFI_CONNECTED, NULL);
        // Set connected flag
        mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        // handle data
        data_handler(event->topic, event->data, event->topic_len, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "MQTT_EVENT_BEFORE_CONNECT");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}


// async connect to mqtt server
esp_err_t mqtt_connect(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URL,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    // Wait for connection
    while (!mqtt_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}

/* start mqtt client */
void app_api_mqtt_start(void)
{
    ESP_ERROR_CHECK(mqtt_connect());

    // Subscribe to hermes and configuration topics
    esp_mqtt_client_subscribe(client, "hermes/#", 0);
    esp_mqtt_client_subscribe(client, "esp-ha/#", 0);

}

/* send commands to mqtt */
void app_api_mqtt_send_cmd(char *topic, char *cmd)
{
    char *payload = malloc(strlen(cmd) + 100);
    sprintf(payload, "{\"input\": \"%s\", \"siteId\": \"%s\"}", cmd, MQTT_SITE_ID);

    esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
}