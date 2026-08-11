# CNC Tool Monitor — Distributed Architecture Hardware Requirements

**For: PCB layout / hardware design**
**Status: DRAFT — settled decisions marked ✅, open items marked 🟡, do not release to fab with 🟡 items unresolved**
**Supersedes: the single-board ADS1115-based analogue front end described in `README.md`'s "Hardware notes"**

---

## 1. Why this document exists

The original design put all analogue acquisition (2× current, 2× pressure) on one
shared ADS1115 ADC, I2C-multiplexed, read by the ESP32. Bench measurement showed
a 423 ms average measurement cycle against a 100–200 ms requirement — the ADS1115's
860 SPS ceiling, shared four ways, is a hard architectural limit, not a tuning
problem.

This redesign moves acquisition, fault detection, and PLC signalling onto two
dedicated **Spindle Monitoring Units (SMUs)** — one Nuvoton M031FB0AE per spindle —
with the ESP32 retained as **Master**, owning networking, configuration, the web
UI, and the external PLC/SCADA-facing Modbus interface.

**Chosen part: Nuvoton M031FB0AE** (Cortex-M0, 16 KB Flash / 2 KB RAM, TSSOP20,
7-ch 12-bit 2 MSPS ADC, 2× I²C, 3× UART). No analogue comparator on this variant —
verified against Nuvoton's own selection guide, not the product page summary.
Fault detection is done digitally, in firmware, not via hardware comparator trip.

---

## 2. System architecture

```
                         ┌─────────────────────────────────────────┐
                         │                 ESP32 (Master)            │
                         │                                            │
   External PLC ◄───────►│  RS485 (Modbus RTU) + WiFi (Modbus TCP)   │
   / SCADA                │  Web UI · Config store · OTA              │
                         │                                            │
                         │  I2C_NUM_0          I2C_NUM_1              │
                         └──────┬──────────────────┬──────────────────┘
                                │                   │
                    (independent bus, no sharing)   │
                                │                   │
                    ┌───────────▼──────────┐  ┌─────▼─────────────────┐
                    │  SMU 1 (Spindle 1)    │  │  SMU 2 (Spindle 2)    │
                    │  M031FB0AE            │  │  M031FB0AE            │
                    │                        │  │                        │
                    │  ADC: current, pressure│  │  ADC: current, pressure│
                    │  PCNT/GPIO: RPM pulse  │  │  PCNT/GPIO: RPM pulse  │
                    │  Full 4-band alarm     │  │  Full 4-band alarm     │
                    │  engine + local arming │  │  engine + local arming │
                    │  state machine         │  │  state machine         │
                    │                        │  │                        │
                    │  4× direct PLC outputs │  │  4× direct PLC outputs │
                    └───────────┬────────────┘  └──────────┬─────────────┘
                                │                            │
                                ▼                            ▼
                        PLC digital inputs           PLC digital inputs
                    (current/pressure/RPM/health) (current/pressure/RPM/health)
```

**Mounting**: both SMUs are daughter cards on the same board as the ESP32 (not
remote at the spindle) — confirmed. Analogue sensor wiring (CT, pressure loop,
RPM pulse) still runs the full cable distance from the spindle to this board;
only the ESP32↔SMU link is short/on-board.

### 2.1 Division of responsibility

| | ESP32 (Master) | SMU (per spindle) |
|---|---|---|
| Networking (WiFi, HTTP, OTA) | ✅ Owns | — |
| Modbus RTU (RS485) + TCP, external-facing | ✅ Owns | — |
| Web UI, config store, threshold/calibration editing | ✅ Owns | — |
| Current / pressure ADC acquisition | — (removed) | ✅ Owns |
| RPM pulse counting | — (removed) | ✅ Owns |
| Spindle arming state machine (CUTTING detection) | — | ✅ Owns, local copy |
| 4-band threshold engine (LoLo/Lo/Hi/HiHi × hysteresis/delay/latch) | — | ✅ Owns, full duplicate of `alarm.c` |
| Breakage / crash / wear-trend detection | — | ✅ Owns |
| **PLC fault signalling** | ❌ Retired — SMUs are the sole PLC signal path | ✅ Direct digital outputs, no ESP32 in the loop |
| Telemetry display, trend history, Modbus register exposure | ✅ Relays what SMU reports | Reports up to ESP32 |

**Critical invariant, carried forward from the existing design philosophy** (see
`board.h`'s existing "outputs held safe until first complete cycle" rule): **the
SMU's PLC outputs are asserted from state computed entirely on the SMU.** They
must not depend on the I2C link to the ESP32, on WiFi, or on the ESP32 being
powered at all. A dead, rebooting, or OTA-updating ESP32 must never be able to
suppress a real fault.

---

## 3. Interface requirements

### 3.1 ESP32 ↔ SMU link — I2C, two independent buses ✅

- **Two separate I2C peripheral instances**, `I2C_NUM_0` and `I2C_NUM_1` on the
  ESP32, one dedicated per SMU. **Not** a shared multi-drop bus — a wedged/glitched
  SMU on one bus must not be able to block telemetry from the other spindle. This
  was a deliberate trade of a few extra GPIO for fault isolation between spindles.
- Standard mode (100 kHz) is more than sufficient — the ESP32 polls each SMU at
  **2 Hz**, master-initiated. This is a telemetry/config channel only; nothing
  safety-relevant crosses it (see §2.1 invariant).
- **Signals per bus**: SDA, SCL (M031 has native I²C hardware, 2 sets @ 1 MHz
  per datasheet — either instance is fine). Pull-ups per standard I²C practice
  (4.7 kΩ typical for short on-board traces at 100 kHz — PCB designer to confirm
  against actual trace length/capacitance).
- **`I2C_NUM_0` is available** once the ADS1115 is removed (it currently occupies
  `PIN_I2C_SDA`/`PIN_I2C_SCL`, GPIO21/22 — see `board.h`). `I2C_NUM_1` pins are
  freely assignable via the ESP32 GPIO matrix.
- **Protocol** (firmware-level, informational for PCB designer — no electrical
  impact): SMU acts as I²C slave exposing a fixed memory-mapped register block.
  ESP32 writes a register pointer then reads the telemetry struct each 2 Hz poll;
  config/threshold/calibration writes are event-driven. CRC8 both directions,
  reject-and-keep-last-known-good on mismatch — mirrors the validate-before-apply
  pattern already used in `config_store_commit()`.

### 3.2 SMU analogue inputs — carried forward from the existing front end, one fix required 🟡

Per SMU: 1× current channel, 1× pressure channel, both landing on the M031's
internal 12-bit ADC (7 channels available, 2 used).

**Current (CT)** — electrical spec unchanged from the current board:
- CT ratio 30 A : 1 A (1000 mA secondary), burden resistor **0.1 Ω**, ×2 op-amp
  gain stage (`gain_correction = 0.506` in firmware — this is a calibration
  constant, not a PCB requirement, but the burden/gain stage sizing must produce
  a signal in the SMU ADC's usable input range).
- DC bias: **1.65 V nominal**, centring the AC waveform in the ADC's unipolar
  input window. Confirm this still centres correctly against the M031's ADC
  reference/range (design was originally sized for the ADS1115's ±2.048 V PGA
  window at 3.3 V supply — needs re-verification against the M031's actual ADC
  input characteristics, not just copied over).
- No-load cutoff and auto-zero tare are firmware concerns (`calib.c`), not PCB
  requirements.

**Pressure — 🟡 known defect, fix during this respin, do not carry forward as-is:**
- Current burden resistor is **180 Ω**. At the full 20 mA loop scale that's
  **3.6 V**, which exceeds the 3.3 V rail feeding the ADC — an over-voltage
  condition on the ADC input pin regardless of PGA/reference setting, and it
  defeats the over-range fault check (a shorted transmitter driving max loop
  current may not read as a fault). This was flagged as an open defect against
  the ADS1115 design and was never fixed on that board.
  **For the SMU redesign: drop the burden to 100 Ω** (0.4–2.0 V across 4–20 mA,
  2.1 V at the 21 mA over-range trip point — comfortably inside 3.3 V with
  headroom).
- Sensor range: 0–250 bar over the 4–20 mA loop (`sensor_min`/`sensor_max` in
  firmware config — not a PCB constraint beyond the burden resistor above).
- If a 0–10 V loop variant is ever fitted (see `PRESSURE_DIV_R_TOP_OHM` /
  `_BOT_OHM` = 80.6 kΓ/20 kΩ in the current design), the same 3.3 V input
  ceiling applies to the divider output — carry the same headroom-below-VDD
  principle forward if this option is kept on the SMU.

### 3.3 SMU RPM input ✅

- One pulse input per SMU, moved from the ESP32 (previously `PIN_DI0`/`PIN_DI1`,
  GPIO34/35) to the SMU's own GPIO. Needed **locally** on the SMU for correct
  arming (monitoring must only be active while actually cutting — see §2.1) —
  this was the deciding factor for keeping SMUs fully autonomous rather than
  depending on the ESP32 for arming state.
- Carry forward the existing electrical convention: **opto-isolated, active-low
  at the MCU** (field pulse energises the opto, which pulls the MCU pin low).
  If the input pin chosen on the M031 has no internal pull-up (check M031
  datasheet per-pin), an **external 10 kΩ pull-up to 3.3 V is required** — this
  bit the original design on the ESP32's input-only GPIO34/35 and is exactly
  the kind of thing worth getting right the first time on the new board.
- Speed range: up to ~24,000 RPM at 1 PPR was the original design target
  (`AI-R10` in the original SRS). Whatever glitch-filter/debounce the M031's
  GPIO or timer peripheral offers, do not configure it above ~12 µs — 5 ms of
  debounce (the original, since-amended SRS figure) would cap measurable speed
  at 12,000 RPM at 1 PPR, which is wrong for this application. This was already
  litigated once on the ESP32 side (`rpm.c`); same physics applies here.

### 3.4 SMU → PLC digital outputs ✅ (4 per SMU, 8 total)

Per SMU, four direct, PLC-facing outputs — **not** relayed through the ESP32:

| Output | Asserted when |
|---|---|
| **Current fault** | Any current-band (Hi/HiHi) violation, OR breakage detection, OR crash detection, OR wear-trend alarm — all current-signature-derived conditions roll into this one line |
| **Pressure fault** | Any pressure-band (LoLo/Lo/Hi/HiHi) violation |
| **RPM anomaly** | Any RPM-band violation, OR the RPM-sensor-suspect diagnostic (current flowing but no pulses — a broken speed sensor, distinct from "spindle stopped") |
| **SMU healthy** | Normally energised; de-energises if the SMU hangs, resets, fails its own self-check, or loses power — same fail-safe inversion convention as the existing system-healthy output, so a dead board reads the same as a real fault to the PLC |

**Electrical**: carry forward the existing convention from `board.h`
(`DO_ACTIVE_LEVEL = 1`, driving an opto or relay output stage — M031 GPIO is
3.3 V logic, PLC inputs are typically 24 V DC, so each output needs its own
isolation/level-shift stage, same as the ESP32's current DO0–DO3). **This
requires 4 output stages per SMU × 2 SMUs = 8 total** — up from 4 on the
original single-board design. Confirm current/voltage rating needed against
the target PLC's input card spec.

🟡 **Open, needs firmware-side confirmation before layout is finalised**: the
exact rollup rule above (which alarm-engine severities feed which output) is
my proposed mapping based on the existing `alarm_state_t.bands[quantity][band]`
structure, which already tracks state per-quantity — implementable without
restructuring the alarm engine. Confirm this matches intent before firmware
work starts on the SMU side; it doesn't block PCB layout (4 output stages
either way) but should be confirmed before the SMU firmware is written.

### 3.5 ESP32 ↔ PLC — unchanged ✅

RS485 (Modbus RTU) + WiFi (Modbus TCP), as already implemented — carried
forward as-is. `MB_UART_PORT_NUM` (UART2), RXD/TXD/DE-RE pins per existing
`board.h`. This remains the path for **telemetry** (register-mapped current/
pressure/RPM/severity/health, read-only) and **configuration** (web UI). It is
explicitly **not** a fault-signalling path any more — see §2.1.

### 3.6 Retired from this design ✅

- **ADS1115** and its I2C bus wiring — removed entirely. `ads1115.c`/`.h` and
  the I2C acquisition path in `analog.c` are dead code once the SMU migration
  lands.
- **ESP32's direct RPM inputs** (`PIN_DI0`/`PIN_DI1`, GPIO34/35, `rpm.c`'s PCNT
  driver) — retired. RPM is reported to the ESP32 as part of SMU telemetry over
  I2C, not read directly.
- **ESP32's DO0–DO3** (`dio.c`, `do_map.c`) — retired per §2.1. These GPIO
  (originally GPIO25/26/27/14) are free for reuse or removal on the next board
  rev.

---

## 4. Power

- SMUs share the main board's 3.3 V rail (M031 operating range 1.8–3.6 V — 3.3 V
  matches the ESP32 rail and keeps I²C level-compatible with no shifting needed).
- Each SMU's 4 PLC-facing outputs need their own isolation-stage supply
  considerations (opto/relay driver side) — same pattern as the existing DO
  stages, just ×2 the count.

---

## 5. Bill-of-materials additions (from the single-board design)

- 2× Nuvoton M031FB0AE (TSSOP20)
- 8× opto/relay output driver stages (4 per SMU) — was 4 total, now 8
- I²C pull-up resistors ×2 pairs (one pair per bus)
- Pressure burden resistor: **100 Ω** per channel (was 180 Ω — see §3.2, this
  is a fix, not just a carry-forward)
- *Removed*: ADS1115 and its support components

---

## 6. Open items — resolve before release to fab

1. 🟡 **Current-channel ADC input range/biasing on the M031** — the 1.65 V bias
   and gain stage were originally sized for the ADS1115's PGA characteristics.
   Needs re-verification against the M031's actual ADC input spec, not assumed
   to transfer directly.
2. 🟡 **Exact GPIO pin assignments** — this document specifies functional
   requirements (2× I²C bus pairs on ESP32, 1× ADC + 1× RPM + I²C + 4× DO per
   SMU) but not final pin numbers. `board.h`'s existing numbering scheme should
   be extended to cover the new assignments once layout is underway.
3. 🟡 **SMU non-volatile calibration storage** — the SMU needs somewhere to
   persist its own threshold/calibration values across power loss (mirroring
   `config_store.c`'s role on the ESP32). M031 parts typically support a
   reserved Data Flash region for this without an external EEPROM; needs
   confirming during SMU firmware bring-up. Not expected to require an extra
   part, flagged for awareness only.
4. 🟡 **Fault-output rollup mapping** (§3.4) — confirm before SMU firmware
   starts, does not block PCB layout.

---

## 7. Explicitly not covered by this document

This document covers the distributed acquisition/fault-signalling architecture
only. It does not replace or backfill the SRS/roadmap files referenced in
`README.md` (`SRS_CNC_Tool_Monitor.md`, `ROADMAP_CNC_Tool_Monitor.md`), which
do not currently exist in this repository — that's a separate, larger
documentation gap, not addressed here.
