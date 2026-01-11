idf_component_register(
                         SRCS "main.c" 
                              "LCD_Driver/Vernon_ST7789T/Vernon_ST7789T.c" 
                              "LCD_Driver/ST7789.c"
                              "LVGL_Driver/LVGL_Driver.c"
                              "MBTA/mbta.c"
                              "Weather/weather.c"
                              "RGB/RGB.c"
                              "Wireless/Wireless.c"

                         INCLUDE_DIRS 
                              "./LCD_Driver/Vernon_ST7789T" 
                              "./LCD_Driver" 
                              "./LVGL_Driver" 
                              "./MBTA"
                              "./Weather"
                              "./RGB" 
                              "./Wireless"
                              "."

                         PRIV_REQUIRES
                              driver
                              esp_lcd
                              esp_wifi
                              nvs_flash
                              esp_http_client
                              esp_netif
                              esp_event
                              mbedtls
                              json
                       )
