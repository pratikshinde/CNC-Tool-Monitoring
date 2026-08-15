/*
 * smu_pack.h — translate between spindle_cfg_t (logical, spindle_config.h)
 * and smu_config_t (wire, smu_proto.h).
 *
 * A separate module from both: smu_proto.h is the wire format only, with
 * no notion of the logical config shape; spindle_config.h is the logical
 * shape with no notion of how it is serialised. This file is the one
 * place that knows both, so that translation logic exists exactly once —
 * compiled into the ESP32 master (which packs a config to push, and will
 * eventually unpack telemetry) and, eventually, into the SMU firmware
 * (which does the reverse: unpack an incoming config, pack outgoing
 * telemetry). Duplicating this mapping on both sides is exactly the kind
 * of drift smu_proto.h's own header comment warns against.
 *
 * Two inputs are threaded through explicitly rather than read off
 * spindle_cfg_t, because neither actually lives there:
 *   - schema_version: main/app_config.h's CONFIG_SCHEMA_VERSION is an
 *     ESP32-root concept; this component must not depend on that
 *     ESP32-only header, so the caller supplies the number instead.
 *   - mains_hz: a genuinely system-wide property (both spindles share one
 *     electrical supply), stored once at system_cfg_t level, not
 *     duplicated per spindle in the persisted blob.
 */
#pragma once

#include <stdint.h>

#include "smu_proto.h"
#include "spindle_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pack a logical spindle config into wire format, CRC included. `dst` is
 * fully overwritten (all padding zeroed) so the CRC covers a
 * deterministic byte pattern regardless of what was in `dst` before. */
void smu_config_pack(const spindle_cfg_t *src, uint8_t spindle_index,
                     uint16_t schema_version, uint8_t mains_hz,
                     smu_config_t *dst);

/* Unpack wire format into a logical spindle config. Does NOT check the
 * CRC or schema_version — callers validate the frame (smu_crc16 against
 * SMU_CRC_PAYLOAD_LEN(smu_config_t)) before unpacking, exactly as
 * FIRMWARE_DESIGN_SPEC.md §5.2's two-phase commit describes. Returns the
 * spindle_index and mains_hz the wire frame carried, via out-params, so
 * the caller can cross-check them against what it expected. */
void smu_config_unpack(const smu_config_t *src, spindle_cfg_t *dst,
                       uint8_t *spindle_index_out, uint8_t *mains_hz_out);

#ifdef __cplusplus
}
#endif
