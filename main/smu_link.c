/*
 * smu_link.c — see smu_link.h.
 *
 * Deliberately free of FreeRTOS/driver/esp_timer includes: everything
 * time- or bus-related crosses the smu_transport_t seam instead. That is
 * what lets host_test exercise every rule below — CRC validation, the
 * frozen-seq stall detector, LIVE/DEGRADED/DOWN transitions, the
 * two-phase config commit — against a fake transport with no I2C
 * hardware and no ESP-IDF, which is the whole point: this file is the
 * one piece of the SMU link that is genuinely safety-adjacent (a wrong
 * staleness call here is a stale reading treated as live), so it is the
 * one piece worth testing this hard.
 */

#include "smu_link.h"

#include <string.h>

#include "smu_pack.h"

/* ============================================================
 * Internal
 * ============================================================ */

static void record_failure(smu_link_t *link)
{
    if (link->consecutive_failures < 255) link->consecutive_failures++;

    if (link->consecutive_failures >= SMU_LINK_DOWN_THRESHOLD) {
        link->state = SMU_LINK_DOWN;
    } else if (link->have_good) {
        /* Had a good frame recently — this is a hiccup, not an outage. */
        link->state = SMU_LINK_DEGRADED;
    } else {
        /* Never had a good frame at all; nothing to be "degraded" from. */
        link->state = SMU_LINK_DOWN;
    }
}

static void record_success(smu_link_t *link, const smu_telemetry_t *telem,
                           const smu_ident_t *ident, int64_t now_us)
{
    link->last_good     = *telem;
    link->last_good_us  = now_us;
    link->last_seq      = telem->seq;
    link->have_good     = true;

    link->ident       = *ident;
    link->ident_valid = true;

    link->consecutive_failures = 0;
    link->state = SMU_LINK_LIVE;
}

/* ============================================================
 * Public API
 * ============================================================ */

void smu_link_init(smu_link_t *link, const smu_transport_t *transport,
                   uint8_t spindle_index)
{
    memset(link, 0, sizeof(*link));
    link->transport     = *transport;
    link->spindle_index = spindle_index;
    link->state         = SMU_LINK_DOWN;
    link->next_nonce    = 1;   /* 0 reserved as "no command pending" */
}

void smu_link_poll(smu_link_t *link, int64_t now_us)
{
    link->poll_count++;

    smu_ident_t ident;
    if (link->transport.read(link->transport.ctx, SMU_REG_IDENT,
                             &ident, sizeof(ident)) != ESP_OK) {
        link->timeout_count++;
        record_failure(link);
        return;
    }

    /* Wrong magic/protocol version means either garbage came back or
     * this is not a real SMU at all — a wiring or addressing mistake,
     * not a transient bus error. Treat it the same as a failed read: it
     * is not safe to trust anything else in this frame. */
    if (ident.magic != SMU_PROTO_MAGIC || ident.proto_version != SMU_PROTO_VERSION) {
        record_failure(link);
        return;
    }

    smu_telemetry_t telem;
    if (link->transport.read(link->transport.ctx, SMU_REG_TELEMETRY,
                             &telem, sizeof(telem)) != ESP_OK) {
        link->timeout_count++;
        record_failure(link);
        return;
    }

    uint16_t computed = smu_crc16(&telem, SMU_CRC_PAYLOAD_LEN(smu_telemetry_t));
    if (computed != telem.crc16) {
        link->crc_error_count++;
        record_failure(link);
        return;
    }

    /* A CRC-valid frame with an unchanged sequence number is the stalled-
     * SMU case the header comment for smu_telemetry_t.seq exists for: the
     * bytes are self-consistent but they are not NEW. Only the very
     * first frame this link has ever seen is exempt (there is no prior
     * seq to compare against). Counted with the transport failures
     * rather than CRC failures — nothing is actually corrupt here, the
     * link is just not producing anything fresh. */
    if (link->have_good && telem.seq == link->last_seq) {
        link->timeout_count++;
        record_failure(link);
        return;
    }

    record_success(link, &telem, &ident, now_us);
}

bool smu_link_is_fresh(const smu_link_t *link, int64_t now_us, int64_t max_age_us)
{
    if (!link->have_good) return false;
    return (now_us - link->last_good_us) <= max_age_us;
}

/* Build a command frame with a fresh nonce (0 is reserved, so it is
 * skipped on wrap) and write it. Shared by the fire-and-forget and
 * wait-for-result paths below — both need exactly this, differing only
 * in whether they poll afterward. */
static esp_err_t send_command(smu_link_t *link, uint8_t opcode, uint8_t arg8,
                              uint16_t arg16, uint32_t *nonce_out)
{
    smu_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.arg8   = arg8;
    cmd.arg16  = arg16;
    cmd.nonce  = link->next_nonce++;
    if (link->next_nonce == 0) link->next_nonce = 1;
    cmd.crc16  = smu_crc16(&cmd, SMU_CRC_PAYLOAD_LEN(smu_command_t));

    if (nonce_out) *nonce_out = cmd.nonce;
    return link->transport.write(link->transport.ctx, SMU_REG_COMMAND,
                                 &cmd, sizeof(cmd));
}

/* Poll identity until it echoes `nonce` or the retry budget derived from
 * `max_wait_us` runs out. See smu_link.h: this function has no clock of
 * its own, so "how long to wait" is a whole number of fixed-size delay
 * steps, not elapsed wall-clock time — that is what keeps it
 * host-testable against a transport with no real clock either. */
static esp_err_t wait_for_nonce(smu_link_t *link, uint32_t nonce,
                                int64_t max_wait_us, uint8_t *result_out)
{
    uint32_t attempts = (uint32_t)(max_wait_us / SMU_LINK_COMMIT_POLL_INTERVAL_US);
    if (attempts < 1) attempts = 1;

    for (uint32_t i = 0; i < attempts; i++) {
        smu_ident_t ident;
        esp_err_t err = link->transport.read(link->transport.ctx, SMU_REG_IDENT,
                                             &ident, sizeof(ident));
        if (err == ESP_OK &&
            ident.magic == SMU_PROTO_MAGIC &&
            ident.proto_version == SMU_PROTO_VERSION &&
            ident.last_cmd_nonce == nonce) {
            link->ident       = ident;
            link->ident_valid = true;
            if (result_out) *result_out = ident.last_cmd_result;
            return ESP_OK;
        }
        link->transport.delay_us(link->transport.ctx,
                                 SMU_LINK_COMMIT_POLL_INTERVAL_US);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t smu_link_send_command(smu_link_t *link, uint8_t opcode,
                                uint8_t arg8, uint16_t arg16)
{
    return send_command(link, opcode, arg8, arg16, NULL);
}

esp_err_t smu_link_send_command_wait(smu_link_t *link, uint8_t opcode,
                                     uint8_t arg8, uint16_t arg16,
                                     int64_t max_wait_us, uint8_t *result_out)
{
    uint32_t nonce;
    esp_err_t err = send_command(link, opcode, arg8, arg16, &nonce);
    if (err != ESP_OK) return err;
    return wait_for_nonce(link, nonce, max_wait_us, result_out);
}

esp_err_t smu_link_push_config(smu_link_t *link, const spindle_cfg_t *cfg,
                               uint16_t schema_version, uint8_t mains_hz,
                               int64_t max_wait_us, uint8_t *result_out)
{
    smu_config_t wire;
    smu_config_pack(cfg, link->spindle_index, schema_version, mains_hz, &wire);

    esp_err_t err = link->transport.write(link->transport.ctx, SMU_REG_CONFIG,
                                          &wire, sizeof(wire));
    if (err != ESP_OK) return err;

    uint32_t nonce;
    err = send_command(link, SMU_CMD_CONFIG_COMMIT, 0, 0, &nonce);
    if (err != ESP_OK) return err;

    return wait_for_nonce(link, nonce, max_wait_us, result_out);
}
