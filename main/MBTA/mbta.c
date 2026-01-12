#include "mbta.h"
#include "config.h"

#include "Wireless.h"

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

#define MBTA_POLL_PERIOD_MS   (MBTA_FETCH_PERIOD_MS)
#define MBTA_HTTP_TIMEOUT_MS  (8000)

// User-provided endpoints
// Note: keep responses small so they fit in our JSON buffer.
#define MBTA_BUS_URL MBTA_STOP_1_URL
#define MBTA_T_URL   MBTA_STOP_2_URL

// Titles (kept simple; can be made dynamic later by including route/stop/trip)
#define MBTA_BUS_TITLE MBTA_STOP_1_NAME
#define MBTA_T_TITLE   MBTA_STOP_2_NAME

static const char *TAG = "MBTA";

static SemaphoreHandle_t s_state_mu;
static mbta_state_t s_state;
static uint32_t s_state_version;

static void mbta_state_set(const mbta_state_t *src)
{
    if (s_state_mu == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    s_state = *src;
    // Ensure version is monotonic across updates (UI uses it to detect changes).
    s_state_version++;
    s_state.version = s_state_version;
    xSemaphoreGive(s_state_mu);
}

bool MBTA_GetState(mbta_state_t *out_state)
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

static void mbta_set_timezone(void)
{
    // Localized timezone from config.h
    setenv("TZ", DEFAULT_TIMEZONE, 1);
    tzset();
}

static time_t timegm_compat(struct tm *tmv)
{
    // newlib in ESP-IDF doesn't always provide timegm(); emulate it.
    char old_tz[64] = {0};
    const char *old = getenv("TZ");
    if (old) {
        strlcpy(old_tz, old, sizeof(old_tz));
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(tmv);

    if (old) {
        setenv("TZ", old_tz, 1);
    } else {
        setenv("TZ", DEFAULT_TIMEZONE, 1);
    }
    tzset();
    return t;
}

static bool mbta_time_is_sane(void)
{
    time_t now = time(NULL);
    // 2020-01-01
    return now > 1577836800;
}

static void mbta_time_sync_sntp(void)
{
    if (mbta_time_is_sane()) {
        return;
    }

    mbta_set_timezone();

    // Simple SNTP init (IDF 5.x)
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait up to ~10 seconds for time to become sane.
    for (int i = 0; i < 20; i++) {
        if (mbta_time_is_sane()) {
            ESP_LOGI(TAG, "Time synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "Time not synced (TLS may fail)");
}

static bool iso8601_to_epoch_utc(const char *s, time_t *out_epoch)
{
    // Parses: YYYY-MM-DDTHH:MM:SS(.sss)?(Z|±HH:MM)
    if (s == NULL || out_epoch == NULL) {
        return false;
    }

    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    int n = sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec);
    if (n != 6) {
        return false;
    }

    const char *p = strchr(s, 'T');
    if (p == NULL) {
        return false;
    }
    // Move to end of seconds component.
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    p = strchr(p + 1, ':');
    if (p == NULL) {
        return false;
    }
    p = p + 1; // at seconds
    // Advance past 2 digits of seconds.
    if (p[0] == '\0' || p[1] == '\0') {
        return false;
    }
    p += 2;

    // Optional fractional seconds
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }

    int tz_offset_sec = 0;
    if (*p == 'Z' || *p == '\0') {
        tz_offset_sec = 0;
    } else if (*p == '+' || *p == '-') {
        int sign = (*p == '-') ? -1 : 1;
        int tzh = 0, tzm = 0;
        // Supports ±HH:MM
        if (sscanf(p + 1, "%2d:%2d", &tzh, &tzm) != 2) {
            return false;
        }
        tz_offset_sec = sign * (tzh * 3600 + tzm * 60);
    } else {
        return false;
    }

    struct tm tmv = {0};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;

    // Interpret the parsed wall-clock as UTC then correct by the explicit offset.
    time_t base = timegm_compat(&tmv);
    if (base == (time_t)-1) {
        return false;
    }

    // If string is local time with an offset, UTC = local - offset.
    // Example: 12:00-05:00 => offset=-18000 => UTC = local - (-18000) = local + 5h.
    *out_epoch = base - tz_offset_sec;
    return true;
}

static esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, int *out_http_status)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = MBTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "mbta-lcd/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    // NOTE: Don't use esp_http_client_perform() here, because it can consume the
    // response internally unless you provide an event handler. We want to read
    // the body into our own buffer.
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (out_http_status) {
        *out_http_status = status;
    }

    ESP_LOGI(TAG, "HTTP status=%d content_len=%lld", status, (long long)content_len);

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

    // Detect truncation: if we filled the buffer, try reading 1 more byte.
    if (total == (int)buf_size - 1) {
        char extra;
        int more = esp_http_client_read(client, &extra, 1);
        if (more > 0) {
            ESP_LOGW(TAG, "HTTP body truncated (buf=%u)", (unsigned)buf_size);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static int cmp_time_t(const void *a, const void *b)
{
    const time_t *ta = (const time_t *)a;
    const time_t *tb = (const time_t *)b;
    if (*ta < *tb) return -1;
    if (*ta > *tb) return 1;
    return 0;
}

static bool parse_prediction_epochs(const char *json, time_t *out_epochs, int max_epochs, int *out_count)
{
    if (out_count) {
        *out_count = 0;
    }
    if (json == NULL || out_epochs == NULL || max_epochs <= 0) {
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        int len = (json != NULL) ? (int)strlen(json) : 0;
        ESP_LOGW(TAG, "JSON parse failed (len=%d)", len);
        if (json != NULL && len > 0) {
            char preview[257];
            int n = len < 256 ? len : 256;
            memcpy(preview, json, (size_t)n);
            preview[n] = '\0';
            // Avoid multi-line log spam.
            for (int i = 0; i < n; i++) {
                if (preview[i] == '\n' || preview[i] == '\r') {
                    preview[i] = ' ';
                }
            }
            ESP_LOGW(TAG, "JSON preview: %s", preview);
        }
        return false;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return false;
    }

    int count = 0;
    time_t now = time(NULL);

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, data) {
        if (count >= max_epochs) {
            break;
        }

        cJSON *attributes = cJSON_GetObjectItemCaseSensitive(item, "attributes");
        if (!cJSON_IsObject(attributes)) {
            continue;
        }

        const cJSON *arrival = cJSON_GetObjectItemCaseSensitive(attributes, "arrival_time");
        const cJSON *departure = cJSON_GetObjectItemCaseSensitive(attributes, "departure_time");

        const char *tstr = NULL;
        if (cJSON_IsString(arrival) && arrival->valuestring) {
            tstr = arrival->valuestring;
        } else if (cJSON_IsString(departure) && departure->valuestring) {
            tstr = departure->valuestring;
        }

        if (tstr == NULL) {
            continue;
        }

        time_t epoch = 0;
        if (!iso8601_to_epoch_utc(tstr, &epoch)) {
            continue;
        }

        // Filter out stale predictions.
        if (epoch < now - 30) {
            continue;
        }

        out_epochs[count++] = epoch;
    }

    cJSON_Delete(root);

    if (count == 0) {
        if (out_count) {
            *out_count = 0;
        }
        return true;
    }

    qsort(out_epochs, (size_t)count, sizeof(time_t), cmp_time_t);
    if (out_count) {
        *out_count = count;
    }
    return true;
}

static bool fetch_arrivals_minutes(const char *url, int *out_minutes, int max_minutes, int *out_count)
{
    if (out_count) {
        *out_count = 0;
    }
    if (url == NULL || out_minutes == NULL || max_minutes <= 0) {
        return false;
    }

    // MBTA can return fairly large JSON; keep this generous but bounded.
    char *buf = (char *)calloc(1, 16384);
    if (buf == NULL) {
        return false;
    }

    int http_status = 0;
    esp_err_t err = http_get_to_buffer(url, buf, 16384, &http_status);
    if (err != ESP_OK || http_status < 200 || http_status >= 300) {
        ESP_LOGW(TAG, "HTTP GET failed (%s), status=%d", esp_err_to_name(err), http_status);
        free(buf);
        return false;
    }

    ESP_LOGI(TAG, "HTTP %d len=%d", http_status, (int)strlen(buf));

    time_t epochs[8] = {0};
    int epoch_count = 0;
    bool ok = parse_prediction_epochs(buf, epochs, (int)(sizeof(epochs) / sizeof(epochs[0])), &epoch_count);
    free(buf);

    if (!ok) {
        return false;
    }

    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < epoch_count && n < max_minutes; i++) {
        long delta = (long)(epochs[i] - now);
        int mins = (int)((delta + 59) / 60);
        if (mins < 0) {
            continue;
        }
        out_minutes[n++] = mins;
    }

    if (out_count) {
        *out_count = n;
    }
    return true;
}

static void mbta_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "MBTA task started");

    while (1) {
        mbta_state_t next = {0};
        next.has_data = false;
        next.mode = MBTA_MODE_BUS;
        next.no_bus_service_banner = false;
        next.arrival_count = 0;
        memset(next.arrivals_min, 0, sizeof(next.arrivals_min));
        strlcpy(next.title, MBTA_BUS_TITLE, sizeof(next.title));

        wireless_status_t wifi = Wireless_GetStatus();
        ESP_LOGD(TAG, "poll wifi=%d", (int)wifi);

        time_t now_v;
        time(&now_v);
        struct tm timeinfo;
        localtime_r(&now_v, &timeinfo);
        
        bool time_is_synced = mbta_time_is_sane();
        bool in_hours = (timeinfo.tm_hour >= MBTA_SHOW_START_HOUR && timeinfo.tm_hour < MBTA_SHOW_END_HOUR);

        // Always allow fetching if time isn't synced yet, otherwise respect hours.
        if (wifi == WIRELESS_STATUS_CONNECTED && (in_hours || !time_is_synced)) {
            next.display_off = false;
            // Set fetching status to true before starting
            mbta_state_t current;
            if (MBTA_GetState(&current)) {
                current.is_fetching = true;
                mbta_state_set(&current);
            }

            // Ensure RTC is sane for TLS validation (no-op once synced).
            mbta_time_sync_sntp();

            int mins[3] = {0};
            int cnt = 0;

            bool bus_ok = fetch_arrivals_minutes(MBTA_BUS_URL, mins, 3, &cnt);
            ESP_LOGI(TAG, "bus_ok=%d cnt=%d", (int)bus_ok, cnt);
            if (bus_ok && cnt > 0) {
                next.mode = MBTA_MODE_BUS;
                next.no_bus_service_banner = false;
                next.has_data = true;
                next.arrival_count = cnt;
                memcpy(next.arrivals_min, mins, (size_t)cnt * sizeof(int));
                strlcpy(next.title, MBTA_BUS_TITLE, sizeof(next.title));
            } else if (bus_ok && cnt == 0) {
                // Bus query succeeded but predictions list is empty (data[] == []) => try T
                int tmins[3] = {0};
                int tcnt = 0;
                bool t_ok = fetch_arrivals_minutes(MBTA_T_URL, tmins, 3, &tcnt);

                ESP_LOGI(TAG, "t_ok=%d tcnt=%d", (int)t_ok, tcnt);

                next.mode = MBTA_MODE_T;
                next.no_bus_service_banner = true;
                next.has_data = t_ok;
                next.arrival_count = t_ok ? tcnt : 0;
                if (t_ok && tcnt > 0) {
                    memcpy(next.arrivals_min, tmins, (size_t)tcnt * sizeof(int));
                }
                strlcpy(next.title, MBTA_T_TITLE, sizeof(next.title));
            } else {
                // Bus query failed (HTTP/TLS/parse). Do NOT fall back to T.
                next.mode = MBTA_MODE_BUS;
                next.no_bus_service_banner = false;
                next.has_data = false;
                next.arrival_count = 0;
                strlcpy(next.title, MBTA_BUS_TITLE, sizeof(next.title));
            }
            next.is_fetching = false;
        } else if (wifi == WIRELESS_STATUS_CONNECTED && !in_hours) {
            // Outside hours: explicitly set no data and a sleeping title
            next.display_off = true;
            next.has_data = false;
            strlcpy(next.title, "Sleep Mode", sizeof(next.title));
        } else {
            // No wifi or other state
            next.display_off = !in_hours;
        }

        mbta_state_set(&next);
        vTaskDelay(pdMS_TO_TICKS(MBTA_POLL_PERIOD_MS));
    }
}

void MBTA_TaskStart(void)
{
    if (s_state_mu == NULL) {
        s_state_mu = xSemaphoreCreateMutex();
    }

    // Seed state
    memset(&s_state, 0, sizeof(s_state));
    s_state.mode = MBTA_MODE_BUS;
    s_state.has_data = false;
    strlcpy(s_state.title, MBTA_BUS_TITLE, sizeof(s_state.title));
    s_state_version = 1;
    s_state.version = s_state_version;

    xTaskCreatePinnedToCore(
        mbta_task,
        "mbta_task",
        8192,
        NULL,
        3,
        NULL,
        0);
}
