/*
 * alarm.c
 */

#include "alarm.h"

#include <string.h>

/* Bands 0 and 1 are the low limits; 2 and 3 are the high limits. */
static inline bool band_is_high(band_t b)
{
    return (b == BAND_HI || b == BAND_HIHI);
}

/* ============================================================
 * Band evaluation
 * ============================================================ */

/*
 * Hysteresis is applied only on the way out, and always in the direction
 * that makes clearing harder than tripping:
 *
 *   high band:  trips at  value >= limit
 *               clears at value <  limit - hysteresis
 *
 *   low band:   trips at  value <= limit
 *               clears at value >  limit + hysteresis
 *
 * Applying it symmetrically would create a dead zone where the alarm
 * neither trips nor clears, and the state would depend on how the value
 * got there.
 */
static bool band_compare(const band_cfg_t *cfg, band_t b, float value,
                         bool currently_active)
{
    if (band_is_high(b)) {
        return currently_active ? (value >= cfg->limit - cfg->hysteresis)
                                : (value >= cfg->limit);
    }
    return currently_active ? (value <= cfg->limit + cfg->hysteresis)
                            : (value <= cfg->limit);
}

static void band_eval(band_state_t *st, const band_cfg_t *cfg, band_t b,
                      float value, bool armed, uint32_t now_ms)
{
    if (!cfg->enabled) {
        st->raw = false;
        st->active = false;
        /* A disabled band cannot hold a latch. If a user disables a band
         * to silence an alarm, that is a deliberate act and the latch
         * should go with it. */
        st->latched = false;
        return;
    }

    bool raw = band_compare(cfg, b, value, st->active);

    if (raw != st->raw) {
        st->raw = raw;
        st->raw_since_ms = now_ms;
    }

    /* Wrap-safe by construction: unsigned subtraction wraps the same way
     * now_ms itself wraps, so this stays correct across the 49.7-day
     * rollover. See alarm.h's header comment. */
    uint32_t stable_ms = now_ms - st->raw_since_ms;

    if (raw && !st->active) {
        if (armed && stable_ms >= (uint32_t)cfg->on_delay_ms) {
            st->active = true;
            st->value_at_trip = value;
            if (cfg->latching) {
                st->latched = true;
                st->acknowledged = false;
            }
        }
    } else if (!raw && st->active) {
        if (stable_ms >= (uint32_t)cfg->off_delay_ms) {
            st->active = false;
        }
    }
}

/* ============================================================
 * Sudden-change detection (TW-R8, TW-R9)
 * ============================================================ */

static void hist_push(transient_hist_t *h, float v, uint32_t t)
{
    h->value[h->head] = v;
    h->time_ms[h->head] = t;
    h->head = (uint8_t)((h->head + 1) % TRANSIENT_HISTORY);
    if (h->count < TRANSIENT_HISTORY) h->count++;
}

/* Mean of the samples that are older than `window_ms` but still in the
 * buffer — i.e. the "before" reference against which the newest sample is
 * compared. Returns false if there is not enough history yet, which is
 * the normal state for the first fraction of a second of every cut. */
static bool hist_reference(const transient_hist_t *h, uint32_t now_ms,
                           uint32_t window_ms, float *out_mean)
{
    if (h->count < 3) return false;

    float sum = 0.0f;
    int n = 0;

    for (int i = 0; i < h->count; i++) {
        /* Walk backwards from the newest entry. */
        int idx = (h->head - 1 - i + TRANSIENT_HISTORY * 2) % TRANSIENT_HISTORY;
        /* Wrap-safe: see alarm.h's header comment. */
        if ((uint32_t)(now_ms - h->time_ms[idx]) >= window_ms) {
            sum += h->value[idx];
            n++;
        }
    }

    if (n < 2) return false;
    *out_mean = sum / (float)n;
    return true;
}

static void transient_eval(alarm_state_t *st, const wear_cfg_t *cfg,
                           float current, bool armed, uint32_t now_ms)
{
    hist_push(&st->current_hist, current, now_ms);

    st->breakage = false;
    st->crash    = false;

    if (!armed) return;

    if (cfg->breakage_enabled) {
        float ref;
        if (hist_reference(&st->current_hist, now_ms,
                           (uint32_t)cfg->breakage_window_ms, &ref)) {
            /* Only meaningful against a reference that represents real
             * cutting. Below a token load the percentage is dominated by
             * noise and would fire constantly. */
            if (ref > 0.5f) {
                float drop_pct = (ref - current) / ref * 100.0f;
                if (drop_pct >= (float)cfg->breakage_drop_pct) {
                    st->breakage = true;
                    st->breakage_latched = true;
                }
            }
        }
    }

    if (cfg->crash_enabled) {
        float ref;
        if (hist_reference(&st->current_hist, now_ms,
                           (uint32_t)cfg->crash_window_ms, &ref)) {
            if (ref > 0.5f) {
                float rise_pct = (current - ref) / ref * 100.0f;
                if (rise_pct >= (float)cfg->crash_rise_pct) {
                    st->crash = true;
                    st->crash_latched = true;
                }
            }
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void alarm_init(alarm_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

void alarm_update(alarm_state_t *st, const spindle_cfg_t *cfg,
                  const alarm_input_t *in, bool armed, uint32_t now_ms)
{
    for (int q = 0; q < QTY_COUNT; q++) {
        /* A faulted sensor must not drive process alarms (AI-R3). The
         * reading is meaningless, and letting a dead 4-20 mA loop trip
         * the LoLo band would turn every wiring fault into a spurious
         * tool-breakage indication. */
        bool q_armed = armed && !in->quantity_faulted[q];

        for (int b = 0; b < BAND_COUNT; b++) {
            band_eval(&st->bands[q][b], &cfg->bands[q][b], (band_t)b,
                      in->value[q], q_armed, now_ms);
        }
    }

    transient_eval(st, &cfg->wear, in->value[QTY_CURRENT],
                   armed && !in->quantity_faulted[QTY_CURRENT], now_ms);

    /* --- Resolve severity and roll-ups ------------------------------- */

    bool warning = false, alarm = false;

    for (int q = 0; q < QTY_COUNT; q++) {
        if (st->bands[q][BAND_HI].active   || st->bands[q][BAND_HI].latched ||
            st->bands[q][BAND_LO].active   || st->bands[q][BAND_LO].latched) {
            warning = true;
        }
        if (st->bands[q][BAND_HIHI].active || st->bands[q][BAND_HIHI].latched ||
            st->bands[q][BAND_LOLO].active || st->bands[q][BAND_LOLO].latched) {
            alarm = true;
        }
    }

    bool diag = false;
    for (int q = 0; q < QTY_COUNT; q++) {
        if (in->quantity_faulted[q]) diag = true;
    }

    severity_t sev = SEV_NONE;
    if (diag)                                         sev = SEV_DIAG;
    if (st->trend)                                    sev = SEV_TREND;
    if (warning)                                      sev = SEV_WARNING;
    if (alarm)                                        sev = SEV_ALARM;
    if (st->breakage || st->breakage_latched)         sev = SEV_BREAKAGE;
    if (st->crash    || st->crash_latched)            sev = SEV_CRASH;

    st->severity    = sev;
    st->any_warning = warning || st->trend;
    st->any_alarm   = alarm || st->breakage_latched || st->crash_latched;
}

void alarm_on_cycle_end(alarm_state_t *st, const wear_cfg_t *cfg,
                        float cycle_mean_current)
{
    if (!cfg->trend_enabled) {
        st->rising_cycles = 0;
        st->trend = false;
        st->last_cycle_mean = cycle_mean_current;
        return;
    }

    /* Require a real increase, not sampling noise. 2% of the previous
     * cycle's mean is comfortably above the measurement repeatability
     * and well below the wear signal we are trying to catch. */
    if (st->last_cycle_mean > 0.0f &&
        cycle_mean_current > st->last_cycle_mean * 1.02f) {
        if (st->rising_cycles < 255) st->rising_cycles++;
    } else {
        st->rising_cycles = 0;
    }

    st->trend = (st->rising_cycles >= cfg->trend_cycles);
    st->last_cycle_mean = cycle_mean_current;
}

void alarm_acknowledge(alarm_state_t *st)
{
    for (int q = 0; q < QTY_COUNT; q++) {
        for (int b = 0; b < BAND_COUNT; b++) {
            band_state_t *bs = &st->bands[q][b];
            if (bs->latched) {
                bs->acknowledged = true;
                /* Only drop the latch if the condition has actually
                 * cleared. Acknowledging while still in alarm records
                 * that the operator saw it; it does not make it go away. */
                if (!bs->active) bs->latched = false;
            }
        }
    }

    if (!st->breakage) st->breakage_latched = false;
    if (!st->crash)    st->crash_latched    = false;

    st->rising_cycles = 0;
    st->trend = false;
}

/* ============================================================
 * Strings
 * ============================================================ */

const char *severity_str(severity_t s)
{
    switch (s) {
    case SEV_NONE:     return "none";
    case SEV_DIAG:     return "diagnostic";
    case SEV_TREND:    return "wear trend";
    case SEV_WARNING:  return "warning";
    case SEV_ALARM:    return "alarm";
    case SEV_BREAKAGE: return "tool breakage";
    case SEV_CRASH:    return "crash";
    default:           return "?";
    }
}

const char *band_str(band_t b)
{
    switch (b) {
    case BAND_LOLO: return "LoLo";
    case BAND_LO:   return "Lo";
    case BAND_HI:   return "Hi";
    case BAND_HIHI: return "HiHi";
    default:        return "?";
    }
}

const char *quantity_str(quantity_t q)
{
    switch (q) {
    case QTY_CURRENT:  return "current";
    case QTY_PRESSURE: return "pressure";
    case QTY_RPM:      return "rpm";
    default:           return "?";
    }
}
