# CNC Tool Monitor

Two-spindle CNC tool condition monitor. Measures spindle current (CT),
spindle pressure (4–20 mA / 0–10 V) and spindle speed (pulse input), and
drives alarm outputs to a PLC when a tool wears, breaks or crashes.

**Target:** ESP32 (16 MB flash) · ESP-IDF v6.0.2 · standalone, no cloud

Requirements and plan: [`SRS_CNC_Tool_Monitor.md`](../SRS_CNC_Tool_Monitor.md) ·
[`ROADMAP_CNC_Tool_Monitor.md`](../ROADMAP_CNC_Tool_Monitor.md) *(both referenced,
neither exists in this repo yet — known gap)*

**Hardware redesign in progress:** acquisition and PLC fault-signalling are
moving off the ESP32 onto two dedicated per-spindle MCUs (Nuvoton M031FB0AE).
See [`SMU_HARDWARE_REQUIREMENTS.md`](SMU_HARDWARE_REQUIREMENTS.md) for the full
spec — this is a hardware change, not yet reflected in the firmware described
below.

---

## Status

Phase 1–3 core is implemented: configuration, drivers, spindle state
machine, alarm engine and digital output mapping. Networking (WiFi,
Modbus RTU/TCP), a multi-tab web UI (dashboard, live trend graph,
thresholds, guided calibration, comms, system/OTA) and browser-upload OTA
are all implemented. On-device data logging has been **dropped** — see
below — and the full Phase 4 Material UI SPA remains unbuilt.

| Area | State |
|---|---|
| Config store with known-good rollback | done |
| ADS1115 / analogue acquisition | done, bench-verified, see accuracy caveat below |
| RPM via PCNT | done, bench-verified (tachometer cross-check pending) |
| Digital I/O with safe-state and pulse stretching | done |
| Spindle state machine | done, host-tested |
| Alarm engine: bands, delays, hysteresis, latching | done, host-tested |
| Breakage / crash / wear-trend detection | done, host-tested, **defaults unvalidated** |
| Digital output mapping | done, host-tested |
| WiFi (station + always-on fallback AP) | done |
| Modbus RTU (RS485) + Modbus TCP, read-only telemetry registers | done |
| Guided 2-point calibration (pressure) + 1-point + auto-zero (current) | done |
| No-load current deadband | done |
| Web UI: dashboard, trends, thresholds, calibration, comms, system | done, not the Phase 4 SPA |
| OTA: browser-upload firmware update, project/version checked | done |
| On-device data logging | **dropped** — flash budget does not support it |

**Compiles clean against ESP-IDF v6.0.2.** `idf.py build` passes with zero
warnings on a full rebuild of the `main` component.

### Data logging has been dropped

On-device logging to a `logs` LittleFS partition (originally LG-R1…R7) is
no longer planned: the flash budget doesn't support holding a useful
amount of history. The 5-minute RAM trend buffer (`trend.c`) covers "what
just happened" on the dashboard; it does not survive a reboot and is not a
replacement for durable history. If long-term trending is needed later, it
belongs off-device — pull it over Modbus/TCP into a PLC or historian —
rather than on this flash.

---

## Build

### Firmware

```
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig.defaults` sets the 16 MB flash size, the custom partition
table and OTA rollback. Delete any stale `sdkconfig` before the first
build so the defaults are picked up.

### Host tests

No hardware or ESP-IDF needed:

```
cd host_test
make
```

Covers scaling arithmetic, config validation, the spindle state machine,
alarm delays/hysteresis/latching, sensor-fault isolation, breakage and
crash detection, wear trend, and output mapping.

---

## Layout

```
main/
  board.h          pin map, channel assignment, task/core layout
  app_config.[ch]  config schema, defaults, validation      (pure, tested)
  config_store.[ch] NVS persistence with known-good fallback
  scaling.[ch]     volts -> engineering units                (pure, tested)
  ads1115.[ch]     I2C ADC driver
  analog.[ch]      current burst-RMS + pressure acquisition
  rpm.[ch]         PCNT-based speed measurement
  dio.[ch]         digital I/O, safe state, pulse stretching
  spindle_sm.[ch]  operating state machine                   (pure, tested)
  alarm.[ch]       thresholds and transient detection        (pure, tested)
  do_map.[ch]      alarm state -> output demands             (pure, tested)
  monitor.[ch]     the real-time task
  wifi.[ch]        STA + always-on fallback AP
  modbus.[ch]      Modbus RTU (RS485) and TCP slaves, read-only registers
  calib.[ch]       guided field calibration (2-point pressure, 1-point CT, auto-zero)
  trend.[ch]       5-minute RAM ring buffer for the live graph (not persisted)
  ota.[ch]         browser-upload firmware update into the spare OTA slot
  web.[ch]         HTTP server: dashboard/trend/threshold/calibration/comms/
                   system API, ~15 routes
  web/index.html   the multi-tab operator UI (embedded in firmware)
  main.c           bring-up
host_test/         gcc test harness
```

Anything marked *pure* has no ESP-IDF dependency and takes time as an
argument rather than reading a clock, so it runs deterministically on a
host. That is deliberate: the delay, hysteresis and latch interactions in
`alarm.c` are the kind of logic that looks obviously correct and is not.

---

## Three things to know before trusting this on a machine

### 1. Current accuracy is limited by the ADS1115, not by the code

The board feeds the raw CT burden voltage into an ADS1115 whose maximum
aggregate throughput is 860 SPS — about 17 samples per 50 Hz cycle with a
channel dedicated, and 4.3 with all four active. That is far below what a
single-cycle RMS needs.

The driver compensates by averaging over many cycles. The default
128-sample burst spans ~149 ms and gives roughly 2% error **on a clean
sine wave**. A VFD-driven spindle is not a clean sine wave, and harmonics
above ~430 Hz alias straight into the reading, so real-world accuracy will
be worse.

Two consequences:

- Per-spindle current updates arrive at roughly 3 Hz, not 10 Hz.
- **Breakage detection cannot meet its 100 ms specification.** One burst
  is longer than that window, so detection works burst-to-burst with a
  real window nearer 300–400 ms.

The full trade-off table is in the header comment of `analog.c`. The fix
is external RMS-to-DC conditioning (SRS decision HW-D1); `analog.h` is
already shaped so that swapping the current source does not disturb
anything above it.

### 2. The wear-detection defaults are placeholders

`breakage_drop_pct = 40`, `crash_rise_pct = 60`, `adaptive_k_warn = 3` and
the rest are engineering starting points, not validated values. They must
be tuned against real cutting data during the Phase 6 field trial. Wear
trend detection ships disabled for the same reason.

### 3. The SRS debounce requirement is wrong and the code does not follow it

SRS AI-R11 asks for 0–5 ms of configurable input debounce. The PCNT
glitch filter is clocked from the 80 MHz APB bus with a 10-bit threshold,
so its ceiling is 12.8 µs — three orders of magnitude short.

More importantly, 5 ms of debounce would impose a 200 Hz pulse ceiling,
capping measurable speed at 12 000 RPM with a 1 PPR sensor. The
requirement is wrong for the application, not merely unimplementable.
`rpm.c` clamps to 12 µs and logs when it does. **AI-R11 should be amended.**

---

## Design decisions worth knowing

**Monitoring is armed only in the CUTTING state.** Spindle start-up draws
several times the cutting current and an idle spindle at speed still draws
windage. Evaluating thresholds unconditionally would trip an alarm on
every cycle, and a monitor that cries wolf gets disconnected. This single
rule is the most important false-alarm defence in the product, and it is
why the state machine exists at all.

**A faulted sensor raises a diagnostic, never a process alarm.** A broken
4–20 mA loop reads 0 bar, which is below any sensible LoLo limit. Without
isolation, every wiring fault would present as a critical process alarm.
`alarm_update()` gates each quantity independently on its sensor status.

**Acknowledging an active alarm does not silence it.** It is recorded as
acknowledged, but the latch only releases once the condition has actually
cleared.

**Alarms are evaluated in the measurement task, not a separate one.** A
queue between them would add a scheduling hop inside the 200 ms latency
budget and buy nothing — the evaluation is pure arithmetic on data that
was just produced. The real-time loop is pinned to core 0 so Wi-Fi and
HTTP on core 1 cannot delay it.

**Outputs are held in their safe state until the first complete
measurement cycle**, and the system-healthy output is inverted by default
so that a fault, a broken wire and a dead device all read alike to the
PLC.

---

## Next steps

1. **Fix the pressure burden resistor** (180 Ω → 100 Ω) — see Hardware
   notes. This is the highest-priority open item: it's a fault-detection
   gap on hardware currently on the bench, not a someday cleanup.
2. Cross-check RPM against a tachometer — CT, pressure and RPM are bench-
   verified, but RPM showed minor variation worth confirming independently.
3. Characterise the analogue noise floor **with the VFD running** — this
   is the measurement that decides whether HW-D1 needs resolving before
   anything else proceeds.
4. Decide on authentication for the web UI before it goes on a shop
   network — OTA upload means anyone who can reach the device can reflash
   it, and there is currently no login.
5. The full Material UI SPA (Phase 4) — the current web UI is a hand-
   written multi-tab page, not the dashboard the roadmap describes.

---

## Hardware notes

- **ADS1115 Channel Assignment**:
  - `AIN0` -> Spindle 1 Current (CT1)
  - `AIN1` -> Spindle 1 Pressure (4–20 mA)
  - `AIN2` -> Spindle 2 Current (CT2)
  - `AIN3` -> Spindle 2 Pressure (4–20 mA)
- **CT Current Measurement**:
  - Configured for **30:1** CT ratio (30 A primary / 1 A secondary).
  - Fitted burden resistor: **0.1 Ω** with an opamp gain stage.
  - CT reference bias voltage: nominal **1.65 V**.
- **Pressure Measurement — known issue, not yet resolved**:
  - 4–20 mA pressure loop across a **180 Ω** burden resistor (range 0–250 bar).
    At the full 20 mA scale that is **3.6 V**, which exceeds the ESP32's
    3.3 V supply rail feeding the ADS1115 — the input pin sees more than
    VDD regardless of any PGA/FSR setting.
  - Pressure reads use `ADS_FSR_4096` (±4.096 V), which avoids the ADC
    *digital* code saturating at max-scale, but that only hides the
    symptom: it does not change the pin's absolute voltage limit
    (`ads1115.h` itself documents `ADS_FSR_4096` as "not usable at 3V3").
    A signal genuinely above VDD is an over-voltage condition on the ADC
    input regardless of which range is selected.
  - Consequence: the NAMUR over-range fault check in `scaling.c`
    (`LOOP_OVER_MA = 21.0f`) cannot be trusted near or above this clipping
    point — a shorted transmitter driving max loop current may not read as
    a fault. The reachable top ~10% of the 250 bar range is also suspect.
  - **Fix is a board change, not firmware**: drop the burden to 100 Ω
    (0.4–2.0 V across the 4–20 mA range, 2.1 V at the 21 mA over-range
    trip point — comfortably inside the 3.3 V rail with headroom) and
    revert pressure sampling to `ADS_FSR_2048`, matching the current
    channels.
- DI0/DI1 are GPIO34/35, which are **input-only with no internal
  pull-ups**. External 10 kΩ pull-ups to 3V3 are required on the PCB or
  the pulse counter will pick up noise on a floating pin.
- The 0–10 V divider is 80.6 kΩ / 20 kΩ.
- Modbus RTU is wired to UART2: RXD on GPIO16, TXD on GPIO17, and the
  RS485 transceiver's DE/RE tied together on GPIO4 (driven by the UART's
  RTS line in half-duplex mode). Declared in `board.h`.

---

## Safety

This is a **monitoring and advisory device, not a safety device**. It
carries no SIL or PL rating and must not be the sole protection against
machine damage or injury.
