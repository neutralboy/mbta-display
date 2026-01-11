#pragma once

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "nvs_flash.h" 


#include <stdio.h>
#include <string.h>  // For memcpy
#include "esp_system.h"




extern uint16_t BLE_NUM;
extern uint16_t WIFI_NUM;
extern bool Scan_finish;

typedef enum {
	WIRELESS_STATUS_CONNECTING = 0,
	WIRELESS_STATUS_CONNECTED,
	WIRELESS_STATUS_FAILED,
} wireless_status_t;

void Wireless_Init(void);

wireless_status_t Wireless_GetStatus(void);
const char *Wireless_GetIpStr(void);