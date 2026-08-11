/*
 * alarm.h — threshold evaluation, transient detection and severity
 * resolution (SRS §6).
 *
 * Pure C. Time is an argument, never read from a clock, so the whole
 * engine runs deterministically in host tests — which matters, because
 * the delay/hysteresis/latch interactions are exactly the kind of logic
 * that looks right and is not.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "spindle_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Severity ordering (AL-R5). Higher wins when several are active. */
typedef enum {
    SEV_NONE = 0,
    SEV_DIAG,        /* sensor or hardware fault — never a process alarm */
    SEV_TREND,       /* gradual wear                                     */
    SEV_WARNING,     /* Hi / Lo bands                                    */
    SEV_ALARM,       /* HiHi / LoLo bands                                */
    SEV_BREAKAGE,
    SEV_CRASH,
} severity_t;

/* Runtime state of one threshold band. */
typedef struct {
    bool    raw;             /* comparison result this instant           */
    bool    active;          /* after on/off delays                      */
    bool    latched;
    bool    acknowledged;
    int64_t raw_since_us;    /* when `raw` last changed                  */
    float   value_at_trip;   /* measurement when it first asserted       */
} band_state_t;

/* Short history of current samples, for the sudden-change detectors.
 * Sized for the worst case: the longest configurable detector window
 * divided by the fastest sample interval. */
#define TRANSIENT_HISTORY 32

typedef struct {
    float   value[TRANSIENT_HISTORY];
    int64_t time_us[TRANSIENT_HISTORY];
    uint8_t head;
    uint8_t count;
} transient_hist_t;

typedef struct {
    band_state_t     bands[QTY_COUNT][BAND_COUNT];
    transient_hist_t current_hist;

    bool             breakage;
    bool             crash;
    bool             trend;

    /* Wear trend tracking across cycles (TW-R7). */
    float            last_cycle_mean;
    uint8_t          rising_cycles;

    severity_t       severity;

    /* Convenience roll-ups consumed by the output mapper. */
    bool             any_alarm;
    bool             any_warning;
} alarm_state_t;

/* One measurement sample presented to the engine. */
typedef struct {
    float value[QTY_COUNT];
    bool  quantity_faulted[QTY_COUNT]; /* sensor fault: skip process bands */
} alarm_input_t;

void alarm_init(alarm_state_t *st);

/* Evaluate one sample.
 *
 * `armed` comes from the state machine: when false, bands are still
 * evaluated for display but cannot assert. Latched alarms already active
 * remain active — disarming must not silently clear a latch that an
 * operator has not yet seen. */
void alarm_update(alarm_state_t *st, const spindle_cfg_t *cfg,
                  const alarm_input_t *in, bool armed, int64_t now_us);

/* Feed a completed cut into the wear-trend detector. */
void alarm_on_cycle_end(alarm_state_t *st, const wear_cfg_t *cfg,
                        float cycle_mean_current);

/* Clear every latched condition that is no longer physically present.
 * Conditions still true stay latched — acknowledging an active alarm
 * silences the record, not the reality. */
void alarm_acknowledge(alarm_state_t *st);

const char *severity_str(severity_t s);
const char *band_str(band_t b);
const char *quantity_str(quantity_t q);

#ifdef __cplusplus
}
#endif
