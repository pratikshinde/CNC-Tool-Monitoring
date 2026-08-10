# CNC Tool Monitor

Two-spindle CNC tool condition monitor. Measures spindle current (CT),
spindle pressure (4–20 mA / 0–10 V) and spindle speed (pulse input), and
drives alarm outputs to a PLC when a tool wears, breaks or crashes.

**Target:** ESP32 (16 MB flash) · ESP-IDF v6.0.2 · standalone, no cloud

Requirements and plan: [`SRS_CNC_Tool_Monitor.md`](../SRS_CNC_Tool_Monitor.md) ·
[`ROADMAP_CNC_Tool_Monitor.md`](../ROADMAP_CNC_Tool_Monitor.md)

---

## Status

Phase 1–3 core is implemented: configuration, drivers, spindle state
machine, alarm engine and digital output mapping. Networking (WiFi,
Modbus RTU/TCP, a settings + telemetry web page) is implemented. Data
logging, OTA logic and the full Phase 4 SPA are not written yet.

| Area | State |
|---|---|
| Config store with known-good rollback | done |
| ADS1115 / analogue acquisition | done, see accuracy caveat below |
| RPM via PCNT | done |
| Digital I/O with safe-state and pulse stretching | done |
| Spindle state machine | done, host-tested |
| Alarm engine: bands, delays, hysteresis, latching | done, host-tested |
| Breakage / crash / wear-trend detection | done, host-tested, **defaults unvalidated** |
| Digital output mapping | done, host-tested |
| WiFi (station + always-on fallback AP) | done |
| Modbus RTU (RS485) + Modbus TCP, read-only telemetry registers | done |
| Web UI: WiFi/Modbus settings + 1 Hz live readout | done, not the Phase 4 SPA |
| Data logging | not started (Phase 3 remainder) |
| OTA | partition layout ready, logic not started (Phase 7) |

**Compiles clean against ESP-IDF v6.0.2.** `idf.py build` passes with zero
warnings on a full rebuild of the `main` component. Not yet bench-tested
against real hardware — see [Next steps](#next-steps).

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
  web.[ch]         HTTP server: settings API + telemetry API
  web/index.html   the settings + live-readout page (embedded in firmware)
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

1. Bench-verify on real hardware: I2C bring-up, ADS1115 probe, PCNT counts
   against a signal generator, all four DOs into a PLC input, and the RS485
   transceiver against a Modbus RTU master.
2. Characterise the analogue noise floor **with the VFD running** — this
   is the measurement that decides whether HW-D1 needs resolving before
   anything else proceeds.
3. Data logger against the `logs` partition (LG-R1 … LG-R7).
4. The full Material UI SPA (Phase 4) — the current web UI is a
   settings-and-telemetry page, not the dashboard the roadmap describes.

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
- **Pressure Measurement**:
  - 4–20 mA pressure loop across a **180 Ω** burden resistor (range 0–250 bar).
  - Sampled using `ADS_FSR_4096` (±4.096 V range) to support full 0.72 V – 3.60 V input span without ADC saturation.
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
