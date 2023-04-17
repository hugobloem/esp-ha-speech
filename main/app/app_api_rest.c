#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_http_client.h"

#include "cJSON.h"

// #include "app_hass.h"
#include "app_api_rest.h"
#include "ui_net_config.h"


#ifndef MAX_HTTP_RECV_BUFFER
#define MAX_HTTP_RECV_BUFFER 512
#endif
#ifndef MAX_HTTP_OUTPUT_BUFFER
#define MAX_HTTP_OUTPUT_BUFFER 2048
#endif
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#define HTTP_OK 200

#include "secrets.h"

#define CONV_API_PATH "/api/conversation/process"

static const char *TAG = "app_api_rest";

/* Event Handler */
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                int copy_len = 0;
                if (evt->user_data) {
                    copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
                    if (copy_len) {
                        memcpy(evt->user_data + output_len, evt->data, copy_len);
                    }
                } else {
                    const int buffer_len = esp_http_client_get_content_length(evt->client);
                    if (output_buffer == NULL) {
                        output_buffer = (char *) malloc(buffer_len);
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    copy_len = MIN(evt->data_len, (buffer_len - output_len));
                    if (copy_len) {
                        memcpy(output_buffer + output_len, evt->data, copy_len);
                    }
                }
                output_len += copy_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_header(evt->client, "From", "user@example.com");
            esp_http_client_set_header(evt->client, "Accept", "text/html");
            esp_http_client_set_redirection(evt->client);
            break;
    }
    return ESP_OK;
}


void app_api_rest_get(char* path, char* response_buffer) {
    esp_http_client_config_t config = {
        .host = HASS_URL,
        .port = HASS_PORT,
        .path = path,
        .method = HTTP_METHOD_GET,
        .event_handler = _http_event_handler,
        .user_data = response_buffer,       // Pass address of local buffer to get response
        .timeout_ms = 3000,
        .buffer_size = MAX_HTTP_OUTPUT_BUFFER,
        .is_async = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Add headers
    esp_http_client_set_header(client, "Authorization", "Bearer " HASS_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Get
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d"PRIu64,
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "HTTP GET at %s Yielded: %s", path, response_buffer);
    esp_http_client_cleanup(client);
}

void app_api_rest_post(char* path, char* response_buffer, char* message) {
    esp_http_client_config_t config = {
        .host = HASS_URL,
        .port = HASS_PORT,
        .path = path,
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
        .user_data = response_buffer,       // Pass address of local buffer to get response
        .timeout_ms = 3000,
        .buffer_size = MAX_HTTP_OUTPUT_BUFFER,
        .is_async = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Add headers
    esp_http_client_set_header(client, "Authorization", "Bearer " HASS_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_http_client_set_post_field(client, message, strlen(message));

    // Post
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d"PRIu64,
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "HTTP POST at %s with data %s, Yielded: %s", path, message, response_buffer);
    esp_http_client_cleanup(client);
}

void app_api_rest_test(void *pvParameters) {

    char response[MAX_HTTP_OUTPUT_BUFFER] = {0};
    app_api_rest_get("/api/", response);

    if (strcmp(response, "{\"message\":\"API running\"}")) {
        ui_net_config_update_cb(UI_NET_EVT_CLOUD_CONNECTED, NULL);
    } else {
        ui_net_config_update_cb(UI_NET_EVT_WIFI_CONNECTED, NULL);
    }

    ESP_LOGI(TAG, "Connected");

    vTaskDelete(NULL);
}

/* send recognised command string to home assistant */
void app_api_rest_send_recognised_cmd(char *cmd_str) {
    char response[MAX_HTTP_OUTPUT_BUFFER] = {0};
    char *message = malloc(strlen(cmd_str) + 100);
    sprintf(message, "{\"text\": \"%s\"}", cmd_str);
    app_api_rest_post("/api/conversation/process", response, message);
}
