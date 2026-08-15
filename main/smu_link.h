/*
 * smu_link.h — ESP32-side driver for one SMU's I2C link.
 *
 * One instance per SMU (two total: telemetry.c owns them). Implements the
 * protocol described in shared/smu_proto.h and FIRMWARE_DESIGN_SPEC.md
 * §5.1/§5.2: 20 Hz telemetry polling with CRC and staleness detection,
 * and a two-phase config write.
 *
 * The transport (how bytes actually move — real I2C, or smu_mock's fake
 * SMU) is injected as a pair of function pointers, not called directly.
 * This is what makes the protocol logic in smu_link.c testable on a host
 * with no I2C hardware and no ESP-IDF: a test supplies a transport backed
 * by an in-memory buffer, and every CRC/staleness/state-machine rule
 * below runs identically to how it runs against real silicon. Only the
 * transport functions themselves differ between "test", "mock", and
 * "real I2C" — see smu_mock.[ch] and the real transport in smu_link.c's
 * ESP32-specific half.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#include "smu_proto.h"
#include "spindle_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Transport seam
 * ============================================================ */

typedef struct {
    /* Read `len` bytes starting at wire register offset `reg` into `out`.
     * Must return ESP_OK only if exactly `len` bytes were obtained;
     * partial reads count as failure. */
    esp_err_t (*read)(void *ctx, uint16_t reg, void *out, size_t len);

    /* Write `len` bytes from `data` starting at register offset `reg`. */
    esp_err_t (*write)(void *ctx, uint16_t reg, const void *data, size_t len);

    /* Block for approximately `us` microseconds. Only used by
     * smu_link_push_config()'s commit-completion poll — the 20 Hz
     * telemetry path never calls this. A real transport delegates to
     * vTaskDelay; a host-test transport can fast-forward a fake clock
     * instead of actually sleeping, which is what keeps the test suite
     * fast despite exercising real wait-and-retry logic. */
    void (*delay_us)(void *ctx, uint32_t us);

    void *ctx;   /* opaque, passed to read/write/delay_us untouched */
} smu_transport_t;

/* ============================================================
 * Link state
 * ============================================================ */

typedef enum {
    SMU_LINK_DOWN = 0,     /* no valid frame recently, or never had one */
    SMU_LINK_DEGRADED,     /* frames arriving, but some are failing     */
    SMU_LINK_LIVE,         /* last poll succeeded                      */
} smu_link_state_t;

/* Consecutive poll failures before a link that WAS live is declared
 * fully down rather than merely degraded. Three misses at the 50 ms poll
 * period is 150 ms of silence — long enough that it is not one glitched
 * transaction, short enough that it is still well inside "notice before
 * the operator does". */
#define SMU_LINK_DOWN_THRESHOLD  3

typedef struct {
    smu_transport_t   transport;
    uint8_t            spindle_index;   /* 0 or 1 — cross-checked against
                                          * the SMU's own self-report     */

    smu_link_state_t   state;
    uint8_t             consecutive_failures;

    bool                have_good;      /* at least one valid frame ever */
    smu_telemetry_t      last_good;
    int64_t               last_good_us;
    uint16_t              last_seq;

    bool                ident_valid;
    smu_ident_t           ident;

    uint32_t              crc_error_count;
    uint32_t              timeout_count;
    uint32_t              poll_count;

    uint32_t              next_nonce;   /* monotonic; wraps, never reused
                                          * in a way that matters at 20 Hz */
} smu_link_t;

/* ============================================================
 * API
 * ============================================================ */

void smu_link_init(smu_link_t *link, const smu_transport_t *transport,
                   uint8_t spindle_index);

/* One poll cycle: read identity + telemetry, validate CRC and the
 * sequence counter, update state and counters. Never blocks longer than
 * the transport's own read calls do. Safe to call at the 20 Hz cadence
 * FIRMWARE_DESIGN_SPEC.md §3.1/§5.1 specifies. */
void smu_link_poll(smu_link_t *link, int64_t now_us);

/* True only if the link has EVER produced a valid frame AND that frame
 * is no older than max_age_us. This is the single check a consumer
 * should make before trusting link->last_good — do not read last_good
 * without it, since a link that has never gone live still has a
 * zero-initialised (not "safe defaults", just zero) last_good. */
bool smu_link_is_fresh(const smu_link_t *link, int64_t now_us,
                       int64_t max_age_us);

/* Time between identity re-reads while smu_link_push_config() waits for
 * a commit to land. Chosen so a handful of retries covers a real SMU's
 * Data Flash write time (tens of ms, per the M2003 datasheet's page
 * program figures) without hammering the bus. */
#define SMU_LINK_COMMIT_POLL_INTERVAL_US  20000U

/* Two-phase config write: write the block, then issue
 * SMU_CMD_CONFIG_COMMIT, then poll identity until last_cmd_nonce echoes
 * back or roughly `max_wait_us` has elapsed (measured in whole
 * SMU_LINK_COMMIT_POLL_INTERVAL_US steps via transport.delay_us — this
 * function has no clock of its own, deliberately, so it stays
 * host-testable; see smu_transport_t.delay_us). Does several transport
 * transactions in sequence — call from the config-apply path, never
 * from the 20 Hz poll loop.
 *
 * `*result_out` receives the SMU's SMU_RESULT_* code when this returns
 * ESP_OK. A transport failure returns a non-OK esp_err_t and leaves
 * *result_out untouched. Running out of retries without ever seeing the
 * nonce echo back returns ESP_ERR_TIMEOUT. */
esp_err_t smu_link_push_config(smu_link_t *link, const spindle_cfg_t *cfg,
                               uint16_t schema_version, uint8_t mains_hz,
                               int64_t max_wait_us, uint8_t *result_out);

/* Fire a command with a fresh nonce. Does not wait for completion —
 * appropriate for commands the caller does not need to confirm
 * synchronously, such as SMU_CMD_ACKNOWLEDGE from the 20 Hz poller
 * (monitor.c), where blocking there would delay every other telemetry
 * read behind it. */
esp_err_t smu_link_send_command(smu_link_t *link, uint8_t opcode,
                                uint8_t arg8, uint16_t arg16);

/* Fire a command and block (via transport.delay_us retries, same as
 * smu_link_push_config) until its nonce echoes back in identity or
 * max_wait_us elapses. Use this from a request/response context — a web
 * handler for auto-zero, say — where the caller genuinely needs to know
 * whether the SMU accepted the command before it can answer its own
 * caller. Never call this from the 20 Hz poll loop. */
esp_err_t smu_link_send_command_wait(smu_link_t *link, uint8_t opcode,
                                     uint8_t arg8, uint16_t arg16,
                                     int64_t max_wait_us, uint8_t *result_out);

#ifdef __cplusplus
}
#endif
