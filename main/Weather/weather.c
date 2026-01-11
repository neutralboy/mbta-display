#include "weather.h"
#include "config.h"

#include "Wireless.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include "cJSON.h"

#ifndef WEATHER_FETCH_PERIOD_MS
#define WEATHER_FETCH_PERIOD_MS (10 * 60 * 1000)
#endif

#ifndef WEATHER_HTTP_TIMEOUT_MS
#define WEATHER_HTTP_TIMEOUT_MS (8000)
#endif

static const char *TAG = "WEATHER";

static SemaphoreHandle_t s_state_mu;
static weather_state_t s_state;
static uint32_t s_state_version;

static void weather_state_set(const weather_state_t *src)
{
    if (s_state_mu == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    s_state = *src;
    s_state_version++;
    s_state.version = s_state_version;
    xSemaphoreGive(s_state_mu);
}

bool Weather_GetState(weather_state_t *out_state)
{
    if (out_state == NULL || s_state_mu == NULL) {
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(s_state_mu, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out_state = s_state;
        ok = true;
        xSemaphoreGive(s_state_mu);
    }
    return ok;
}

static void weather_set_timezone(void)
{
    setenv("TZ", DEFAULT_TIMEZONE, 1);
    tzset();
}

static bool weather_time_is_sane(void)
{
    time_t now = time(NULL);
    return now > 1577836800; // 2020-01-01
}

static void weather_time_sync_sntp(void)
{
    if (weather_time_is_sane()) {
        return;
    }

    weather_set_timezone();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    for (int i = 0; i < 20; i++) {
        if (weather_time_is_sane()) {
            ESP_LOGI(TAG, "Time synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "Time not synced (TLS may fail)");
}

static esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, int *out_http_status)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "mbta-lcd/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (out_http_status) {
        *out_http_status = status;
    }

    int total = 0;
    while (total < (int)buf_size - 1) {
        int r = esp_http_client_read(client, buf + total, (int)buf_size - 1 - total);
        if (r < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';

    // Detect truncation
    if (total == (int)buf_size - 1) {
        char extra;
        int more = esp_http_client_read(client, &extra, 1);
        if (more > 0) {
            ESP_LOGW(TAG, "Response truncated (buf_size=%d)", (int)buf_size);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static const char *weather_code_to_condition(int code)
{
    if (code == 0) return "Clear";
    if (code >= 1 && code <= 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if ((code >= 51 && code <= 57) || (code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "Snow";
    if (code >= 95 && code <= 99) return "Storm";
    return "Weather";
}

static bool parse_weather_json(const char *json, weather_state_t *out)
{
    if (json == NULL || out == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }

    bool ok = false;

    // current.temperature_2m, current.weather_code (+ precipitation fields when available)
    cJSON *current = cJSON_GetObjectItem(root, "current");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");

    if (!cJSON_IsObject(current) || !cJSON_IsObject(daily)) {
        goto out;
    }

    cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *wcode = cJSON_GetObjectItem(current, "weather_code");
    if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(wcode)) {
        goto out;
    }

    cJSON *precip = cJSON_GetObjectItem(current, "precipitation");
    cJSON *rain = cJSON_GetObjectItem(current, "rain");
    cJSON *snowfall = cJSON_GetObjectItem(current, "snowfall");
    double precip_mm = cJSON_IsNumber(precip) ? precip->valuedouble : 0.0;
    double rain_mm = cJSON_IsNumber(rain) ? rain->valuedouble : 0.0;
    double snow_mm = cJSON_IsNumber(snowfall) ? snowfall->valuedouble : 0.0;

    cJSON *tmax_arr = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *tmin_arr = cJSON_GetObjectItem(daily, "temperature_2m_min");
    if (!cJSON_IsArray(tmax_arr) || !cJSON_IsArray(tmin_arr)) {
        goto out;
    }

    cJSON *tmax0 = cJSON_GetArrayItem(tmax_arr, 0);
    cJSON *tmin0 = cJSON_GetArrayItem(tmin_arr, 0);
    if (!cJSON_IsNumber(tmax0) || !cJSON_IsNumber(tmin0)) {
        goto out;
    }

    memset(out, 0, sizeof(*out));
    out->temp_c = (int)lround(temp->valuedouble);
    out->high_c = (int)lround(tmax0->valuedouble);
    out->low_c = (int)lround(tmin0->valuedouble);

    if (snow_mm > 0.0) {
        strlcpy(out->condition, "Snowing", sizeof(out->condition));
    } else if (rain_mm > 0.0 || precip_mm > 0.0) {
        strlcpy(out->condition, "Raining", sizeof(out->condition));
    } else {
        const char *cond = weather_code_to_condition(wcode->valueint);
        strlcpy(out->condition, cond, sizeof(out->condition));
    }

    out->has_data = true;
    ok = true;

out:
    cJSON_Delete(root);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;

    if (s_state_mu == NULL) {
        s_state_mu = xSemaphoreCreateMutex();
    }

    weather_state_t init = {0};
    init.has_data = false;
    init.is_fetching = false;
    strlcpy(init.condition, "Weather", sizeof(init.condition));
    weather_state_set(&init);

    bool sntp_attempted = false;

    char url[256];
    snprintf(
        url,
        sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,weather_code,precipitation,rain,snowfall&daily=temperature_2m_max,temperature_2m_min&temperature_unit=celsius&timezone=auto",
        (double)WEATHER_LATITUDE,
        (double)WEATHER_LONGITUDE);

    char json_buf[4096];

    while (1) {
        // WiFi/netif init happens asynchronously in Wireless_Init().
        // Avoid touching LWIP (DNS/TLS/HTTP/SNTP) until WiFi is connected,
        // otherwise tcpip_send_msg_wait_sem can assert with "Invalid mbox".
        if (Wireless_GetStatus() != WIRELESS_STATUS_CONNECTED) {
            weather_state_t no_wifi = {0};
            no_wifi.has_data = false;
            no_wifi.is_fetching = false;
            strlcpy(no_wifi.condition, "No WiFi", sizeof(no_wifi.condition));
            weather_state_set(&no_wifi);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!sntp_attempted) {
            sntp_attempted = true;
            weather_time_sync_sntp();
        }

        weather_state_t st = {0};
        st.is_fetching = true;
        strlcpy(st.condition, "Weather", sizeof(st.condition));
        weather_state_set(&st);

        int http_status = 0;
        esp_err_t err = http_get_to_buffer(url, json_buf, sizeof(json_buf), &http_status);

        weather_state_t new_state = {0};
        new_state.is_fetching = false;

        if (err == ESP_OK && http_status == 200 && parse_weather_json(json_buf, &new_state)) {
            // parsed OK
        } else {
            new_state.has_data = false;
            strlcpy(new_state.condition, "No data", sizeof(new_state.condition));
            ESP_LOGW(TAG, "Weather fetch failed: err=%s status=%d", esp_err_to_name(err), http_status);
        }

        weather_state_set(&new_state);
        vTaskDelay(pdMS_TO_TICKS(WEATHER_FETCH_PERIOD_MS));
    }
}

void Weather_TaskStart(void)
{
    // Start only once
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    xTaskCreatePinnedToCore(weather_task, "weather", 8192, NULL, 3, NULL, 0);
}
