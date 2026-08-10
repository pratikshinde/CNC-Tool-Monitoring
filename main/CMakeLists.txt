idf_component_register(
    SRCS "main.c"
         "app_config.c"
         "config_store.c"
         "scaling.c"
         "ads1115.c"
         "analog.c"
         "rpm.c"
         "dio.c"
         "spindle_sm.c"
         "alarm.c"
         "do_map.c"
         "monitor.c"
         "wifi.c"
         "modbus.c"
         "web.c"
         "calib.c"
         "trend.c"
         "ota.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES esp_driver_i2c
             esp_driver_gpio
             esp_driver_pcnt
             esp_driver_uart
             esp_timer
             esp_app_format
             app_update
             nvs_flash
             esp_wifi
             esp_netif
             esp_event
             esp_http_server
)
