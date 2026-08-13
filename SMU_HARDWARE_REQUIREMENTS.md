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
dedicated **Spindle Monitoring Units (SMUs)** — one Nuvoton M2003FC1AE per spindle —
with the ESP32 retained as **Master**, owning networking, configuration, the web
UI, and the external PLC/SCADA-facing Modbus interface.

**Chosen part: Nuvoton M2003FC1AE** (Cortex-M23, no TrustZone, 24 MHz, 32 KB
Flash / 4 KB RAM, TSSOP20, 8-ch 12-bit 500 kSPS ADC, 1× I²C, native RS-485
UART). No analogue comparator or DAC — verified against the actual datasheet
(no dedicated ACMP section anywhere in it, and the peripheral summary
explicitly lists only ADC + PWM), not just the product-page summary. Fault
detection is done digitally, in firmware, not via hardware comparator trip.

**Decision driver: supply availability.** M2003FC1AE was confirmed by a
distributor as high-availability; that settles the choice over the
previously-considered alternative below. This is a supply-chain decision, not
a functional one — see the comparison table for the (minor) technical
trade-offs, none of which block any requirement in this document.

**Alternative considered, not selected: Nuvoton M031FB0AE.** Same TSSOP20
footprint and price class, verified against the actual datasheet the same way
M2003FC1AE was:

| | M2003FC1AE (chosen) | M031FB0AE (alternative, not selected) |
|---|---|---|
| Core | Cortex-M23 (no TrustZone), 24 MHz | Cortex-M0, 48 MHz |
| Flash / RAM | 32 KB / 4 KB | 16 KB / 2 KB |
| Package | TSSOP20 | TSSOP20 (same footprint) |
| I/O pins | **18** | 15 |
| ADC | 8-ch, 12-bit, 500 kSPS | 7-ch, 12-bit, 2 MSPS |
| RPM-input peripheral | **3-channel enhanced input capture** — purpose-built for pulse period/frequency measurement | general-purpose 32-bit timer |
| I²C | 1 set (SMU only needs 1) | 2 sets |
| UART | up to 2 + 1 via USCI, native RS-485 (9-bit + auto direction) | 3 sets |
| ACMP / DAC | none (verified) | none (verified against selection guide, not product page) |
| Voltage range | 2.4–5.5 V | 1.8–3.6 V (both cover 3.3 V comfortably) |
| Supply availability | **Confirmed high availability by distributor** — deciding factor | not checked against this criterion |

Every SMU requirement in this document (§3.1–§3.4) is met by either part, so
availability was free to be the deciding factor without trading away any
functional requirement. M2003FC1AE also happens to have two incidental
technical advantages: more I/O headroom (18 vs 15) and a hardware
input-capture peripheral that is a more natural fit for RPM pulse timing than
a general-purpose timer. Neither part has a hardware comparator, so this
choice does not affect the "digital detection only, no ACMP trip" decision
already made above.

Every M031FB0AE reference that previously appeared in this document (§2
diagram, §3.1–§3.6, §5 BOM) has been updated to M2003FC1AE below — the
interface requirements themselves (I²C link, ADC channel count, RPM input, 4
PLC outputs) did not change, only the specific part number did.

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
                    │  M2003FC1AE           │  │  M2003FC1AE           │
                    │                        │  │                        │
                    │  ADC: current, pressure│  │  ADC: current, pressure│
                    │  PCNT/GPIO: RPM pulse  │  │  PCNT/GPIO: RPM pulse  │
                    │  Full 4-band alarm     │  │  Full 4-band alarm     │
                    │  engine + local arming │  │  engine + local arming │
                    │  state machine         │  │  state machine         │
                    │                        │  │                        │
                    │  4× fault/health       │  │  4× fault/health       │
                    │     outputs             │  │     outputs             │
                    │  1× cycle-start/       │  │  1× cycle-start/       │
                    │     fault-clear input  │  │     fault-clear input  │
                    └───────────┬────────────┘  └──────────┬─────────────┘
                                │▲                           │▲
                                ▼│                           ▼│
                        PLC digital I/O                PLC digital I/O
                (4× fault/health inputs,        (4× fault/health inputs,
                 1× cycle-start/fault-clear      1× cycle-start/fault-clear
                 output)                         output)
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
| Cycle start / fault clear from PLC | ❌ Not in the loop | ✅ Direct digital input, no ESP32 in the loop |
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
- **Signals per bus**: SDA, SCL (M2003FC1AE has native I²C hardware, 1 set —
  matches the SMU's single-bus requirement exactly, nothing left unused).
  Pull-ups per standard I²C practice
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

### 3.2 SMU analogue inputs ✅

Per SMU: 1× current channel, 1× pressure channel, both landing on the
M2003FC1AE's internal 12-bit ADC (8 channels available, 2 used).

**ADC input range: 0–3.3 V**, full-scale against the SMU's own 3.3 V rail —
confirmed, resolves the range/biasing open item that previously blocked this
section.

**Current (CT)** — electrical spec unchanged from the current board:
- CT ratio 30 A : 1 A (1000 mA secondary), burden resistor **0.1 Ω**, ×2 op-amp
  gain stage (`gain_correction = 0.506` in firmware — this is a calibration
  constant, not a PCB requirement, but the burden/gain stage sizing must produce
  a signal in the SMU ADC's usable input range).
- DC bias: **1.65 V nominal** (mid-rail of the 0–3.3 V ADC input range),
  centring the AC waveform in the ADC's unipolar input window — confirmed for
  the M2003FC1AE, superseding the earlier "needs re-verification" note (this
  design was originally sized for the ADS1115's ±2.048 V PGA window and has
  now been re-checked against the M2003FC1AE's actual ADC input range instead
  of just carried over).
- No-load cutoff and auto-zero tare are firmware concerns (`calib.c`), not PCB
  requirements.

**Pressure** — dual-mode, jumper-selectable per channel, no DC bias (the
signal is already unipolar, unlike current):
- Two loop types supported: **4–20 mA current loop** and **0–10 V voltage
  loop**, selected per channel via a **hardware jumper / populate option**
  (burden resistor for current mode vs. voltage divider for voltage mode —
  mutually exclusive population, not both live at once) and mirrored in
  firmware as a matching per-spindle pressure-mode setting in the web UI, so
  the scaling math always matches what's actually populated on the board.
- **4–20 mA mode**: burden resistor **100 Ω** (0.4–2.0 V across 4–20 mA,
  2.1 V at the 21 mA over-range trip point — comfortably inside 0–3.3 V with
  headroom). Was 180 Ω on the original ADS1115 board, which produced 3.6 V at
  full scale — an over-voltage condition on the ADC input that also defeated
  the over-range fault check (a shorted transmitter driving max loop current
  wouldn't have read as a fault). This is a fix, not a carry-forward.
- **0–10 V mode**: resistive divider, same `PRESSURE_DIV_R_TOP_OHM` /
  `_BOT_OHM` topology as the existing design (80.6 kΩ/20 kΩ), sized to keep
  the divider output inside 0–3.3 V with headroom at 10 V input.
- Sensor range: 0–250 bar (`sensor_min`/`sensor_max` in firmware config — not
  a PCB constraint beyond the two population options above).

### 3.3 SMU RPM input ✅

- One pulse input per SMU, moved from the ESP32 (previously `PIN_DI0`/`PIN_DI1`,
  GPIO34/35) to the SMU's own GPIO. Needed **locally** on the SMU for correct
  arming (monitoring must only be active while actually cutting — see §2.1) —
  this was the deciding factor for keeping SMUs fully autonomous rather than
  depending on the ESP32 for arming state.
- Carry forward the existing electrical convention: **opto-isolated, active-low
  at the MCU** (field pulse energises the opto, which pulls the MCU pin low).
  If the input pin chosen on the M2003FC1AE has no internal pull-up (check
  M2003FC1AE datasheet per-pin), an **external 10 kΩ pull-up to 3.3 V is
  required** — this bit the original design on the ESP32's input-only
  GPIO34/35 and is exactly the kind of thing worth getting right the first
  time on the new board.
- Speed range: up to ~24,000 RPM at 1 PPR was the original design target
  (`AI-R10` in the original SRS). Land this input on the M2003FC1AE's
  3-channel enhanced input-capture peripheral rather than a general-purpose
  timer (see §1 comparison) — it is purpose-built for pulse period/frequency
  measurement. Whatever glitch-filter/debounce it offers, do not configure it
  above ~12 µs — 5 ms of debounce (the original, since-amended SRS figure)
  would cap measurable speed at 12,000 RPM at 1 PPR, which is wrong for this
  application. This was already litigated once on the ESP32 side (`rpm.c`);
  same physics applies here.

### 3.4 SMU ↔ PLC digital I/O — 4 outputs + 1 control input per SMU (10 signals total) ✅

Per SMU, four direct, PLC-facing outputs plus one PLC-driven control input —
**not** relayed through the ESP32 in either direction:

| Output | Asserted when |
|---|---|
| **Current fault** | Any current-band (Hi/HiHi) violation, OR breakage detection, OR crash detection, OR wear-trend alarm — all current-signature-derived conditions roll into this one line |
| **Pressure fault** | Any pressure-band (LoLo/Lo/Hi/HiHi) violation |
| **RPM fault** | Any RPM-band violation, OR the RPM-sensor-suspect diagnostic (current flowing but no pulses — a broken speed sensor, distinct from "spindle stopped") |
| **SMU healthy** | Normally energised; de-energises if the SMU hangs, resets, fails its own self-check, or loses power — a dedicated status bit, separate from the three fault lines, so the PLC can distinguish "board dead" from "a specific quantity faulted" without having to notice all three fault lines going inactive at once |

One dedicated output per monitored quantity plus one dedicated health line —
this settles the fault-output rollup mapping that was previously an open
item. All four outputs use the fail-safe inversion convention already
established elsewhere in this design (`DO_ACTIVE_LEVEL` semantics in
`board.h`): normally energised, de-energising on the specific condition each
line represents.

**Control input — Cycle Start / Fault Clear**:
- One digital input per SMU, driven by the PLC/operator, carrying two related
  functions: signalling the start of a machining cycle, and clearing/
  acknowledging latched faults.
- Carry forward the same opto-isolated, active-low electrical convention used
  for the RPM input (§3.3) and the ESP32's existing DI pins in `board.h`,
  including the external pull-up caveat if the chosen SMU pin has no internal
  one.
- 🟡 **Firmware-level detail, does not change the hardware requirement**:
  whether this is one signal driving both functions or needs to be
  disambiguated by edge/level/hold-time, and how it interacts with the SMU's
  own arming state machine (§2.1), needs to be defined before SMU firmware is
  written. The hardware requirement is fixed regardless of the answer: one
  more opto-isolated digital input per SMU.

**Electrical**: carry forward the existing convention from `board.h`
(`DO_ACTIVE_LEVEL = 1`, driving an opto or relay output stage — M2003FC1AE
GPIO is 3.3 V logic, PLC inputs are typically 24 V DC, so each output needs
its own isolation/level-shift stage, same as the ESP32's current DO0–DO3; the
new control input needs a matching opto input stage, same convention as the
RPM input). **This requires 4 output stages + 1 input stage per SMU × 2 SMUs
= 8 output stages + 2 input stages total** — the output-stage count matches
the original single-board design (8), with 2 new input stages not previously
accounted for. Confirm current/voltage rating needed against the target
PLC's I/O card spec.

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

- SMUs share the main board's 3.3 V rail (M2003FC1AE operating range 2.4–5.5 V
  — 3.3 V matches the ESP32 rail and keeps I²C level-compatible with no
  shifting needed).
- Each SMU's 3 PLC-facing fault outputs and 1 PLC-facing control input need
  their own isolation-stage supply considerations (opto/relay driver side for
  the outputs, opto input stage for the control input) — same pattern as the
  existing DO/DI stages, adjusted for the new count (see §3.4).

---

## 5. Bill-of-materials additions (from the single-board design)

- 2× Nuvoton **M2003FC1AE** (TSSOP20) — chosen over M031FB0AE on
  distributor-confirmed supply availability, see §1
- 8× opto/relay output driver stages (4 per SMU) — same count as the
  original single-board design, see §3.4
- 2× opto input driver stages (1 per SMU) — new, for the cycle-start/
  fault-clear control input, see §3.4
- I²C pull-up resistors ×2 pairs (one pair per bus)
- Pressure burden resistor: **100 Ω** per channel, for the 4–20 mA population
  option (was 180 Ω — see §3.2, this is a fix, not just a carry-forward)
- Pressure divider resistors (`PRESSURE_DIV_R_TOP_OHM`/`_BOT_OHM`, 80.6 kΩ/
  20 kΩ), for the 0–10 V population option, per channel (§3.2)
- Jumper / 0 Ω populate option per pressure channel, to select between the
  two population options above (§3.2)
- *Removed*: ADS1115 and its support components

---

## 6. Open items — resolve before release to fab

1. 🟡 **Exact GPIO pin assignments** — this document now specifies the full
   per-SMU signal count: 2× ADC (current, pressure) + 1× RPM capture input +
   1× cycle-start/fault-clear input + 2× I²C (SDA/SCL) + 4× DO = **10 signal
   pins per SMU**, comfortably inside the M2003FC1AE's 18 available I/O. Not
   yet mapped to specific pin numbers. `board.h`'s existing numbering scheme
   should be extended to cover the new assignments once layout is underway.
2. 🟡 **SMU non-volatile calibration storage mechanism** — calibration is
   entered through the web UI (same as V1's UX, relayed to the SMU over I²C
   the same way thresholds are — see §3.1), so the *config path* is settled.
   Still open: where the SMU itself persists those values locally across
   power loss (mirroring `config_store.c`'s role on the ESP32). Nuvoton parts
   typically support a reserved Data Flash region for this without an
   external EEPROM; needs confirming against the M2003FC1AE specifically
   during SMU firmware bring-up. Not expected to require an extra part,
   flagged for awareness only.
3. 🟡 **Cycle-start/fault-clear signal semantics** (§3.4) — one input serves
   both "cycle start" and "fault clear/acknowledge." Exact triggering
   behaviour (single signal vs. needing to be split, edge- vs level-
   triggered, interaction with the SMU's local arming state machine) needs
   firmware-side definition before SMU firmware is written. Does not block
   PCB layout — the hardware requirement (one opto-isolated DI per SMU) is
   fixed regardless of the answer.

---

## 7. Explicitly not covered by this document

This document covers the distributed acquisition/fault-signalling architecture
only. It does not replace or backfill the SRS/roadmap files referenced in
`README.md` (`SRS_CNC_Tool_Monitor.md`, `ROADMAP_CNC_Tool_Monitor.md`), which
do not currently exist in this repository — that's a separate, larger
documentation gap, not addressed here.
