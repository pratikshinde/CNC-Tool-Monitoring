# Host test build for the pure logic modules.
#
#   make        build and run
#   make clean
#
# No ESP-IDF and no hardware required. Warnings are errors here on purpose:
# this is the cheapest place in the project to catch a mistake.

CC      ?= gcc
CFLAGS  := -std=gnu11 -O1 -g \
           -Wall -Wextra -Werror \
           -Wno-unused-parameter \
           -I../main -Istubs
LDFLAGS := -lm

SRCS := test_main.c \
        ../main/app_config.c \
        ../main/scaling.c \
        ../main/spindle_sm.c \
        ../main/alarm.c \
        ../main/do_map.c

BIN := run_tests

.PHONY: all run clean

all: run

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
