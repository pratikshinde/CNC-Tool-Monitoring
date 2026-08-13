# CNC Tool Monitor V2 — Firmware Design Specification

**Status: DRAFT — settled decisions marked ✅, open items marked 🟡**
**Companion to: `SMU_HARDWARE_REQUIREMENTS.md` (hardware/electrical), which this document assumes and does not repeat**
**Applies to: branch `hardware/smu-distributed-architecture`. V1 firmware (`main`, tagged `v1.0.0`) is the baseline being migrated from.**

---

## 1. Scope and design intent

V2 splits one firmware image into two, across three MCUs:

| Image | Runs on | Count | Responsibility |
|---|---|---|---|
| **SMU firmware** | Nuvoton M2003FC1AE | 2 (one per spindle) | Acquisition, alarm evaluation, PLC fault signalling — the entire safety path |
| **Master firmware** | ESP32 | 1 | Networking, web UI, config authority, Modbus, OTA — no safety path |

**The governing requirement for V2 is stated plainly so every later decision can be checked against it: this is a high-reliability industrial monitoring system.** V1 was deliberately simplified for one customer (Lo/Hi bands only, fixed hysteresis and delays, no latching, everything auto-clearing). V2 does not inherit those simplifications. The full four-band engine with configurable hysteresis, delays, and per-band latching returns — see §4.3.

Two consequences follow from the split, and both are load-bearing:

1. **The safety path fits entirely inside one SMU.** Acquisition, arming, threshold evaluation, and output assertion all happen on the same MCU, with no network, no I²C transaction, and no other processor in the loop. This is the §2.1 autonomy invariant from the hardware document, restated as a firmware constraint: *no code path that asserts or clears a PLC fault output may block on, or read state produced by, the ESP32.*
2. **The ESP32 keeps configuration authority but loses runtime authority.** It decides what the thresholds *are*; it has no say in whether a given sample violates them.

### 1.1 Data retention — the device is a monitor, not a historian ✅

**No historical data is persisted. All runtime data is volatile and is lost on
power loss, by design.** Customers who need history log it externally over
Modbus RTU (RS-485) or Modbus TCP (WiFi), both of which are available
simultaneously (§5.3).

What this means precisely:

| Data | Persisted? | Where |
|---|---|---|
| Calibration, thresholds, band config, network settings | ✅ Yes | ESP32 NVS (canonical) + SMU Data Flash (autonomous copy) |
| Measurements, alarm state, latches | ❌ No | RAM |
| Cycle statistics, counters, rolling windows | ❌ No | RAM, zeroed at power-on |
| Learned wear baseline | ❌ No | RAM, relearned after power-on |
| Event, alarm and trend logs | ❌ Not kept at all | — |

Configuration persists because it *is* configuration — losing calibration
would leave the device measuring wrongly, which §3.2's whole mode-sense
argument exists to prevent. Everything the device *observes* is volatile.

Three consequences, stated so nobody is surprised by them later:

- **Counters are "since power-on", not lifetime.** `CYCLE_COUNT`,
  `TOTAL_CUTTING_S`, `ALARM_COUNT_TOTAL` and friends restart at zero on every
  power cycle. They are not an MTBF or maintenance-scheduling record on their
  own; they become one only once SCADA is logging and accumulating them.
- **The wear baseline relearns after every power cycle**, so
  `DEVIATION_SIGMA` — the strongest predictive signal in the Modbus map — is
  unavailable until enough cycles have run to re-establish it. On a machine
  powered down nightly, the baseline may rarely mature. **This is the one
  place where the no-persistence rule has real cost**, and it is worth
  revisiting if predictive maintenance becomes a headline feature: the
  baseline is arguably a learned calibration constant rather than history,
  and persisting just that one struct would be defensible without reopening
  the policy. Flagged, not assumed — the rule as stated is implemented.
- **A fault that occurs and clears during a power interruption leaves no
  trace.** The latch is in RAM. This is acceptable because the hard-wired
  outputs are fail-safe: a board that loses power de-energises every output,
  and the PLC sees that as a fault regardless.

This is a deliberate simplification and a defensible one — it keeps flash
wear bounded, removes a whole class of storage-corruption failure modes, and
puts historical data where the site's own retention, backup, and reporting
policies already apply.

---

## 2. What carries over from V1, and what does not

The V1 codebase was written with a deliberate split between pure C modules (no ESP-IDF includes, host-testable) and platform modules. That split is what makes this migration tractable — the safety-critical logic is already portable and already has tests.

### 2.1 Ports to the SMU essentially unchanged ✅

These compile against the M2003FC1AE toolchain with no ESP-IDF dependency, and keep their existing `host_test/` coverage:

| Module | Role on the SMU | Notes |
|---|---|---|
| `alarm.[ch]` | Full four-band engine, transient detection, severity resolution | Time is already a parameter, never read from a clock — see §4.1 for the one required change (time base width) |
| `spindle_sm.[ch]` | Arming state machine (CUTTING detection) | The single most important false-alarm defence in the product; must run locally per §1 |
| `scaling.[ch]` | ADC counts → engineering units | |
| `app_config.[ch]` | Config schema + validation | Split, see §5.2 — the SMU needs the per-spindle subset, not WiFi/Modbus |

**This branch is the right starting point.** It forked before V1's simplification pass, so `alarm.h` here still carries `breakage_latched` / `crash_latched` and the full `band_cfg_t` (hysteresis, on/off delay, latching). V1's later removal of those fields is *not* to be cherry-picked forward.

### 2.2 Rewritten for the SMU

| V1 module | V2 replacement | Why |
|---|---|---|
| `analog.c` + `ads1115.[ch]` | `smu_adc.c` | The ADS1115 is gone; acquisition moves to the M2003FC1AE's internal 12-bit ADC. The RMS burst *algorithm* carries over, the transport does not. |
| `rpm.c` (ESP32 PCNT) | `smu_rpm.c` | Retargeted onto the M2003FC1AE's 3-channel enhanced input capture peripheral |
| `dio.c` | `smu_dio.c` | Same logic — fail-safe inversion, minimum-pulse stretching — on SMU GPIO |
| `do_map.c` | folded into `smu_outputs.c` | The V2 mapping is fixed (one output per quantity + health line), not a configurable source matrix. Much simpler than V1's `do_source_t` switch. |
| `config_store.c` (NVS) | `smu_store.c` (Data Flash) | Same validate-before-commit and known-good-fallback pattern, different medium |

### 2.3 Retired from the ESP32 ✅

`ads1115.[ch]`, `analog.c`, `rpm.c`, `dio.c`, `do_map.c` are deleted from the master image, along with their `board.h` pin definitions. The ESP32 no longer touches a sensor or an output.

### 2.4 Unchanged on the ESP32 ✅

`wifi.c`, `web.c` + `web/`, `modbus.c`, `ota.c`, `config_store.c`, `trend.c`, `calib.c`. These need new *data sources* (SMU telemetry instead of local acquisition) but not redesign.

---

## 3. SMU firmware

### 3.1 Execution model — bare-metal superloop ✅

No RTOS. One deterministic superloop plus a small number of interrupt handlers. With 4 KB of RAM and a single well-defined job, an RTOS would add scheduling non-determinism and stack overhead in exchange for nothing.

```
  reset
    └─> hardware init, self-check
        └─> load calibration from Data Flash (fallback: safe defaults, health output stays de-energised)
            └─> superloop, nominal 50 ms period:

                  1. read RPM          (from capture ISR accumulator)
                  2. read pressure     (single-shot ADC)
                  3. read current      (RMS burst, whole mains cycles)
                  4. read machine-running / fault-clear inputs
                  5. advance arming state machine
                  6. evaluate alarms
                  7. drive fault outputs
                  8. service I²C telemetry buffer (non-blocking)
                  9. kick watchdog
```

Steps 1–7 are the safety path and never block. Step 8 only publishes into a buffer the I²C ISR reads — it never waits for the ESP32.

#### Latency budget ✅

The requirement is **detect a fault within 50 ms, report it within 100 ms**.
Those are two different clocks and they decompose cleanly:

| Stage | Budget | Mechanism |
|---|---|---|
| **Detect** — fault present → SMU output asserted | ≤ 50 ms | 50 ms superloop; steps 1–7 run to completion every pass |
| **Report** — fault present → visible in Modbus registers | ≤ 100 ms | 50 ms detect + 50 ms worst-case ESP32 poll latency |

**SMU loop period: 50 ms nominal (20 Hz)** ✅. A fault appearing just after a
sample is caught on the next pass, so detection latency is 0–50 ms worst case,
and the hard-wired PLC output asserts within that same pass. This is the
payoff for the whole redesign: V1 measured 423 ms because four channels shared
one 860 SPS ADS1115. The M2003FC1AE's 500 kSPS ADC serving two channels on a
dedicated core removes that ceiling entirely.

**ESP32 poll period: 50 ms (20 Hz), raised from the 2 Hz in the hardware
document** ✅. The report budget is the *sum* of both stages — a fault detected
just after a poll waits a full poll interval to be seen — so 50 ms detect plus
50 ms poll lands exactly on the 100 ms requirement.

Bus loading is not a concern: a ~64-byte telemetry read at 100 kHz takes about
7 ms, so 20 Hz is roughly 14% utilisation on each of the two independent
buses. 400 kHz is available if more headroom is ever wanted.

🟡 **To confirm on bring-up**: that steps 1–7 complete comfortably inside
50 ms at 24 MHz, particularly the RMS burst arithmetic. The M23 has no FPU, so
`float` maths is soft-float — see §4.2. Note the burst itself must span whole
mains cycles (20 ms at 50 Hz), so 50 ms leaves 30 ms for everything else; if
that proves tight the burst can drop to a single mains cycle before the loop
period has to move.

### 3.2 Acquisition

**Current (RMS burst)** — algorithm carried over from `analog.c`:
- The burst must span a **whole number of mains cycles** (20 ms at 50 Hz, 16.67 ms at 60 Hz) or the RMS reads a beat frequency against the mains rather than a stable value. `mains_hz` stays a config field.
- Sample rate is set so `rms_burst_samples` lands exactly on that window. At 128 samples over one 50 Hz cycle that is 6.4 kSPS — trivially inside 500 kSPS, unlike V1 where this was the binding constraint.
- Bias removal, `gain_correction`, `zero_offset_v`, and `noload_cutoff_a` all behave exactly as in V1. The 1.65 V hardware bias is subtracted in firmware; the auto-zero tare refreshes `zero_offset_v`.

**Pressure** — single-shot conversion, no burst needed (the signal is DC).
- Dual-mode per §3.2 of the hardware document: `PRESSURE_INPUT_4_20MA` or `PRESSURE_INPUT_0_10V`, already an enum in `app_config.h`.
- ✅ **Mode mismatch is guarded by a sense pin — adopted.** The mode is selected twice and
  independently: by a *hardware jumper* (which population is live) and by a
  *firmware setting* (which transfer function is applied). Nothing couples
  them, so they can disagree, and when they do the firmware applies the wrong
  scale factor to a perfectly healthy signal. Worked example: in 4–20 mA mode
  a 4 mA loop across the 100 Ω burden gives 0.4 V, and firmware maps
  0.4–2.0 V onto 0–250 bar. If the board is jumpered that way but firmware is
  set to 0–10 V, it instead expects the divider (10 V → ~1.98 V) and maps
  0–1.98 V onto the same span — so a genuine zero-pressure reading displays as
  roughly 50 bar.

  **What makes this the worst class of fault is that every existing diagnostic
  passes.** The loop is not broken, the current is in range, the ADC is
  healthy, and no band is violated at the wrong-but-plausible value. There is
  no fault to report, so nothing is reported: the operator sees a number, and
  the number is wrong. It is also the one thing the firmware cannot detect
  about itself without help.

  **Mitigation — adopted**: the jumper, in addition to selecting the analogue
  path, ties a **sense pin** high or low. Firmware reads it every loop,
  compares against the configured mode, and on mismatch **refuses to report
  pressure at all** — raises a diagnostic, marks the pressure value invalid
  over Modbus, and de-energises the health output. It costs one GPIO and
  converts a silent measurement error into a loud, specific one.

  The check runs continuously, not only at boot: a jumper moved during
  maintenance with the board powered is exactly the scenario this is for.

  This matters more in V2 than it did in V1: V1 had a single fixed
  configuration soldered down, whereas V2 introduces a field-selectable option
  on two independent channels — and field-selectable options get mis-set in
  the field.
- Loop-fault detection carries over: below ~3.5 mA is a broken loop, above ~21 mA is a short. Both map to `SENSOR_FAULT`, not to a pressure value.

**RPM** — input capture, replacing V1's PCNT counting:
- Measure *period between edges* rather than counting edges per window. Period measurement gives a usable reading from a single revolution, where counting needs a full window to elapse — meaningfully faster to detect a stall.
- `zero_timeout_ms` still governs the stopped decision: no edge for that long means RPM 0.
- Glitch filter ≤ ~12 µs per the hardware document's §3.3 reasoning; do not let this creep upward.

### 3.3 Machine Running and Fault Clear inputs ✅

**Two separate opto-isolated inputs** — Machine Running on pin 19, Fault Clear
on pin 20 (§3.7).

| Input | Pin | Type | Configurable |
|---|---|---|---|
| **Machine Running** | 19 | **Level** — asserted continuously while cutting, de-asserted when idle | Enable/disable per spindle in web UI |
| **Fault Clear** | 20 | Pulse, nominal 1 s | Always active, not disableable |

Both get a **50 ms debounce** for noise immunity on a 24 V field wire. Because
each function has its own line, there is no pulse-width discrimination, no
dead band, and no timing ambiguity between them — an earlier draft needed all
three and none of it survives.

#### Machine Running ✅

A level signal stating machine state directly. This is a materially better
signal than the cycle-start pulse it replaces: **there is nothing left to
infer.** The earlier design had to reconstruct end-of-cycle from the
current/RPM decay signature, and get that reconstruction right in the presence
of tool breakage, which produces a near-identical current drop. All of that
machinery is deleted. The wire says what the machine is doing.

**It gates arming; it does not force it.** When enabled, monitoring arms only
when Machine Running is asserted **and** the local state machine independently
detects CUTTING — an AND, never an override:

```
monitoring_armed  =  (machine_running || !machine_running_enabled)
                  &&  spindle_sm.state == CUTTING
                  &&  inhibit window expired
```

Keeping the AND matters even though the signal is now authoritative. A machine
asserting Machine Running during a dry run, an air cut, or a probing move
would otherwise arm monitoring against a cut that is not happening. Local
detection is the second opinion. When disabled, arming falls back to
CUTTING-detection alone, which is V1's behaviour.

**Cycle boundaries come from this signal** when it is enabled. Each
asserted→de-asserted transition closes one cycle for the statistics in
`MODBUS_REGISTER_MAP.md` §7. That is more accurate than the inferred CUTTING
episodes V1 used, and it means per-cycle wear trending is aligned with what
the machine considers a cut rather than with a current threshold.

#### Disarm ordering — the one edge case worth stating ✅

Within a loop pass, **alarms are evaluated before the Machine Running edge is
processed.** So the final armed evaluation uses the last genuine cutting
sample, and a fault present in that sample is caught rather than lost to the
disarm.

Arming is deliberately **not** extended past the de-assert edge. A transient
detector still accumulating evidence when the machine stops may therefore fail
to confirm — a breakage in roughly the last `breakage_window_ms` before the
stop can be missed. That is accepted knowingly, because the alternative is
worse: keeping the detectors live across the stop would present every normal
end-of-cut current drop to a breakage detector looking for exactly that shape,
and manufacture a false breakage on every single cycle. V1's whole lesson was
that a monitor which cries wolf gets disconnected within a week.

The residual risk is small in practice: the CNC controller has no knowledge of
tool breakage — detecting it is this device's job — so it stops at programmed
end, not in reaction to a break. The overlap case requires an operator hitting
e-stop within a couple hundred milliseconds of a breakage.

#### New cross-checks this signal enables ✅

Having an authoritative external statement of machine state makes two new
plausibility checks possible, both in the spirit of the existing
RPM-sensor-suspect logic (§3.4):

- **Machine Running asserted, but no current and no RPM locally.** The machine
  says it is cutting and the instrument sees nothing. Either the CT or RPM
  sensor has failed, or the signal is miswired. Raised as a diagnostic — it
  must not pass silently, because in this state the device is armed but blind.
- **Machine Running de-asserted, but real cutting current present.** The
  instrument sees a cut the machine is not admitting to. Signal miswired,
  inverted, or the spindle is being run outside the program. Also a
  diagnostic; monitoring stays disarmed (the AND holds), which is why the
  operator needs telling.

A **stuck-asserted** check falls out of the same reasoning: Machine Running
held continuously for far longer than any plausible cut (configurable, default
hours) indicates a wiring or PLC fault rather than a very long operation.

#### Fault Clear ✅

Nominal **1 s pulse**. The action fires once the debounced input has been
continuously asserted for 1000 ms, then one-shots — holding longer does
nothing further, and the input must be released before it can fire again.
Assertions shorter than 1000 ms are rejected as noise.

Semantics are the existing `alarm_acknowledge()` behaviour: it clears latched
conditions that are **no longer physically present**, and leaves
latched-and-still-true conditions asserted. Acknowledging an active alarm
silences the record, not the reality — pressing it against a live fault does
nothing, by design, and a clear that "did not work" is almost always a
still-present condition.

Never disableable: an operator at the panel must always be able to clear a
cleared fault.

**Latched faults survive disarm.** When Machine Running de-asserts, monitoring
disarms but latched conditions stay latched and their outputs stay asserted —
per the rule already documented in `alarm.h`, disarming must not silently
clear a latch an operator has not yet seen. Clearing is this input's job, or
the web UI's, or Modbus's; it is never a side effect of the machine stopping.

### 3.4 Fault outputs ✅

Four outputs, mapping fixed in firmware (no configurable source matrix — that was V1's `do_map.c`, and V2 does not need it):

| Output | Driven by |
|---|---|
| Current fault | any `bands[QTY_CURRENT][*]` active/latched, OR `breakage`, OR `crash`, OR `trend` |
| Pressure fault | any `bands[QTY_PRESSURE][*]` active/latched |
| RPM fault | any `bands[QTY_RPM][*]` active/latched, OR the RPM-sensor-suspect diagnostic |
| SMU healthy | de-energised on: watchdog reset, failed self-check, Data Flash CRC failure, ADC not responding, or loss of power |

Carried over from `dio.c` unchanged:
- **Fail-safe inversion**: all four are normally energised, de-energising to signal. A dead board and a real fault look identical to the PLC, which is the point.
- **Minimum pulse stretching** (`min_pulse_ms`, 3000 ms in V1's shipped config): once asserted, an output stays asserted for at least this long even if the condition clears sooner, so a PLC scan cannot step over a brief event.
- **Outputs held safe until the first complete measurement cycle**, so a garbage first reading can never pulse the PLC.

**RPM-sensor-suspect** is the cross-check from V1's `monitor.c` and it must survive the port: real cutting current with the RPM input reading zero means the *speed sensor* has failed, not that the spindle is stopped. Treating it as "stopped" would silently disarm all monitoring — everything looks normal while nothing is being watched. It is a diagnostic, and it drives the RPM fault output.

### 3.5 Calibration storage — Data Flash ✅

Per the hardware document, calibration is stored redundantly: the ESP32's NVS holds the canonical copy, the SMU holds its own working copy so it stays correct through link loss or ESP32 downtime.

`smu_store.c` mirrors `config_store.c`'s proven pattern:
- Two slots, written alternately. A write that is interrupted by power loss leaves the *other* slot intact.
- Each slot: schema version + payload + CRC32. Load validates version and CRC before accepting.
- On both slots failing: fall back to compiled-in safe defaults **and de-energise the health output**. Running on unknown calibration without saying so would be exactly the silent-wrongness failure this design exists to prevent.
- Writes are event-driven (an ESP32 config push), never periodic — Data Flash has limited endurance and calibration changes at commissioning, not continuously.

#### Flash budget — settled against the datasheet and TRM ✅

**There is no separately-configurable Data Flash region on this part**, and no
`DFBA` base-address register (unlike the M031 family, where one exists). The
TRM describes the 32 KB as "Application ROM **with** Data Flash": data storage
is simply APROM pages that the application reserves in its linker script and
writes at runtime through the IAP registers (`FMC_ISPCMD` / `ISPADDR` /
`ISPDAT` / `ISPTRG`). Nothing needs configuring — it needs *reserving*.

| Fact | Value | Source |
|---|---|---|
| APROM | 32 KB | DS §6.4.2 |
| LDROM (ISP loader) | 4 KB, separate — does not consume APROM | DS §6.4.2 |
| **Page erase granularity** | **512 B, all embedded flash** | DS §6.4.2, TRM §6.4 |
| Endurance | 100,000 cycles (guaranteed by design) | DS §9.3 |
| Retention after 100 k cycles | 50 yr @ 55 °C, 25 yr @ 85 °C, 10 yr @ 105 °C | DS §9.3 |

The 512 B page is the number that decides the budget, not the 296-byte struct:
flash erases a whole page at a time, so a slot costs one page regardless of
how little of it is used.

```
    smu_config_t          296 B   ->  fits one 512 B page (216 B spare)
    two slots (A / B)   2 pages   =  1 KB reserved at the top of APROM
    remaining for code           ~31 KB of 32 KB  (3.1% consumed)
```

**The 512 B page also validates the two-slot design rather than merely
permitting it.** Erase is destructive and page-granular, so a single-slot
store has a window — between erase and successful rewrite — in which no valid
calibration exists anywhere on the SMU. Losing power inside that window would
leave the board to come up on defaults. Alternating pages means the previous
good copy is never erased until the new one is written and verified.

Endurance is a non-issue at four orders of magnitude of headroom: writes
happen at commissioning and on operator config changes, realistically tens to
low hundreds over the product's life, against 100,000 cycles.

🟡 **What remains is a build-time question, not a bring-up measurement**: the
SMU firmware must fit in the ~31 KB left after reserving the two pages. That
is answered by the first real link, not by probing silicon.

### 3.6 Self-check and watchdog ✅

- Hardware watchdog enabled, kicked once per superloop only. Never kicked from an ISR — a watchdog that a stuck main loop can still satisfy is decoration.
- Power-on self-check: Data Flash CRC, ADC responds and reads plausibly, config validates. Any failure → health output de-energised, fault reported in telemetry, superloop still runs so the ESP32 can read *why*.
- Watchdog reset is recorded in a reset-cause field exposed over I²C, so repeated resets are visible from the web UI rather than being invisible.

### 3.7 SMU pin assignment ✅

Verified against the M2003 Series datasheet (Rev 1.00, Apr 2024) §4.1. Full
map is in `SMU_HARDWARE_REQUIREMENTS.md` §6.1; the firmware-relevant
peripheral bindings are:

| Pin | Port | Signal | Peripheral |
|---|---|---|---|
| 1 | PB.1 | RPM pulse | `ECAP0_IC0` — enhanced input capture |
| 2 | PB.2 | Current (CT) | `ADC0_CH2` |
| 3 | PB.3 | Pressure | `ADC0_CH3` |
| 12 | PB.14 | Debug UART RX | `UART0_RXD` |
| 13 | PB.13 | Debug UART TX | `UART0_TXD` |
| 16 | PB.8 | I²C SDA | `I2C0_SDA` |
| 17 | PB.9 | I²C SCL | `I2C0_SCL` |
| 5, 6, 14, 15 | PB.4/5/12/7 | Fault outputs (current, pressure, RPM, healthy) | GPIO |
| 11 | PB.15 | Pressure mode sense | GPIO input |
| 19, 20 | PB.11, PB.0 | Machine Running, Fault Clear | GPIO input |

Reserved: pin 4 nRESET, 7 VSS, 8 ICE_DAT, 9 VDD, 18 ICE_CLK. Pin 10 spare.

Two notes that matter for firmware bring-up:

- **The debug UART is on pins 12/13, not 10/11 as first proposed.** Pin 11
  (PB.15) has no `UART0_RXD` at all, and pin 10 (PC.14) is contradicted
  between the datasheet's diagrams and its tables. See the hardware document
  for the full reasoning.
- **`ECAP0_IC0` on pin 1 is the only input-capture channel available.**
  `ECAP0_IC1` and `IC2` exist only on pins 2 and 3, which are committed to
  the two ADC channels. There is no second capture input to fall back on, so
  the RPM path has no spare — worth knowing before assuming one exists.

### 3.8 Wear baseline learning ✅

`wear_cfg_t` has carried `adaptive_k_warn = 3.0` and `adaptive_k_alarm = 5.0`
since V1, and no code has ever read them. `trend_enabled` defaults to `false`
with the comment *"needs a learned baseline first."* This section supplies the
missing piece.

The detector answers a different question from the threshold bands. Bands ask
*"is this cut outside its absolute limits?"* — a safety question. The baseline
asks *"is this cut different from how this job normally runs?"* — a wear
question, and it can be answered long before any absolute limit is approached.

#### The statistic ✅

**Per-cycle mean current**, which `spindle_sm_take_cycle()` already produces
as `cycle_summary_t.mean_current`. One value per completed cut. Mean rather
than peak because peak is dominated by entry transients and is far noisier;
mean integrates the whole cut and is where blunting shows up as steadily
rising cutting force.

#### The estimator: median and MAD, not mean and standard deviation ✅

Collect `baseline_learn_cycles` admitted cycle means into a ring buffer, then:

```
    med     = median(samples)
    MAD     = median(|samples[i] - med|)
    sigma   = 1.4826 * MAD          /* consistent with std-dev for normal data */
```

Robust statistics are used deliberately. A single anomalous cycle during
learning — a hard spot in the casting, a chip jam, an interrupted cut —
inflates a conventional standard deviation, which widens the adaptive band,
which **permanently desensitises the detector**. The median and MAD ignore up
to half the samples being outliers, so one bad cut during commissioning cannot
quietly blunt wear detection for the life of the installation.

The cost is having to keep the samples rather than accumulating incrementally:
`baseline_learn_cycles × 4` bytes per SMU, 80 bytes at the default of 20 and
256 bytes at the maximum of 64. Affordable against 4 KB, and worth it.

#### Learn, then freeze — the central decision ✅

**The baseline is computed once and frozen. It does not continuously adapt.**

This is the decision that makes the detector work, and getting it wrong is the
classic failure of adaptive thresholding: a baseline that keeps tracking the
process will slowly follow a blunting tool upward, the deviation stays near
zero, and the detector reports nothing while the tool wears out. It would look
perfectly healthy and detect nothing. An EWMA or rolling recomputation is
therefore explicitly rejected here.

The baseline is re-established only on an explicit trigger:

| Trigger | Rationale |
|---|---|
| Power-on | Baseline is volatile (§1.1) |
| `SMU_CMD_RELEARN_BASELINE` | Operator/SCADA request |
| `SMU_CMD_RESET_TOOL_STATS` | New tool — the old baseline describes the old tool |
| Committed config change touching current calibration or the SM thresholds | A baseline in amps is meaningless if the amps scale changed |

#### Sample admission ✅

A cycle contributes to learning only if **all** hold:

1. it completed normally (Machine Running de-asserted; not a fault stop);
2. no alarm, breakage, crash or trend condition was active during it;
3. monitoring was armed for the whole cut;
4. no diagnostic was set for the current channel.

A baseline learned from faulted cycles describes a broken process, and every
subsequent comparison inherits that. Cycles that fail admission are skipped
silently — they do not reset progress, they simply do not count.

#### Validity gates ✅

Once the samples are in, the baseline is accepted only if the spread is
usable:

```
    sigma_floor   = baseline_sigma_floor_pct   / 100 * med   /* default  2% */
    sigma_ceiling = baseline_sigma_ceiling_pct / 100 * med   /* default 25% */

    if (sigma < sigma_floor)   sigma = sigma_floor;          /* clamp up   */
    if (sigma > sigma_ceiling) baseline_valid = false;       /* reject     */
```

- **The floor** stops a very repeatable process from producing `sigma → 0`,
  which would collapse the adaptive band onto the mean and trip on measurement
  noise every cycle. It mirrors the 2% guard already used in
  `alarm_on_cycle_end()`'s rise detector, for the same reason.
- **The ceiling** is a judgement that some processes are simply not repeatable
  enough for adaptive monitoring. If cycle-to-cycle spread exceeds a quarter
  of the mean, any band wide enough to avoid false trips is too wide to catch
  wear. Better to declare the baseline invalid and say so than to publish a
  number that cannot work.

#### How it drives detection ✅

With a valid baseline:

```
    deviation_sigma = (last_cycle_mean - baseline_median) / sigma

    warn_limit  = baseline_median + adaptive_k_warn  * sigma   /* k = 3.0 */
    alarm_limit = baseline_median + adaptive_k_alarm * sigma   /* k = 5.0 */
```

Both limits are **clamped to stay below the fixed `BAND_HI` limit** for
current. An adaptive limit above the absolute limit is meaningless — the fixed
band would have tripped first — and letting it drift above would imply a
safety margin that does not exist.

Exceeding a limit for **one** cycle raises nothing. The condition must persist
for `trend_cycles` consecutive admitted cycles (default 3). At k = 3 the
per-cycle false-positive rate on roughly normal data is a few tenths of a
percent, which at industrial cycle rates is several nuisance alarms a week;
requiring three consecutive occurrences makes that negligible while costing at
most three cycles of detection latency on a process that degrades over
hundreds.

Severity follows the existing model: the adaptive detector raises `SEV_TREND`,
below the `SEV_WARNING`/`SEV_ALARM` that the fixed bands raise. It feeds the
current fault output through the same rollup as `breakage` and `crash` (§3.4).

#### Behaviour while learning ✅

Wear detection **degrades rather than disappears** during the learning window.
The existing consecutive-rise detector in `alarm_on_cycle_end()` needs no
baseline and stays active throughout, so a tool blunting during the first 20
cycles is still caught — just less sensitively. Once the baseline matures the
adaptive band takes over as the primary signal and the rise counter remains as
a complementary one.

`BASELINE_STATE` reports 0 (learning) throughout, and `DEVIATION_SIGMA` is not
meaningful until it reads 1. Given §1.1, this window reopens after every power
cycle.

#### Configuration ✅

Three new fields join the two that already exist:

| Field | Default | Range |
|---|---|---|
| `baseline_learn_cycles` | 20 | 5–64 |
| `baseline_sigma_floor_pct` | 2 | 1–20 |
| `baseline_sigma_ceiling_pct` | 25 | 5–100 |
| `adaptive_k_warn` | 3.0 | 1.0–10.0 (existing) |
| `adaptive_k_alarm` | 5.0 | 1.0–20.0 (existing) |

`app_config_validate()` must additionally enforce
`adaptive_k_warn < adaptive_k_alarm` and
`sigma_floor_pct < sigma_ceiling_pct`.

🟡 **Implementation note**: these three fields are already carried in
`shared/smu_proto.h`, but `wear_cfg_t` in `app_config.h` still needs them
added, with a `CONFIG_SCHEMA_VERSION` bump, when the ESP32 side is migrated.
That is deliberately not done on this branch, which still holds unmigrated V1
firmware. `trend_enabled` can default to `true` once this lands.

---

## 4. Portability changes required in the shared modules

Three concrete changes to the pure-C modules to run on a 24 MHz Cortex-M23 with 4 KB RAM. All are mechanical; none change behaviour.

### 4.1 Time base: `int64_t` microseconds → `uint32_t` milliseconds ✅

`alarm.c` and `spindle_sm.c` take `int64_t now_us`. 64-bit arithmetic on an M23 is multi-instruction and this appears throughout the hot path.

Change to `uint32_t now_ms`. Millisecond resolution is ample — the shortest configurable interval in the system is an on-delay in whole milliseconds.

**This introduces a wrap at 49.7 days of continuous uptime, on a device explicitly intended to run for months.** Every comparison must therefore be wrap-safe:

```c
/* correct across wrap */    if ((uint32_t)(now_ms - since_ms) >= delay_ms)
/* WRONG — breaks at wrap */ if (now_ms >= since_ms + delay_ms)
```

A single subtraction-then-compare idiom, used everywhere, is wrap-correct by construction. The existing host tests should gain a case that runs across the wrap boundary — this is precisely the class of bug that is invisible on the bench and appears seven weeks into a customer's production run.

### 4.2 Floating point is soft-float

The M23 has no FPU. `float` still works, via library calls, and the existing code is not float-heavy outside the RMS burst (one multiply-accumulate per sample plus one `sqrtf`). Keep `float` for clarity and correctness; measure the burst on bring-up (§3.1) rather than pre-emptively converting to fixed-point.

### 4.3 Config subset for the SMU ✅

`app_config_t` contains WiFi, Modbus, and output-mapping fields the SMU has no use for. Split the header:

- `spindle_cfg_t` (current, pressure, rpm, sm, wear, `bands[QTY_COUNT][BAND_COUNT]`) → **shared**, compiled into both images, and is exactly what crosses the I²C link.
- WiFi / Modbus / `dout[]` → ESP32-only.

The shared struct must be laid out identically on both sides. Explicit padding, fixed-width types, `static_assert` on `sizeof()` in both builds, and a schema version inside the struct — an ESP32 and an SMU disagreeing about a struct layout would corrupt calibration silently.

**Per the V2 reliability requirement, `band_cfg_t` keeps all six fields** — `enabled`, `limit`, `hysteresis`, `on_delay_ms`, `off_delay_ms`, `latching` — for all four bands of all three quantities, and the web UI exposes them. This reverses V1's fixed-value simplification.

---

## 5. ESP32 master firmware

### 5.1 New module: `smu_link.[ch]`

The only substantially new code on the ESP32.

- One FreeRTOS task per SMU, or one task servicing both buses — **one task per SMU is preferred**, because it makes fault isolation between spindles structural rather than a matter of careful coding, which is the same reasoning that put the two SMUs on independent I²C buses.
- Polls telemetry at **20 Hz (50 ms)**, set by the 100 ms report budget in §3.1 — not the 2 Hz originally sketched in the hardware document, which predated that requirement.
- Pushes config event-driven, on operator change and on link (re)establishment.
- Publishes a snapshot behind a mutex, exactly as `monitor.c` does today, so `web.c` / `modbus.c` / `trend.c` need no structural change — only a new source.

**Link supervision** — the failure mode that matters:
- A poll timeout or CRC failure marks that SMU's telemetry **stale**, and
  stale is displayed as stale — never as last-known-good with no indication,
  and never as zero.
- Stale telemetry means **the ESP32 is blind, not that the machine is
  faulted.** The SMU is still acquiring, still evaluating, and still driving
  its own PLC outputs correctly; only the master's view is missing. The ESP32
  must therefore never escalate its own ignorance into a process alarm —
  reporting a fault the machine does not have is itself a reliability failure,
  and it trains operators to distrust the alarms that are real.
- Instead, the ESP32 **reports its own uncertainty explicitly and lets the
  SCADA layer decide** — see §5.3. This is the standard industrial pattern
  (the same reason OPC tags carry a quality attribute alongside a value), and
  it is the correct division of labour here: the master knows whether its data
  is fresh, the SCADA layer knows what the site's policy should be.
- Recovery is automatic: on the first good frame telemetry goes live again,
  and the ESP32 re-pushes config to confirm the SMU's copy matches.

### 5.2 I²C addressing and register map

**Slave addresses (7-bit)** ✅:

| Device | Address | Binary |
|---|---|---|
| SMU 1 (Spindle 1) | `0x21` | `010 0001` |
| SMU 2 (Spindle 2) | `0x22` | `010 0010` |

Two notes on why these:

- **The two SMUs get *different* addresses even though they sit on separate
  buses** and could legitimately share one. Distinct addresses make a
  cross-wiring or swapped-daughter-card mistake immediately detectable at
  bring-up — the ESP32 simply gets no ACK — instead of silently attributing
  spindle 2's readings to spindle 1. That failure would be very hard to spot
  in the field and very easy to spot here.
- **Avoid the low addresses.** I²C reserves `0x00–0x07` and `0x78–0x7F`, so a
  literal `0b101` (`0x05`) is not usable. `0x21`/`0x22` sit clear of both
  reserved ranges and of the addresses commonly squatted by sensor parts.

**Register map — implemented in `shared/smu_proto.h`** ✅, compiled into both
images. That header is the single definition of every offset and struct on the
wire; they are deliberately not restated anywhere else, including here.

| Base | Access | Size | Contents |
|---|---|---|---|
| `0x0000` | R | 16 B | Identity: magic, protocol version, firmware version, reset cause/count, uptime, last command result |
| `0x0100` | R | 72 B | Telemetry: sequence, status/diag flags, state, severity, current/pressure/RPM, per-band active+latched bitmaps, sensor status, output read-back, raw pre-scaling values, closed-cycle summary |
| `0x0200` | W | 16 B | Commands: acknowledge, auto-zero, config commit/abort, tool-stat reset, baseline relearn, soft reset |
| `0x0300` | R/W | 296 B | Config: calibration, detector parameters, and the full `bands[3][4]` |

Four decisions worth recording, three of which corrected the original sketch:

- **The register pointer is 16-bit, not 8-bit.** The sketch assumed a 256-byte
  space. The band configuration array alone is 192 bytes and the config block
  totals 296, so it does not fit — this was caught by computing the sizes
  rather than assuming them. Sixteen bits also lets telemetry grow without
  renumbering anything.
- **CRC-16/CCITT replaces the sketched CRC8.** Over a 296-byte config block,
  CRC8's error detection is not worth the one byte saved on a link running at
  ~14% utilisation. Calibration silently corrupted in transit is precisely
  what this guards against.
- **Telemetry carries a `seq` counter.** A CRC cannot detect a *stalled* SMU
  that is still ACKing and returning perfectly checksummed stale bytes. A
  sequence number can, so the master treats a frozen `seq` as a link fault.
- **Commands carry a nonce**, echoed back in the identity block, so the master
  can distinguish "command not yet seen" from "seen, and this is its result".
  Without it, a retried command is indistinguishable from a stuck result code.

Plus the two properties the design already required:

- **Config writes are two-phase**: write the block, then issue an explicit
  commit. A partial write interrupted mid-transfer is never applied. The SMU
  validates the complete staged block (`app_config_validate()`, already
  written and host-tested) and rejects with a reason code the UI can surface.
- **Telemetry reads are atomic**: the SMU double-buffers, so a read cannot
  catch a half-updated frame.

Layout is enforced by `_Static_assert` on every struct size and window bound
in both builds. An ESP32 and an SMU that disagree about padding fail the build
rather than corrupting calibration in the field — that enforcement is the
reason the header exists at all.

### 5.3 Modbus register map — data quality is explicit ✅

Modbus RTU (RS-485) and TCP remain the PLC/SCADA-facing interface, and since
SCADA is where site-level analysis actually happens, the ESP32's obligation is
to **report accurately including what it does not know**. Three rules:

1. **Process and alarm registers are never synthesised from link state.** If a
   spindle's telemetry is stale, its measurement and alarm registers are not
   written with invented values, and no alarm bit is set merely because the
   ESP32 cannot see the SMU.
2. **Every telemetry group carries a data-validity bit**, plus a per-spindle
   link-status register (live/stale, age of last good frame). Stale values are
   **held at last-known-good rather than zeroed** — zeroing would look like a
   real reading of zero, which for RPM or pressure is itself an alarm
   condition — and the validity bit is what distinguishes "3.2 A, measured
   200 ms ago" from "3.2 A, last seen 40 seconds ago."
3. **Health and diagnostic registers are separate from process alarms.**
   Sensor faults, RPM-sensor-suspect, SMU health, reset cause, and link status
   are their own registers. A SCADA integrator must be able to distinguish "the
   process is out of limits" from "the instrument is unwell" without decoding
   one merged severity value.

This lets the site apply whatever policy it wants — trip on stale data, ride
through it, or alarm only after N seconds — without the ESP32 having
pre-decided on its behalf. The SMU's own PLC outputs (§3.4) remain the
fast, deterministic, hard-wired safety path regardless of what SCADA does
with any of this.

**The full register map is specified in `MODBUS_REGISTER_MAP.md`** — system
identity and link supervision, live measurement, alarm/band detail with
value-at-trip and echoed limits, instrument diagnostics, and a substantial
cycle-statistics block for predictive maintenance (baseline deviation in σ,
margin-to-trip, rolling windows, and since-power-on counters). Register numbering
there is provisional until the shared generated header of §5.2 exists.

### 5.4 Web UI changes

- **Thresholds**: restore the full four-band editor with hysteresis, delays, and per-band latching. V1's `VISIBLE_BANDS` restriction and fixed values are removed.
- **Calibration**: unchanged workflow (guided 2-point pressure, 1-point CT, auto-zero), now writing through to the SMU and awaiting its commit acknowledgement rather than completing locally.
- **New — SMU status panel**: per-SMU link state (live/stale), firmware version, reset cause, last config sync. Without this, a stale link is invisible to the operator, and an operator who cannot tell a working system from a blind one does not have a reliable system.
- **Pressure mode**: per-spindle 4–20 mA / 0–10 V selector, matching the hardware jumper (§3.2), with the mismatch diagnostic surfaced prominently if the sense pin is adopted.
- **New — Machine Running enable**: per-spindle toggle (`machine_running_enabled` in the shared config) for whether the pin-19 level signal participates in arming (§3.3). Fault clear is always active and has no toggle. The panel should also show the live state of both inputs, so a miswired or stuck signal is diagnosable without a meter.

---

## 6. Failure modes

The design intent, stated as behaviour, so it can be tested:

| Failure | SMU behaviour | ESP32 behaviour | PLC hard-wired inputs see | SCADA sees over Modbus |
|---|---|---|---|---|
| ESP32 powered off, rebooting, or mid-OTA | Unaffected — full monitoring, outputs live | — | Correct fault state throughout | No comms (its own detectable condition) |
| I²C link broken | Unaffected — runs on its Data Flash calibration | Marks telemetry stale | Correct fault state throughout | Last-good values, **validity bit clear**, link register stale |
| SMU hangs | Watchdog resets it; outputs de-energise | Marks telemetry stale | Health output de-energised = fault | Validity bit clear + health register faulted |
| SMU Data Flash corrupt | Safe defaults, health output de-energised | Shows the fault | Health output de-energised = fault | Health register faulted, values valid but flagged |
| Sensor open/short | Diagnostic; process bands for that quantity suppressed | Shows the fault | That quantity's fault output asserted | Diagnostic register set, distinct from process alarm |
| Speed sensor failed while cutting | RPM-sensor-suspect; monitoring stays armed | Shows the fault | RPM fault output asserted | RPM-suspect diagnostic set |
| Pressure jumper/firmware mode mismatch | Diagnostic; pressure not reported; health de-energised | Shows the fault | Health output de-energised = fault | Pressure validity clear + diagnostic set |
| Both SMUs dead | — | Marks both stale | All outputs de-energised | All validity bits clear |

Two lines run through all of these:

- **A fault the system cannot measure is reported as a fault, never as
  normal.**
- **Not knowing is reported as not knowing, never as a fault.** The stale-link
  rows above are the important distinction: the hard-wired outputs stay
  correct because the SMU is still working, while Modbus honestly reports that
  the master's view is out of date. Conflating the two — either direction —
  produces alarms the machine did not have or silence it did.

---

## 7. Verification

- **Host tests are the primary safety-logic gate.** `alarm.c`, `spindle_sm.c`,
  `scaling.c`, and `app_config.c` already run under `host_test/` with no
  hardware. That coverage survives the port unchanged and must be extended
  for:
  - the millisecond time base **including a case that runs across the 49.7-day
    wrap boundary** (§4.1) — precisely the class of bug that is invisible on
    the bench and surfaces seven weeks into a customer's production run;
  - per-band latching combinations restored in V2 (§4.3);
  - **the arming AND and disarm ordering (§3.3)**: Machine Running asserted
    without local CUTTING must not arm; local CUTTING without Machine Running
    must not arm (when the signal is enabled); a fault present in the last
    armed sample must still be caught when the de-assert edge arrives in the
    same pass; and latched conditions must survive the disarm rather than
    being cleared by it.
  - **the Fault Clear one-shot**: fires at 1000 ms of continuous assertion,
    once only, and rearms only after release.
- **Shared-struct layout** is checked by `static_assert` in both builds, not by inspection.
- **Link supervision and data-quality reporting** are testable on the ESP32
  side by stubbing `smu_link` — stale/recovery behaviour and the Modbus
  validity bits need no SMU hardware.
- **Bench verification** is required for the items firmware cannot self-check:
  loop timing at 24 MHz, the 50 ms detect / 100 ms report budget measured
  end-to-end, RMS accuracy against a reference meter, Machine Running edge
  timing against real machine behaviour, and output timing against a real PLC
  scan.

---

## 8. Open items

| # | Item | Blocks | Resolved by | Ref |
|---|---|---|---|---|
| 1 | 🟡 SMU firmware must fit ~31 KB APROM after the 1 KB storage reservation | SMU coding | First real link | §3.5 |
| 2 | 🟡 Loop timing and soft-float RMS burst cost at 24 MHz | SMU bring-up | Measurement on silicon | §3.1, §4.2 |

**All design decisions are closed.** Both remaining items are measurements
against a real build: one at link time, one on the bench. Neither affects PCB
layout, so hardware can proceed independently.

**Closed in recent revisions**: SMU pin assignment (verified against the
datasheet, §3.7 — debug UART moved to pins 12/13); the I²C register map
(`shared/smu_proto.h`, §5.2); Modbus rolling-window depths and float-vs-integer
representation (Modbus doc §9); the adaptive wear baseline algorithm (§3.8);
and the flash budget (§3.5 — 512 B pages, 1 KB reserved, no external EEPROM).

**Closed since first draft**: cycle-start/fault-clear discrimination
(superseded — two separate inputs, §3.3); end-of-cycle detection (**no longer
needed** — Machine Running is a level signal, so cycle end is read rather than
inferred, and `coast_min_ms` / `cycle_end_dwell_ms` / `fault_lockout_ms` are
deleted along with the decay-signature discriminator); `uint32_t` millisecond
time base (§4.1); stale-telemetry handling (explicit data-quality reporting
over Modbus, §5.3); pressure jumper sense pin (adopted, §3.2); I²C slave
addresses (§5.2); Data Flash endurance (§3.5); latency budget and poll rate
(§3.1); Modbus register map (now its own document,
`MODBUS_REGISTER_MAP.md`).

---

## 9. Not covered here

Toolchain and IDE selection for the Nuvoton part, production programming/test fixtures, SMU firmware update mechanism in the field (the ESP32 has OTA; whether it should also be able to reflash an SMU over I²C is an open product question, not yet a requirement), and the SRS/roadmap documents referenced by `README.md` that do not exist in this repository.
