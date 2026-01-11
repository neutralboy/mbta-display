#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MBTA_MODE_NONE = 0,
    MBTA_MODE_BUS,
    MBTA_MODE_T,
} mbta_mode_t;

typedef struct {
    mbta_mode_t mode;
    bool no_bus_service_banner;

    // Up to 3 upcoming arrivals in minutes (0 == due).
    int arrivals_min[3];
    int arrival_count;

    // UI title to show under the WiFi bar.
    char title[96];

    // If false, UI should show "No data" / placeholders.
    bool has_data;

    // True if a background fetch is active.
    bool is_fetching;

    // True if the display should be powered off (outside scheduled hours).
    bool display_off;

    // Monotonic version, incremented on update.
    uint32_t version;
} mbta_state_t;

void MBTA_TaskStart(void);

// Snapshot the latest state. Returns true on success.
bool MBTA_GetState(mbta_state_t *out_state);

#ifdef __cplusplus
}
#endif
