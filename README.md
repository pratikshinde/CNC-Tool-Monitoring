# CNC Tool Monitor V2

Two-spindle CNC tool condition monitor. Measures spindle current (CT),
spindle pressure (4–20 mA / 0–10 V) and spindle speed (pulse input), and
drives alarm outputs to a PLC when a tool wears, breaks or crashes.

**V2 is a distributed architecture, not a single ESP32 doing everything.**
Two dedicated per-spindle MCUs own the entire safety path — acquisition,
arming, threshold evaluation, PLC output — with no network, no I²C
transaction and no other processor in the loop. The ESP32 owns networking,
the web UI and configuration authority, but has no say in whether a given
sample trips an alarm. See [`FIRMWARE_DESIGN_SPEC.md`](FIRMWARE_DESIGN_SPEC.md)
for the full reasoning and [`SMU_HARDWARE_REQUIREMENTS.md`](SMU_HARDWARE_REQUIREMENTS.md)
for the hardware side.

| Image | Runs on | Count | Responsibility |
|---|---|---|---|
| **SMU firmware** | Nuvoton M2003FC1AE | 2 (one per spindle) | Acquisition, alarm evaluation, PLC fault signalling — the entire safety path. Separate project: `CNC Tool Monitoring V2 SMU` |
| **Master firmware** (this repo) | ESP32, 16 MB flash | 1 | Networking, web UI, config authority, Modbus, OTA — no safety path |

**Target:** ESP32 · ESP-IDF v6.0.2 · standalone, no cloud

---

## Status

The V1→V2 migration is complete: local acquisition (`ads1115.c`, `analog.c`,
`rpm.c`, `dio.c`, `do_map.c`) has been removed from this image entirely — the
ESP32 no longer touches a sensor or an output. All measurement and alarm
data now arrives from the two SMUs over I²C and is aggregated for the UI,
Modbus and OTA layers built in V1.

| Area | State |
|---|---|
| Config store (NVS) with known-good rollback | done |
| SMU link: I²C protocol, telemetry/ident aggregation | done, host-tested against a fake transport |
| Software SMU stand-in (`smu_mock.c`) for development without hardware | done — default until real SMU hardware exists (`CONFIG_SMU_USE_MOCK`) |
| Real I²C transport (`smu_i2c_transport.c`) | done, compiles clean, **unverified against hardware** — no SMU board exists yet |
| WiFi (station + always-on fallback AP) | done |
| Modbus RTU (RS485) + Modbus TCP, read-only telemetry registers | done — see [`MODBUS_REGISTER_MAP.md`](MODBUS_REGISTER_MAP.md) |
| Guided calibration (2-point pressure, 1-point current, auto-zero) | done — pushes results to the target SMU over I²C |
| Job templates: save/load/export/import/copy full spindle setups | done, host-tested — see below |
| Web UI: dashboard, trends, thresholds, jobs, calibration, SMU status, comms, system | done — gzipped SPA served from its own LittleFS partition, independently OTA-updatable |
| OTA: browser-upload firmware update, project/version checked, automatic rollback | done |
| On-device data logging | **not implemented, by design** — see `FIRMWARE_DESIGN_SPEC.md` §1.1 |

**Compiles clean against ESP-IDF v6.0.2** under both `CONFIG_SMU_USE_MOCK`
settings. `idf.py build` passes with zero warnings on a full rebuild.

### The device is a monitor, not a historian

Configuration (calibration, thresholds, network settings, job templates)
persists; everything the device *observes* — measurements, alarm state,
latches, cycle counters, the learned wear baseline — is volatile and lost on
power loss, by design. Customers who need history log it externally over
Modbus. See `FIRMWARE_DESIGN_SPEC.md` §1.1 for the full reasoning and the
consequences that follow from it (counters are "since power-on", the wear
baseline relearns every boot, etc.).

---

## Build

### Firmware

```
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig.defaults` sets the 16 MB flash size, the custom partition table
and OTA rollback. Delete any stale `sdkconfig` before the first build so the
defaults are picked up.

`CONFIG_SMU_USE_MOCK` (menuconfig, under this component) selects between the
software SMU stand-in and the real I²C transport. Mock is the default — it
lets the full stack (web UI, Modbus, alarms, jobs) be developed and tested
with no SMU hardware attached. Flip it once real SMU boards exist.

**A partition table change needs a physical reflash, not OTA.** If a unit is
already in the field, changing `partitions.csv` (adding the `jobs`
partition, for instance) requires connecting to it directly — OTA only
replaces the app image, not the partition layout.

### Host tests

No hardware or ESP-IDF needed:

```
cd host_test
make
```

Covers scaling arithmetic, config validation, the spindle state machine,
alarm delays/hysteresis/latching (including behaviour across the `uint32_t`
millisecond wrap), sensor-fault isolation, breakage/crash/wear-trend
detection, the SMU wire protocol and config pack/unpack, `smu_link.c`
against a fake transport, and the job-template extract/apply/check logic —
the last of these including the round-trip guarantee that an apply never
touches a machine's calibration (see Job templates, below).

---

## Layout

```
main/
  board.h              pin map, task/core layout
  app_config.[ch]       ESP32-root config aggregate: WiFi, Modbus, both
                        spindles' spindle_cfg_t                (tested)
  config_store.[ch]     NVS persistence with known-good fallback
  smu_link.[ch]         I2C protocol logic, no transport dependency (tested)
  smu_mock.c            software SMU stand-in — the default until hardware exists
  smu_i2c_transport.c   the real I2C master transport
  monitor.[ch]          polls both SMUs, aggregates telemetry for consumers
  wifi.[ch]             STA + always-on fallback AP
  modbus.[ch]           Modbus RTU (RS485) and TCP slaves, read-only registers
  calib.[ch]             guided field calibration, pushed to the SMU over I2C
  trend.[ch]             5-minute RAM ring buffer for the live graph (not persisted)
  ota.[ch]               browser-upload firmware update into the spare OTA slot
  job_template.[ch]      job/machine field split + cross-machine validation (tested)
  job_store.[ch]         job template CRUD on the `jobs` LittleFS partition
  web.[ch]               HTTP server + API — ~20 routes across telemetry,
                         config, calibration, jobs, OTA, system
  web/index.html         the multi-tab operator UI, gzipped into the `web`
                         LittleFS partition at build time
  main.c                 bring-up
components/
  smu_shared/            portable C compiled verbatim into BOTH this image
                         and the SMU firmware: alarm.c, spindle_sm.c,
                         scaling.c, spindle_config.c, smu_crc16.c, smu_pack.c
host_test/                gcc test harness — no ESP-IDF, no hardware
```

Anything under `components/smu_shared/` or marked *tested* above has no
ESP-IDF dependency and takes time as a `uint32_t` millisecond argument
rather than reading a clock, so it runs deterministically on a host. That is
deliberate: the delay, hysteresis and latch interactions in `alarm.c`, and
the job/machine field split in `job_template.c`, are exactly the kind of
logic that looks obviously correct and is not.

---

## Job templates

A saved job template captures everything about a cutting operation —
thresholds, cut-detect levels, wear-detection settings — and can be
reloaded in one action, exported to a file, and applied on a **different**
machine or a different spindle.

The design's one hard rule: a template can never carry this machine's
**calibration** (CT ratio, gain corrections, sensor ranges, zero-offset
tare). `job_profile_t` — the struct a template is built from — has no field
that could hold a calibration value, so there is nothing for the apply path
to accidentally overwrite even if someone tried. Applying a template checks
compatibility against the destination spindle's hardware (unit mismatch is
a hard refusal; an unreachable band is a warning, since apply is already
blocked while that spindle is cutting) and always leaves the destination's
own calibration untouched.

`test_job_template_machine_fields_survive()` in `host_test/test_main.c` is
the guarantee in test form: it builds two machines with deliberately
different CT ratings and sensor ranges, applies a template across them, and
asserts every calibration field is byte-identical afterward. It must not be
deleted or weakened.

Storage is its own `jobs` LittleFS partition (`partitions.csv`), separate
from the `web` partition that holds the UI — a UI OTA update replaces that
partition wholesale, and customer-saved templates must survive it.

---

## Three things to know before trusting this on a machine

### 1. The real SMU I²C transport is unverified — no hardware exists yet

`smu_i2c_transport.c` compiles clean and implements the same
`smu_transport_t` interface `smu_mock.c` does, but has never talked to a
real M2003FC1AE. The SMU PCB is still in layout. Everything downstream —
telemetry aggregation, alarms, calibration push, jobs — has been developed
and tested against the mock. Bench verification (SMU Phase 7 in the
companion firmware project) is blocked on hardware, not code.

### 2. The wear-detection defaults are placeholders

`breakage_drop_pct`, `crash_rise_pct`, `adaptive_k_warn` and the rest are
engineering starting points, not validated values. They must be tuned
against real cutting data once hardware exists to generate it.

### 3. No authentication on the web UI

OTA upload, job import/export, config and calibration writes are all
unauthenticated HTTP. Anyone who can reach the device on the network can
reflash it or change its thresholds. Decide on an authentication story
before this goes on a shop network the customer doesn't fully control.

---

## Design decisions worth knowing

**The safety path fits entirely inside one SMU.** No code path that asserts
or clears a PLC fault output may block on, or read state produced by, the
ESP32. A WiFi outage, a reboot, or a firmware update in progress on the
ESP32 never leaves a spindle unprotected.

**Monitoring is armed only in the CUTTING state.** Spindle start-up draws
several times the cutting current and an idle spindle at speed still draws
windage. Evaluating thresholds unconditionally would trip an alarm on every
cycle, and a monitor that cries wolf gets disconnected. This runs on the
SMU, not the ESP32 — it is part of the safety path.

**A faulted sensor raises a diagnostic, never a process alarm.** A broken
4–20 mA loop reads 0 bar, which is below any sensible LoLo limit. Without
isolation, every wiring fault would present as a critical process alarm.

**A job template structurally cannot carry calibration.** See Job
templates, above — this is the same design instinct as sensor-fault
isolation, applied to a different failure mode: make the dangerous state
impossible to represent, rather than trusting a check to catch it every
time.

**Outputs are held in their safe state until the first complete measurement
cycle**, and the system-healthy output is inverted by default so that a
fault, a broken wire and a dead device all read alike to the PLC.

---

## Safety

This is a **monitoring and advisory device, not a safety device**. It
carries no SIL or PL rating and must not be the sole protection against
machine damage or injury.
