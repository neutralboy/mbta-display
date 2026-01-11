#include "Wireless.h"
#include "config.h"

#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_netif.h"

#define WIFI_CONNECT_SSID WIFI_SSID
#define WIFI_CONNECT_PASS WIFI_PASS

#define WIFI_MAXIMUM_RETRY 5

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static esp_netif_t *s_wifi_sta_netif;

static volatile wireless_status_t s_status = WIRELESS_STATUS_CONNECTING;
static char s_ip_str[16] = {0};

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

wireless_status_t Wireless_GetStatus(void)
{
    return s_status;
}

const char *Wireless_GetIpStr(void)
{
    return s_ip_str;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_status = WIRELESS_STATUS_CONNECTING;
        if (s_wifi_retry_num < WIFI_MAXIMUM_RETRY) {
            s_wifi_retry_num++;
            esp_wifi_connect();
        } else {
            s_status = WIRELESS_STATUS_FAILED;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_num = 0;

        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_status = WIRELESS_STATUS_CONNECTED;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        return;
    }
}

uint16_t BLE_NUM = 0;
uint16_t WIFI_NUM = 0;
bool Scan_finish = 0;

static void WIFI_Init(void *arg);

void Wireless_Init(void)
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    // BLE not used
    BLE_NUM = 0;

    // Wi-Fi connect task
    xTaskCreatePinnedToCore(
        WIFI_Init, 
        "WIFI task",
        8192, 
        NULL, 
        1, 
        NULL, 
        0);
}

static void WIFI_Init(void *arg)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    if (s_wifi_sta_netif == NULL) {
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_CONNECT_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_CONNECT_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    s_status = WIRELESS_STATUS_CONNECTING;
    s_wifi_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Must set STA config before starting/connecting, otherwise ESP_ERR_WIFI_STATE can happen.
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        WIFI_NUM = 1;
        Scan_finish = 1;
    } else {
        WIFI_NUM = 0;
        Scan_finish = 0;
        s_status = WIRELESS_STATUS_FAILED;
    }
    
    vTaskDelete(NULL);
}