#include "secrets.h"

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "app_wifi.h"
#include "app_sntp.h"
#include "app_hass.h"
#include "ui_main.h"
#include "ui_net_config.h"

static bool s_connected = false;
static char s_payload[150] = "";
static const char *TAG = "app_wifi";
static const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT_SOFTAP "softap"
#define QRCODE_BASE_URL "https://www.espressif.github.io/esp-jumpstart/qrcode.html"

/* Event handler for catching system events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  if (event_base == WIFI_PROV_EVENT)
  {
    switch (event_id)
    {
    case WIFI_PROV_START:
    {
      ESP_LOGI(TAG, "Provisioning started");
      break;
    }
    case WIFI_PROV_CRED_RECV:
    {
      wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
      ESP_LOGI(TAG, "Received Wi-Fi credentials"
                    "\n\tSSID: %s\n\tPassword: %s",
               (const char *)wifi_sta_cfg->ssid,
               (const char *)wifi_sta_cfg->password);
      break;
    }
    case WIFI_PROV_CRED_FAIL:
    {
      wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
      ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s",
               "\n\tPlease reset to factory and retry provisioning",
               (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
      break;
    }
    case WIFI_PROV_CRED_SUCCESS:
    {
      ESP_LOGI(TAG, "Provisioning successful");
      break;
    }
    case WIFI_PROV_END:
    {
      /* De-initialize manager once provisioning is finished */
      wifi_prov_mgr_deinit();
      break;
    }
    default:
      break;
    }
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    ui_net_config_update_cb(UI_NET_EVT_START_CONNECT, NULL);
    switch (ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect()))
    {
    case ESP_ERR_WIFI_NOT_INIT:
      ESP_LOGI(TAG, "WiFi Connect Failed: ESP_ERR_WIFI_NOT_INIT");
      break;
    case ESP_ERR_WIFI_NOT_STARTED:
      ESP_LOGI(TAG, "WiFi Connect Failed: ESP_ERR_WIFI_NOT_STARTED");
      break;
    case ESP_ERR_WIFI_CONN:
      ESP_LOGI(TAG, "WiFi Connect Failed: ESP_ERR_WIFI_CONN");
      break;
    case ESP_ERR_WIFI_SSID:
      ESP_LOGI(TAG, "WiFi Connect Failed: ESP_ERR_WIFI_SSID");
      break;
    }
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_WIFI_READY)
  {
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
    s_connected = 1;
    ui_acquire();
    ui_main_status_bar_set_wifi(s_connected);
    ui_release();
    /* Signal main application to continue execution */
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
    esp_wifi_connect();
    s_connected = 0;
    ui_acquire();
    ui_main_status_bar_set_wifi(s_connected);
    ui_release();
  }
}

static void wifi_init_sta(void)
{
  /* start wifi station mode */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  wifi_config_t wifi_config = {
      .sta = {
          .ssid = WIFI_SSID,
          .password = WIFI_PASS},
  };
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max)
{
  uint8_t eth_mac[6];
  const char *ssid_prefix = "ESP_";
  esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
  snprintf(service_name, max, "%s%02X%02X%02X", ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                   uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
  if (inbuf)
  {
    ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);
  }
  char response[] = "SUCCESS";
  *outbuf = (uint8_t *)strdup(response);
  if (*outbuf == NULL)
  {
    ESP_LOGE(TAG, "System out of memory");
    return ESP_ERR_NO_MEM;
  }
  *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

  return ESP_OK;
}

static void wifi_prov_print_qr(const char *name, const char *username, const char *pop, const char *transport)
{
  if (!name || !transport)
  {
    ESP_LOGW(TAG, "Cannot generate QR code payload. Data missing.");
    return;
  }
  char payload[150] = {0};
  if (pop)
  {
  }
  else
  {
    snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\""
                                       ",\"transport\":\"%s\"}",
             PROV_QR_VERSION, name, transport);
  }
  ESP_LOGI(TAG, "If QR code is not visible, copy paste the below URL in a browser.\n%s?data=%s", QRCODE_BASE_URL, payload);
}

char *app_wifi_get_prov_payload(void)
{
  return s_payload;
}

/* Initialise WiFi */
void app_wifi_init(void)
{

  /* Initialize TCP/IP */
  ESP_ERROR_CHECK(esp_netif_init());

  /* Initialize the event loop */
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_event_group = xEventGroupCreate();

  /* Register our event handler for Wi-Fi, IP and Provisioning related events */
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

  /* Initialize Wi-Fi including netif with default config */
  esp_netif_create_default_wifi_sta();
  // esp_netif_create_default_wifi_ap();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  wifi_init_sta();
}

esp_err_t app_wifi_start(void)
{
  ui_net_config_update_cb(UI_NET_EVT_START, NULL);
  /* Configuration for the provisioning manager */
  wifi_prov_mgr_config_t config = {
      /* What is the Provisioning Scheme that we want ?
       * wifi_prov_scheme_softap or wifi_prov_scheme_ble */
      .scheme = wifi_prov_scheme_softap,
      .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE};

  /* Initialize provisioning manager with the
   * configuration parameters set above */
  ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

  bool provisioned = false;
  /* Let's find out if the device is provisioned */
  ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

  /* If device is not yet provisioned start provisioning service */
  if (!provisioned)
  {
    ESP_LOGI(TAG, "Starting provisioning");
    ui_net_config_update_cb(UI_NET_EVT_START_PROV, NULL);

    /* Wi-Fi SSID when scheme is wifi_prov_scheme_softap */
    char service_name[12];
    get_device_service_name(service_name, sizeof(service_name));

    const char *service_key = NULL;

    /* An optional endpoint that applications can create if they expect to
     * get some additional custom data during provisioning workflow.
     * The endpoint name can be anything of your choice.
     * This call must be made before starting the provisioning.
     */
    wifi_prov_mgr_endpoint_create("custom-data");

    /* Start provisioning service */
    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, (const void *)NULL, service_name, service_key));

    /* The handler for the optional endpoint created above.
     * This call must be made after starting the provisioning, and only if the endpoint
     * has already been created above.
     */
    wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);

    /* Uncomment the following to wait for the provisioning to finish and then release
     * the resources of the manager. Since in this case de-initialization is triggered
     * by the default event loop handler, we don't need to call the following */
    // wifi_prov_mgr_wait();
    // wifi_prov_mgr_deinit();
    /* Print QR code for provisioning */
    wifi_prov_print_qr(service_name, NULL, NULL, "softap");
    ui_net_config_update_cb(UI_NET_EVT_GET_NAME, NULL);
  }
  else
  {
    ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

    /* We don't need the manager as device is already provisioned,
     * so let's release it's resources */
    wifi_prov_mgr_deinit();

    /* Start Wi-Fi station */
    wifi_init_sta();
  }

  /* Wait for Wi-Fi connection */
  xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, true, true, portMAX_DELAY);
  ui_net_config_update_cb(UI_NET_EVT_WIFI_CONNECTED, NULL);
  /* Sync time */
  app_sntp_init();

  app_hass_init();

  return ESP_OK;
}

bool app_wifi_is_connected(void)
{
  return s_connected;
}

esp_err_t app_wifi_get_wifi_ssid(char *ssid, size_t len)
{
  wifi_config_t wifi_cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK)
  {
    return ESP_FAIL;
  }
  strncpy(ssid, (const char *)wifi_cfg.sta.ssid, len);
  return ESP_OK;
}