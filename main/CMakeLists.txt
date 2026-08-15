# scaling.c, spindle_sm.c and alarm.c moved to the smu_shared component
# (../components/smu_shared) — they are portable C shared verbatim with
# the future SMU firmware image. spindle_config.c also moved there: it is
# the per-spindle config schema/defaults/validation, split out of what
# used to be a single app_config.c (see FIRMWARE_DESIGN_SPEC.md §4.3).
#
# app_config.c is listed below anyway — it is a DIFFERENT file now, the
# ESP32-only root aggregate (WiFi/Modbus/spindle[NUM_SPINDLES]) that wraps
# spindle_config.h's types. It never compiles for the SMU.
#
# REQUIRES smu_shared is what keeps #include "spindle_config.h" (and, via
# it, spindle_cfg_t / band_cfg_t / etc.) resolving from every file here
# that needs them — app_config.[ch] chiefly, transitively everything that
# includes app_config.h (config_store.c, monitor.c, calib.c, web.c,
# modbus.c).
#
# V2 migration Phase 2: ads1115.c / analog.c / rpm.c / dio.c / do_map.c
# are DELETED — acquisition, arming and output-driving all move to the
# SMU (FIRMWARE_DESIGN_SPEC.md §2.3). esp_driver_pcnt dropped from
# REQUIRES below along with them; it existed only for rpm.c's pulse
# counter.
#
# V2 migration Phase 3: smu_link.c (protocol logic, host-testable) plus
# two smu_transport_t implementations — smu_mock.c (a software SMU stand-
# in, the default per Kconfig CONFIG_SMU_USE_MOCK) and
# smu_i2c_transport.c (the real thing, unverified against hardware since
# none exists yet). Both compile unconditionally; telemetry.c selects
# between them with #if CONFIG_SMU_USE_MOCK, so switching later is a
# menuconfig change, not a source edit.
idf_component_register(
    SRCS "main.c"
         "app_config.c"
         "config_store.c"
         "smu_link.c"
         "smu_mock.c"
         "smu_i2c_transport.c"
         "monitor.c"
         "wifi.c"
         "modbus.c"
         "web.c"
         "calib.c"
         "trend.c"
         "ota.c"
    INCLUDE_DIRS "."
    REQUIRES smu_shared
             esp_driver_i2c
             esp_driver_gpio
             esp_driver_uart
             esp_timer
             esp_app_format
             app_update
             nvs_flash
             esp_wifi
             esp_netif
             esp_event
             esp_http_server
             joltwallet__littlefs
)

# V2 migration Phase 5: the UI no longer ships EMBED_TXTFILES'd into the app
# image. It is gzipped once here at build time and packed into a LittleFS
# image flashed to the `web` partition (partitions.csv reserves 512 KB for
# it) — this is what makes OTA-R4 real (the UI is updatable independently of
# firmware) and shrinks it from the raw ~42.5 KB to ~11.3 KB on the wire.
# Compression happens at build time, not per-request: the ESP32 has no spare
# cycles to gzip on the fly for a request that already shares CORE_NETWORK
# with WiFi, Modbus and OTA — see web.c's handle_index().
set(web_fs_dir "${CMAKE_CURRENT_BINARY_DIR}/web_fs")
set(web_index_gz "${web_fs_dir}/index.html.gz")

add_custom_command(
    OUTPUT "${web_index_gz}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${web_fs_dir}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gzip_asset.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "${web_index_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/gzip_asset.py"
    COMMENT "Gzipping web/index.html for the LittleFS web partition"
    VERBATIM
)

littlefs_create_partition_image(web "${web_fs_dir}" FLASH_IN_PROJECT
                                 DEPENDS "${web_index_gz}")
