# CNC Tool Monitor V2 — Modbus Register Map

**Audience: PLC and SCADA integrators**
**Status: DRAFT — register numbering is provisional until the shared header is generated (firmware spec §5.2)**
**Companions: `FIRMWARE_DESIGN_SPEC.md` (behaviour), `SMU_HARDWARE_REQUIREMENTS.md` (electrical)**

---

## 1. How to read this document

The device exposes measurement, alarm, diagnostic, and cycle-statistics data
over **Modbus RTU (RS-485)** and **Modbus TCP (WiFi)** simultaneously. Both
transports are always available, present the identical register map, and may
be used at the same time by different clients.

Four principles govern the map, and understanding them will save integration
time:

0. **The device keeps no history. This map is the historian's input, not its
   output.** Nothing here survives a power cycle: every counter and statistic
   restarts at zero on power-on, and there is no event log, no trend archive,
   and no stored alarm record. Only configuration and calibration persist.
   **If the site needs history, SCADA must poll and store it** — that is the
   intended architecture, not a workaround. Retention, backup, and reporting
   then live where the site's own policies already apply. See §7 for what this
   means for the predictive-maintenance registers specifically.

1. **Process alarms, instrument diagnostics, and data quality are three
   different things, in three different register blocks.** "The cut is out of
   limits," "the pressure sensor is broken," and "this reading is 8 seconds
   old" are distinct conditions requiring distinct responses. They are never
   merged into a single severity value.

2. **Every measurement block carries a validity flag.** The ESP32 polls each
   Spindle Monitoring Unit (SMU) at 20 Hz; if that link degrades, the last
   good values are **held, not zeroed**, and the validity flag goes false.
   Zeroing would be actively harmful — a pressure or RPM reading of zero is
   itself an alarm condition, so a zeroed stale register looks like a real
   fault. Always gate on validity before trusting a value.

3. **Modbus is not the safety path.** Each SMU drives its own hard-wired
   fault outputs directly, within 50 ms, with no processor in between. Those
   outputs remain correct even if the ESP32 is rebooting, mid-OTA, or
   powered off. Modbus is for supervision, trending, diagnostics, and
   predictive maintenance — treat it as rich but advisory.

### Conventions

| | |
|---|---|
| Register model | Input Registers (FC 04) for read-only data; Holding Registers (FC 03/06/16) for commands and settable values |
| Word order | Big-endian, high word first for 32-bit values |
| Floats | IEEE-754 single precision, 2 registers each |
| Scaled integers | Where noted, to allow integer-only PLCs to avoid float handling — both representations are provided for the main measurements |
| Timestamps | Milliseconds since SMU boot, 32-bit, wraps at ~49.7 days |
| Spindle offset | Spindle 1 at base, Spindle 2 at base + `0x0100` |

---

## 2. Block map

| Range | Type | Contents | Section |
|---|---|---|---|
| `30001–30099` | Input | System identity, health, link status | §3 |
| `30101–30199` | Input | Spindle 1 — live measurement | §4 |
| `30201–30299` | Input | Spindle 1 — alarm and band detail | §5 |
| `30301–30399` | Input | Spindle 1 — diagnostics | §6 |
| `30401–30499` | Input | Spindle 1 — cycle statistics & PdM | §7 |
| `30501–30899` | Input | Spindle 2 — same layout, +`0x0100` per block | §4–§7 |
| `40001–40099` | Holding | Commands and acknowledgements | §8 |

---

## 3. System block — `30001` onward

Read this block first. It tells you whether the rest of the map can be
believed.

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30001 | u16 | `DEVICE_ID` | `0x434D` ("CM") — confirms you are talking to the right device |
| 30002 | u16 | `PROTOCOL_VERSION` | Increments only on breaking map changes |
| 30003–30004 | u32 | `FIRMWARE_VERSION` | ESP32 master image, packed `MAJOR.MINOR.PATCH.BUILD` |
| 30005–30006 | u32 | `UPTIME_S` | Master uptime, seconds |
| 30007 | u16 | `SYSTEM_HEALTHY` | 1 = every subsystem nominal. **Not** a process-alarm summary |
| 30008 | u16 | `SPINDLE_COUNT` | 2 |
| 30009 | u16 | `SPINDLE_ENABLED_MASK` | bit0 = S1, bit1 = S2 |
| 30010 | u16 | `CONFIG_SCHEMA_VERSION` | |
| 30011–30012 | u32 | `CONFIG_CHANGE_COUNTER` | Increments on every committed config change — poll to detect someone editing limits |
| 30013–30014 | u32 | `CONFIG_LAST_CHANGE_S` | Uptime seconds at last change |

**Link supervision** — the registers that tell you whether spindle data is
current:

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30020 | u16 | `S1_LINK_STATE` | 0 = down, 1 = live, 2 = degraded (intermittent CRC failures) |
| 30021 | u16 | `S1_DATA_VALID` | **Gate every S1 measurement on this** |
| 30022–30023 | u32 | `S1_DATA_AGE_MS` | Since last good frame. Normally ≤ 50 ms |
| 30024–30025 | u32 | `S1_CRC_ERROR_COUNT` | Cumulative. A slowly rising count is a cabling or EMI problem worth investigating before it becomes a dropout |
| 30026–30027 | u32 | `S1_TIMEOUT_COUNT` | Cumulative poll timeouts |
| 30030–30037 | | `S2_*` | Same fields for Spindle 2 |

> **Integration note.** `LINK_STATE = 2` (degraded) is the one worth alarming
> on early. It means frames are arriving but some are failing CRC — the SMU is
> still monitoring correctly and its hard-wired outputs are still right, but
> the link is deteriorating. Catching it here is the difference between a
> scheduled connector re-seat and an unexplained loss of visibility mid-shift.

---

## 4. Live measurement — `30101` (S1) / `30501` (S2)

| Reg | Type | Name | Unit |
|---|---|---|---|
| 30101 | u16 | `BLOCK_VALID` | 1 = values below are fresh |
| 30102–30103 | f32 | `CURRENT_A` | A, RMS |
| 30104–30105 | f32 | `CURRENT_AVG_A` | A, 1 s moving average |
| 30106–30107 | f32 | `CURRENT_PEAK_A` | A, peak hold within current cut |
| 30108–30109 | f32 | `PRESSURE` | Configured unit (see 30120) |
| 30110–30111 | f32 | `RPM` | rev/min |
| 30112 | u16 | `CURRENT_A_X100` | A × 100, integer convenience copy |
| 30113 | u16 | `PRESSURE_X10` | × 10, integer convenience copy |
| 30114 | u16 | `RPM_INT` | Integer convenience copy |
| 30115 | u16 | `SPINDLE_STATE` | 0 STOPPED, 1 SPIN_UP, 2 IDLE, 3 CUTTING, 4 COAST_DOWN |
| 30116 | u16 | `MONITORING_ARMED` | 1 = thresholds are live. **See note below** |
| 30117 | u16 | `MACHINE_RUNNING` | Live state of the pin-19 level input (1 = machine reports cutting) |
| 30118 | u16 | `SEVERITY` | 0 none, 1 diag, 2 trend, 3 warning, 4 alarm, 5 breakage, 6 crash |
| 30119 | u16 | `PRESSURE_MODE` | 0 = 4–20 mA, 1 = 0–10 V |
| 30120 | u16 | `PRESSURE_UNIT` | 0 bar, 1 psi, 2 kPa, 3 MPa |

> **`MONITORING_ARMED` is the single most useful register in this block for
> understanding device behaviour.** Thresholds are evaluated only while a
> spindle is actually cutting — that is the product's primary false-alarm
> defence. If you are asking "why did this out-of-limit reading not raise an
> alarm," check this register first. A spindle in SPIN_UP draws several times
> its cutting current entirely normally, and monitoring is deliberately
> disarmed there.

**Raw / pre-scaling values**, for calibration verification and drift analysis:

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30130–30131 | f32 | `CT_BURDEN_VRMS` | Burden voltage before scaling |
| 30132–30133 | f32 | `PRESSURE_ADC_V` | ADC volts before scaling |
| 30134–30135 | f32 | `PRESSURE_LOOP_MA` | Computed loop current, 4–20 mA mode |
| 30136–30137 | f32 | `CURRENT_ZERO_OFFSET_V` | Active auto-zero tare |

> Trending `PRESSURE_LOOP_MA` at known-zero process conditions is the cheapest
> available early warning of transmitter drift: the value should sit at 4.00 mA
> and a slow walk away from it predicts a calibration failure weeks ahead.

---

## 5. Alarm and band detail — `30201` (S1) / `30601` (S2)

Summary bits:

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30201 | u16 | `ANY_ALARM` | Any alarm-severity condition |
| 30202 | u16 | `ANY_WARNING` | Any warning-severity condition |
| 30203 | u16 | `ANY_LATCHED` | Something is latched awaiting acknowledgement |
| 30204 | u16 | `OUTPUT_STATE` | Mirror of the 4 hard-wired outputs: bit0 current, bit1 pressure, bit2 RPM, bit3 healthy. **Read-back of the real safety path** |

Per-quantity band state. Each quantity gets an active bitmap and a latched
bitmap, with bit0 = LoLo, bit1 = Lo, bit2 = Hi, bit3 = HiHi:

| Reg | Name | Reg | Name |
|---|---|---|---|
| 30210 | `CURRENT_BANDS_ACTIVE` | 30211 | `CURRENT_BANDS_LATCHED` |
| 30212 | `PRESSURE_BANDS_ACTIVE` | 30213 | `PRESSURE_BANDS_LATCHED` |
| 30214 | `RPM_BANDS_ACTIVE` | 30215 | `RPM_BANDS_LATCHED` |

Transient detectors:

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30220 | u16 | `BREAKAGE` | Sudden current drop — tool breakage |
| 30221 | u16 | `CRASH` | Sudden current rise — collision |
| 30222 | u16 | `WEAR_TREND` | Consecutive rising cycle means |
| 30223 | u16 | `RISING_CYCLES` | Current count toward the trend threshold — **leading indicator, see §7** |

**Value-at-trip** — what the measurement actually was when each condition
first asserted. Preserved until cleared, so a post-event investigation does
not depend on having been polling fast enough at the moment:

| Reg | Type | Name |
|---|---|---|
| 30230–30231 | f32 | `CURRENT_AT_TRIP` |
| 30232–30233 | f32 | `PRESSURE_AT_TRIP` |
| 30234–30235 | f32 | `RPM_AT_TRIP` |
| 30236–30237 | u32 | `FIRST_TRIP_TIMESTAMP_MS` |

**Active limits**, echoed back so SCADA can display and trend against them
without a separate configuration channel:

| Reg | Type | Name |
|---|---|---|
| 30240–30247 | f32 ×4 | `CURRENT_LIMIT_LOLO / LO / HI / HIHI` |
| 30248–30255 | f32 ×4 | `PRESSURE_LIMIT_LOLO / LO / HI / HIHI` |
| 30256–30263 | f32 ×4 | `RPM_LIMIT_LOLO / LO / HI / HIHI` |
| 30264 | u16 | `BANDS_ENABLED_MASK` | 12 bits: 3 quantities × 4 bands |

> **Why the limits are exposed.** Trending a measurement against its limit as
> a *ratio* rather than an absolute — margin-to-trip — is what turns this from
> an alarm system into a predictive one. A cut running at 60% of its Hi limit
> that drifts to 85% over three weeks has told you something well before it
> trips, and margin is only computable if SCADA knows the limit.

---

## 6. Diagnostics — `30301` (S1) / `30701` (S2)

Instrument health, strictly separate from process alarms. A set bit here means
*the measurement cannot be trusted*, not *the machine is out of limits*.

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30301 | u16 | `DIAG_SUMMARY` | Any diagnostic active |
| 30302 | u16 | `CURRENT_SENSOR_STATUS` | 0 OK, 1 open, 2 short, 3 over-range, 4 uncalibrated |
| 30303 | u16 | `PRESSURE_SENSOR_STATUS` | Same encoding. Open loop < 3.5 mA, short > 21 mA |
| 30304 | u16 | `RPM_SENSOR_SUSPECT` | **See note** |
| 30305 | u16 | `PRESSURE_MODE_MISMATCH` | Jumper disagrees with configured mode — pressure is not reported at all while set |
| 30306 | u16 | `COAST_ANOMALY` | Spindle decelerated far faster than inertia allows — seizure or broken belt |
| 30320 | u16 | `RUN_SIGNAL_NO_LOAD` | Machine Running asserted but no current and no RPM seen — **armed but blind** |
| 30321 | u16 | `LOAD_NO_RUN_SIGNAL` | Real cutting current with Machine Running de-asserted — signal miswired, inverted, or spindle run outside the program |
| 30322 | u16 | `RUN_SIGNAL_STUCK` | Asserted continuously beyond any plausible cut duration |
| 30323 | u16 | `MACHINE_RUNNING_ENABLED` | 0 = signal excluded from arming by configuration |
| 30307 | u16 | `SMU_HEALTHY` | SMU self-check status |
| 30308 | u16 | `SMU_RESET_CAUSE` | 0 power-on, 1 watchdog, 2 brownout, 3 software, 4 external |
| 30309–30310 | u32 | `SMU_RESET_COUNT` | Cumulative. **Any nonzero watchdog contribution deserves investigation** |
| 30311–30312 | u32 | `SMU_UPTIME_MS` | Since last SMU reset |
| 30313 | u16 | `CALIBRATION_SOURCE` | 0 = Data Flash (normal), 1 = safe defaults after storage failure |
| 30314 | u16 | `SMU_LOOP_TIME_MS` | Last measured superloop period — should read 50 |
| 30315 | u16 | `SMU_LOOP_WORST_MS` | Worst observed since reset. **Rising = the SMU is losing timing margin** |

> **`RPM_SENSOR_SUSPECT` deserves specific attention in SCADA.** It means real
> cutting current is flowing while the RPM input reads zero — the speed sensor
> has failed, not the spindle stopped. It is called out separately because the
> naive interpretation ("spindle stopped") would disarm all monitoring, and
> everything would then look perfectly normal while nothing was being watched.
> This register exists so that failure is loud rather than silent.

> **`PRESSURE_MODE_MISMATCH` likewise.** The pressure input is jumper-selected
> between 4–20 mA and 0–10 V, and firmware is configured to match. If the two
> disagree, readings are wrong by a fixed scale factor while every other
> diagnostic passes cleanly. A sense pin detects it, and this is where it
> surfaces.

> **`RUN_SIGNAL_NO_LOAD` is the most operationally urgent bit in this block.**
> The machine reports it is cutting and the instrument sees neither current
> nor rotation. Monitoring is armed against a cut it cannot observe — the
> device looks healthy and is watching nothing. Treat it as an instrument
> failure requiring attention before the next cut, not as an informational
> flag. `LOAD_NO_RUN_SIGNAL` is its mirror image and usually means the signal
> is miswired or inverted; monitoring stays disarmed in that state, so real
> cutting is going unwatched until it is fixed.

---

## 7. Cycle statistics and predictive maintenance — `30401` (S1) / `30801` (S2)

This block is the reason the map is as large as it is. Per-cycle statistics
are where tool wear, fixture problems, and coolant degradation show up long
before any threshold trips.

**Cycle boundaries come from the Machine Running input** (asserted→de-asserted
closes a cycle) when that signal is enabled, and from local CUTTING detection
otherwise. The former is more accurate, because it aligns statistics with what
the machine considers a cut rather than with a current threshold — worth
knowing when comparing data across installations that differ in whether the
signal is wired.

**Last completed cycle:**

| Reg | Type | Name | Unit |
|---|---|---|---|
| 30401–30402 | u32 | `CYCLE_COUNT` | Cumulative completed cuts |
| 30403–30404 | u32 | `LAST_CYCLE_DURATION_MS` | |
| 30405–30406 | f32 | `LAST_CYCLE_MEAN_A` | |
| 30407–30408 | f32 | `LAST_CYCLE_PEAK_A` | |
| 30409–30410 | f32 | `LAST_CYCLE_MEAN_PRESSURE` | |
| 30411–30412 | f32 | `LAST_CYCLE_MEAN_RPM` | |
| 30413–30414 | u32 | `LAST_CYCLE_END_TIMESTAMP_MS` | |
| 30415 | u16 | `LAST_CYCLE_ENDED_CLEANLY` | 1 = ended with no alarm active, 0 = a fault was live when Machine Running de-asserted |

**Baseline and deviation** — the core predictive signal:

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30420–30421 | f32 | `BASELINE_MEAN_A` | Learned reference for a healthy cut |
| 30422–30423 | f32 | `BASELINE_SIGMA_A` | Spread of that reference |
| 30424–30425 | f32 | `DEVIATION_SIGMA` | How many σ the last cycle sat from baseline — **the single best wear indicator here** |
| 30426–30427 | f32 | `MARGIN_TO_HI_PCT` | Last cycle peak as % of the Hi limit |
| 30428 | u16 | `BASELINE_SAMPLE_COUNT` | Cycles contributing to the baseline |
| 30429 | u16 | `BASELINE_VALID` | 0 while still learning — **including after every power-on** |

> **The baseline is relearned from scratch after every power cycle.** It lives
> in RAM like everything else here, so `DEVIATION_SIGMA` — the strongest
> predictive signal in this map — reads as invalid until enough cycles have
> run to re-establish it. On a machine powered down nightly, the baseline may
> rarely mature.
>
> If predictive maintenance matters at your site, the practical answer is to
> have SCADA compute and hold the baseline itself from the per-cycle values in
> this block, rather than depending on the device's own. SCADA persists; the
> device deliberately does not.

**Rolling windows**, for trend plots without SCADA-side history:

| Reg | Type | Name |
|---|---|---|
| 30430–30431 | f32 | `MEAN_A_LAST_10_CYCLES` |
| 30432–30433 | f32 | `MEAN_A_LAST_100_CYCLES` |
| 30434–30435 | f32 | `PEAK_A_LAST_10_CYCLES` |
| 30436–30437 | f32 | `CYCLE_TIME_MEAN_LAST_10` |

**Counters — all since power-on, none persisted:**

| Reg | Type | Name | Notes |
|---|---|---|---|
| 30440–30441 | u32 | `RUNTIME_S` | Spindle energised |
| 30442–30443 | u32 | `CUTTING_S` | Actual time in CUTTING — the number that correlates with tool life |
| 30444–30445 | u32 | `ALARM_COUNT` | |
| 30446–30447 | u32 | `WARNING_COUNT` | |
| 30448–30449 | u32 | `BREAKAGE_COUNT` | |
| 30450–30451 | u32 | `CRASH_COUNT` | |
| 30452–30453 | u32 | `TREND_ALARM_COUNT` | |
| 30454–30455 | u32 | `CYCLES_SINCE_LAST_ALARM` | |
| 30456–30457 | u32 | `CYCLES_SINCE_TOOL_CHANGE` | Also reset via §8 command |
| 30458–30459 | u32 | `CUTTING_S_SINCE_TOOL_CHANGE` | |
| 30460–30461 | u32 | `POWER_ON_UPTIME_S` | Master uptime — **the denominator for every counter above** |

> **These are not lifetime figures and must not be presented as such.** They
> restart at zero on every power cycle, so `BREAKAGE_COUNT = 0` after a
> reboot means "none since boot," not "none ever." Always read
> `POWER_ON_UPTIME_S` alongside them: a count of 3 over 8 hours and a count
> of 3 over 40 minutes are very different machines. To get true lifetime
> figures, SCADA must accumulate these across power cycles itself —
> detecting a reset by watching `POWER_ON_UPTIME_S` go backwards.

### Using this block

**Everything below requires SCADA-side storage.** The device supplies the
per-cycle values; it does not retain them, and nothing here survives a power
cycle. Poll, store, and analyse externally — that is the intended split, and
the reason this block is as detailed as it is.

Four things worth building, roughly in order of value:

1. **Plot `DEVIATION_SIGMA` per cycle.** A blunting tool raises cutting force
   gradually and monotonically. This shows it while the absolute current is
   still nowhere near the Hi limit — typically many cycles of warning.
2. **Trend `MARGIN_TO_HI_PCT`.** Normalises across tools and materials, so one
   chart works for the whole machine rather than one per operation.
3. **Watch `RISING_CYCLES` (§5).** It is the trend detector's internal
   counter, exposed deliberately: it reaches 1, 2, 3… before it reaches the
   threshold that raises an alarm. Alarming on the counter at *n−1* buys a
   cycle of notice.
4. **Correlate `CYCLES_SINCE_TOOL_CHANGE` against `DEVIATION_SIGMA` at
   failure.** After a few tool lives this gives a site-specific expected tool
   life, which is the input a real preventive-replacement schedule needs.
   Note this one spans multiple tool lives and therefore multiple power
   cycles — it only works if SCADA is accumulating, since the device's own
   counters reset.

Also worth alarming: `LAST_CYCLE_ENDED_CLEANLY = 0` with no corresponding
process alarm, and any rise in `SMU_LOOP_WORST_MS` (§6) — both indicate
something changing that no threshold is watching.

---

## 8. Commands — holding registers, `40001` onward

| Reg | Access | Name | Notes |
|---|---|---|---|
| 40001 | R/W | `ACK_SPINDLE_1` | Write 1 to acknowledge/clear latched S1 conditions |
| 40002 | R/W | `ACK_SPINDLE_2` | |
| 40003 | R/W | `ACK_ALL` | |
| 40010 | R/W | `RESET_TOOL_COUNTERS_S1` | Write 1 at tool change |
| 40011 | R/W | `RESET_TOOL_COUNTERS_S2` | |
| 40020 | R/W | `RELEARN_BASELINE_S1` | Discard and re-learn the wear baseline |
| 40021 | R/W | `RELEARN_BASELINE_S2` | |

All command registers are self-clearing: write 1, the device acts and returns
the register to 0. Read back 0 to confirm the command was consumed.

> **Acknowledgement semantics.** Acknowledging clears latched conditions that
> are **no longer physically present**. A condition that is still true stays
> latched and its output stays asserted — acknowledging an active alarm
> silences the record, not the reality. A write that appears to "not work" is
> almost always a still-present condition.

**Not writable over Modbus**, deliberately: thresholds, calibration, and
band configuration. These are web-UI-only, so that limits cannot be changed by
a PLC program without a human decision and an audit trail. `CONFIG_CHANGE_COUNTER`
(§3) lets SCADA detect when they do change.

---

## 9. Open items

| # | Item |
|---|---|
| 1 | 🟡 Baseline/σ learning algorithm (§7) — `wear_cfg_t` has `adaptive_k_warn`/`adaptive_k_alarm`; the learning window and reset policy need defining |
| 2 | 🟡 Register numbers firm up once the ESP32-side implementation lands; the I²C-side map is now fixed in `shared/smu_proto.h` |

**Resolved:**

- **Rolling-window depths: 10 and 100 cycles, both retained.** Cost is
  bounded and small — the windows store per-cycle summaries, not sample
  streams, so 100 cycles × ~24 bytes × 2 spindles is under 5 KB, which the
  ESP32 has comfortably. Keeping both matters because they answer different
  questions: the 10-cycle mean tracks the current tool, while the 100-cycle
  mean survives tool changes and exposes drift in the *process* — fixture
  wear, coolant degradation — that a short window would mistake for normal
  tool-to-tool variation.
- **Float is primary; scaled integers are retained as a convenience copy.**
  The device computes in float throughout (`alarm.c`, `scaling.c`), so float
  registers are the direct representation and the integer copies are derived.
  They cost 3 registers per spindle and remove an entire class of
  integration friction for integer-only PLCs, which is worth far more than
  the registers. The scaling factors are fixed (`×100` for current, `×10` for
  pressure) rather than configurable — a configurable scale factor is a
  silent-wrongness hazard of exactly the kind §6's mode-mismatch diagnostic
  exists to catch.
