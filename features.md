# CNC Tool Monitor — Features

Real-time tool condition monitoring for CNC machining centers. Catches
tool breakage, crashes, and gradual tool wear before they turn into scrap
parts, damaged spindles, or unplanned downtime — and keeps protecting
your machine even if the network, PC, or Wi-Fi goes down.

---

## Real-Time Tool Condition Monitoring

- **Continuous monitoring of spindle current, coolant/hydraulic
  pressure, and spindle speed** — the three signals that reveal what a
  cutting tool is actually doing, updated multiple times per second.
- **Dual-spindle support** — monitor two independent spindles from a
  single unit, each with its own complete set of thresholds, calibration,
  and protection logic.
- **Instant fault detection** — a broken tool, a crash, or an out-of-range
  condition is detected and signaled to your machine control in a
  fraction of a second, not after the damage has already spread to the
  next several parts.

## Catches the Failures That Matter Most

- **Tool breakage detection** — recognizes the sudden drop in cutting
  load that means a tool has snapped, before the spindle keeps running
  against nothing (or against the part).
- **Crash detection** — recognizes the sudden spike in load that means a
  collision has occurred, so the machine can be stopped before a small
  mistake becomes an expensive one.
- **Tool wear trend detection** — spots a tool gradually cutting harder
  over successive parts, the earliest warning sign of a tool going dull,
  long before it fails outright. Turns tool changes from a guessing game
  into a scheduled event.
- **Speed sensor cross-checking** — automatically tells the difference
  between "the spindle actually stopped" and "the speed sensor stopped
  reporting," so a wiring fault never gets mistaken for a real machine
  event or silently ignored.

## Smart, Configurable Alarms

- **Four-level threshold bands per measurement** — set both a "warning"
  and a "critical" limit on the high side and the low side of every
  signal, so operators get an early heads-up before a hard stop is ever
  needed.
- **Adjustable hysteresis and response delays** — tune exactly how
  quickly an alarm trips and how long a condition must clear before it
  resets, eliminating false alarms from momentary spikes without
  sacrificing fast response to a genuine fault.
- **Latching alarms with operator acknowledgment** — a serious fault
  stays flagged until an operator actively clears it, so nothing gets
  missed at a shift change or in a noisy shop environment.
- **Automatically arms and disarms itself** — thresholds are only live
  while the tool is actually cutting, so normal spindle start-up and
  idling never trigger a false alarm.

## Built to Never Be the Single Point of Failure

- **Independent safety per spindle** — each spindle's protection runs on
  its own dedicated hardware. A problem on one spindle can never affect
  the other.
- **Keeps protecting the machine even if the network goes down** — the
  fault-detection and machine-protection logic runs locally and
  continuously, independent of the network connection, the web
  dashboard, or any PC. A Wi-Fi outage, a reboot, or a firmware update in
  progress never leaves the machine unprotected.
- **Fail-safe outputs** — wiring and signaling are designed so that a
  powered-down or malfunctioning unit reads as a fault, never as
  "everything's fine." The system is never wrong in the dangerous
  direction.
- **Self-monitoring** — the unit continuously checks its own health and
  reports it separately from the machine's condition, so you always know
  whether you're looking at a real machine problem or a monitoring
  problem.

## Easy to See, Easy to Use

- **Web-based dashboard** — view live readings, tool status, and alarm
  history from any phone, tablet, or PC on the shop network. No special
  software to install.
- **Live trend charts** — watch how current, pressure, and speed have
  moved over the last several minutes, right from the dashboard.
- **Clear, at-a-glance status** — color-coded indicators show every
  spindle's state, active alarms, and connection health without needing
  to interpret raw numbers.
- **Designed for the shop floor, not the office** — a mobile-friendly
  layout with large, glove-friendly controls, so thresholds can be
  checked and adjusted from right next to the machine.

## Simple Setup and Calibration

- **Guided calibration wizard** — walks an operator through setting up
  current and pressure readings against known reference points, with no
  specialized training required.
- **One-touch auto-zero** — re-tare the current sensor to compensate for
  drift with a single command, right from the dashboard.
- **Works with the sensors already on your machine** — supports common
  current transformer ratios and both standard industrial pressure
  signal types, so it fits into an existing machine rather than requiring
  it to be rebuilt around the monitor.
- **Factory reset and easy defaults** — get back to a known-good
  configuration in one step if needed.

## Job Templates — Set Up Once, Reuse Every Time

- **Save a complete job setup with one action** — every threshold, tool
  breakage/crash setting, and wear-detection limit for a job, captured and
  named in a single step. No more re-entering the same values by hand every
  time a job comes back around.
- **Load a saved job in seconds** — switching a spindle from one job to
  another is a single selection, not a technician working through every
  setting from memory or a paper checklist. Fewer manual re-entries means
  fewer chances for a mistyped limit to slip through.
- **Carry a job to a different machine** — export a proven job setup from
  one unit and load it straight onto another, so a setup that works on line
  1 doesn't have to be rebuilt from scratch on line 2. Each machine's own
  sensor setup is automatically kept safe and untouched by the transfer —
  a job never overwrites the machine it's loaded onto.
- **Copy settings between spindles on the same machine** — when both
  spindles are running the same kind of job, copy one spindle's setup to
  the other in a single step, with each spindle's own setup kept intact.
- **Built-in compatibility check** — before a job is applied, it's checked
  against the machine it's being loaded onto, with a clear heads-up if
  anything won't behave as expected — so nothing gets loaded blind.

## Connects to What You Already Run

- **Industry-standard protocol support** — integrates directly with
  existing PLCs, SCADA systems, and shop-floor data collection over both
  wired and network connections, so tool status becomes just another
  signal your existing systems already understand.
- **Two independent connection paths** — reachable over both a
  hard-wired industrial connection and the shop Wi-Fi/network at the same
  time, so a plant's SCADA system and a supervisor's laptop can both stay
  connected simultaneously.
- **Redundant calibration storage** — critical setup data is kept safely
  in two places, so a power interruption during a settings change can
  never leave the unit without a valid configuration.

## Future-Proof

- **Remote firmware updates** — new features and improvements can be
  installed over the network, without opening a panel or standing at the
  machine.
- **Automatic rollback protection** — if an update ever doesn't take
  properly, the unit automatically reverts to the last known-good version
  rather than risking a machine left in an unknown state.
