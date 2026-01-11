#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Current temperature in Celsius (rounded).
    int temp_c;

    // Daily high / low in Celsius (rounded).
    int high_c;
    int low_c;

    // Human-friendly condition (e.g., "Clear", "Cloudy", "Rain").
    char condition[24];

    bool has_data;
    bool is_fetching;

    // Monotonic version, incremented on update.
    uint32_t version;
} weather_state_t;

void Weather_TaskStart(void);

// Snapshot the latest state. Returns true on success.
bool Weather_GetState(weather_state_t *out_state);

#ifdef __cplusplus
}
#endif
