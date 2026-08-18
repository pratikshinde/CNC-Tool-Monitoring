# Host test build for the pure logic modules.
#
#   make        build and run
#   make clean
#
# No ESP-IDF and no hardware required. Warnings are errors here on purpose:
# this is the cheapest place in the project to catch a mistake.

CC      ?= gcc
# -std=gnu11: also the floor smu_proto.h enforces itself via its own
# __STDC_VERSION__ #error guard (needs C11 for _Static_assert). Do not
# drop below C11 here without checking that guard.
CFLAGS  := -std=gnu11 -O1 -g \
           -Wall -Wextra -Werror \
           -Wno-unused-parameter \
           -I../main -I../components/smu_shared -Istubs
LDFLAGS := -lm

# app_config.c (ESP32 root) stays under ../main. Everything else that
# moved to the portable smu_shared component during the V2 split — see
# FIRMWARE_DESIGN_SPEC.md §4.3 — is pulled from its new location so this
# suite keeps testing the exact files that ship, not stale copies.
#
# do_map.c is gone (V2 migration Phase 2): the ESP32 drives no physical
# output in V2, so its configurable DO-source matrix has no successor
# here — see the "Output mapping — RETIRED" note in test_main.c.
# smu_link.c lives under ../main (it is ESP32-master-only — the SMU side
# of this link is entirely different code in the future SMU firmware
# project) but is deliberately free of FreeRTOS/driver/esp_timer
# includes, communicating with its transport only through the
# smu_transport_t function-pointer seam. That is what makes it
# host-testable here against a fake in-memory transport with no I2C
# hardware — see test_smu_link() in test_main.c. stubs/esp_err.h supplies
# the one ESP-IDF type it still needs.
SRCS := test_main.c \
        ../main/app_config.c \
        ../main/smu_link.c \
        ../main/job_template.c \
        ../components/smu_shared/spindle_config.c \
        ../components/smu_shared/scaling.c \
        ../components/smu_shared/spindle_sm.c \
        ../components/smu_shared/alarm.c \
        ../components/smu_shared/smu_crc16.c \
        ../components/smu_shared/smu_pack.c

BIN := run_tests

.PHONY: all run clean

all: run

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
