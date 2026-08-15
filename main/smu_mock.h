/*
 * smu_mock.h — software stand-in for an SMU, behind CONFIG_SMU_USE_MOCK.
 *
 * Not a fake that returns plausible-looking numbers: it runs the real
 * spindle_sm.c and alarm.c (the exact modules that will run on the SMU
 * itself) against a synthesized current/pressure/RPM signal, so the
 * arming, banding, breakage/crash, and wear-baseline logic this exercises
 * is the genuine article, not a stand-in for it. It also implements the
 * real two-phase config commit (SMU_REG_CONFIG write, then
 * SMU_CMD_CONFIG_COMMIT), including rejecting an invalid config via
 * spindle_cfg_validate() — so smu_link_push_config() gets tested against
 * realistic accept/reject behaviour, not just a transport that always
 * says yes.
 *
 * One instance per spindle. Each exposes an smu_transport_t so it plugs
 * into smu_link_t exactly like a real I2C transport would.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm.h"
#include "smu_link.h"
#include "smu_proto.h"
#include "spindle_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t          spindle_index;

    spindle_cfg_t    cfg;
    spindle_sm_t      sm;
    alarm_state_t     alarm;
    float             peak_hold;

    int64_t           sim_start_us;
    uint16_t          seq;
    uint32_t          cycle_count;
    uint32_t          last_cycle_ms;
    float             last_cycle_mean_a;
    float             last_cycle_peak_a;

    /* Staged config, between the SMU_REG_CONFIG write and the
     * SMU_CMD_CONFIG_COMMIT that either applies or discards it — the
     * same two-phase discipline a real SMU is required to implement. */
    uint8_t           staged_cfg[sizeof(smu_config_t)];
    bool               staged_cfg_present;

    smu_ident_t        ident;

    /* Fault injection (FIRMWARE_DESIGN_SPEC.md §7 verification section):
     * exercises the failure-mode table in §6 without hardware. */
    bool  inject_link_down;       /* transport.read/write always fail   */
    bool  inject_crc_corrupt;     /* telemetry CRC is wrong on purpose  */
    bool  inject_freeze_seq;      /* seq stops advancing                */
    bool  inject_current_fault;   /* current_sensor reports SENSOR_OPEN */
    bool  inject_pressure_fault;  /* pressure_sensor reports SENSOR_OPEN*/
} smu_mock_spindle_t;

/* Initialise one mock spindle and return an smu_transport_t bound to it.
 * `now_us` seeds both the state machine's clock and the synthetic signal
 * generator's phase origin. */
smu_transport_t smu_mock_init(smu_mock_spindle_t *mock, uint8_t spindle_index,
                              int64_t now_us);

/* Fault-injection setters, for a debug UI/endpoint to drive later
 * (Phase 4+). Safe to call at any time; take effect on the next poll. */
void smu_mock_set_link_down(smu_mock_spindle_t *mock, bool down);
void smu_mock_set_crc_corrupt(smu_mock_spindle_t *mock, bool corrupt);
void smu_mock_set_freeze_seq(smu_mock_spindle_t *mock, bool freeze);
void smu_mock_set_current_fault(smu_mock_spindle_t *mock, bool faulted);
void smu_mock_set_pressure_fault(smu_mock_spindle_t *mock, bool faulted);

#ifdef __cplusplus
}
#endif
