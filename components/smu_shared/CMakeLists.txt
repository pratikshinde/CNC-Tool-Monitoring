# smu_shared — portable C, no ESP-IDF dependency.
#
# Compiled into the ESP32 master image as a normal ESP-IDF component (this
# file), and compiled directly into the SMU (arm-none-eabi) image from the
# same .c files with no ESP-IDF wrapper at all. That is the whole point of
# keeping this component free of esp_*/driver/*/freertos/* includes: one
# definition of the wire protocol, the config schema, and the safety-path
# logic (alarm.c, spindle_sm.c, scaling.c), not two that can drift apart.
#
# Do not add REQUIRES here. If a future change needs one, that change no
# longer belongs in this component.

idf_component_register(
    SRCS "alarm.c"
         "scaling.c"
         "smu_crc16.c"
         "smu_pack.c"
         "spindle_config.c"
         "spindle_sm.c"
    INCLUDE_DIRS "."
)
