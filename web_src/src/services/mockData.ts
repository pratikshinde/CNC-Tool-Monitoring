import { TelemetrySnapshot, SpindleConfig, HistoryData, JobTemplate, SystemInfo, CommsConfig, CalibState } from "../types";

let simTick = 0;
let spindle1Phase = 0;
let spindle2Phase = 2.5;

export function getMockTelemetry(): TelemetrySnapshot {
  simTick++;
  spindle1Phase += 0.05;
  spindle2Phase += 0.03;

  // Simulate cutting cycles
  const s1Cycle = Math.sin(spindle1Phase);
  const isS1Cutting = s1Cycle > 0.2;
  const isS1Spin = s1Cycle > -0.5;

  const s1Current = !isS1Spin ? 0.08 + Math.random() * 0.04 : (isS1Cutting ? 12.4 + Math.sin(simTick * 0.4) * 1.8 + Math.random() * 0.6 : 3.8 + Math.random() * 0.3);
  const s1Pressure = 5.9 + Math.sin(simTick * 0.1) * 0.25;
  const s1Rpm = !isS1Spin ? 0 : 12000 + (Math.random() - 0.5) * 80;

  const s2Cycle = Math.sin(spindle2Phase);
  const isS2Cutting = s2Cycle > 0.4;
  const isS2Spin = s2Cycle > -0.3;
  const s2Current = !isS2Spin ? 0.05 : (isS2Cutting ? 8.2 + Math.sin(simTick * 0.3) * 1.2 : 2.6 + Math.random() * 0.2);
  const s2Pressure = 6.0 + Math.cos(simTick * 0.12) * 0.2;
  const s2Rpm = !isS2Spin ? 0 : 9000 + (Math.random() - 0.5) * 50;

  return {
    system_healthy: true,
    adc_healthy: true,
    diagnostic_fault: false,
    loop_period_ms: 10,
    worst_loop_ms: 14,
    uptime_s: 3600 + simTick,
    spindles: [
      {
        name: "Main Spindle (Left)",
        enabled: true,
        state: !isS1Spin ? "IDLE" : (isS1Cutting ? "CUTTING" : "RUNNING"),
        monitoring_armed: isS1Cutting,
        current_a: s1Current,
        current_avg_a: 11.2,
        current_peak_a: 16.4,
        pressure: s1Pressure,
        pressure_unit: "bar",
        rpm: s1Rpm,
        severity: s1Current > 15.5 ? "warning" : "none",
        any_alarm: false,
        any_warning: s1Current > 15.5,
        any_latched: false,
        breakage: false,
        crash: false,
        trend: false,
        current_status: "ok",
        pressure_status: "ok",
        rpm_sensor_suspect: false,
        cycle_count: 142,
        last_cycle_ms: 18400,
        last_cycle_mean_a: 12.1,
      },
      {
        name: "Sub Spindle (Right)",
        enabled: true,
        state: !isS2Spin ? "IDLE" : (isS2Cutting ? "CUTTING" : "RUNNING"),
        monitoring_armed: isS2Cutting,
        current_a: s2Current,
        current_avg_a: 7.8,
        current_peak_a: 10.9,
        pressure: s2Pressure,
        pressure_unit: "bar",
        rpm: s2Rpm,
        severity: "none",
        any_alarm: false,
        any_warning: false,
        any_latched: false,
        breakage: false,
        crash: false,
        trend: false,
        current_status: "ok",
        pressure_status: "ok",
        rpm_sensor_suspect: false,
        cycle_count: 88,
        last_cycle_ms: 14200,
        last_cycle_mean_a: 7.9,
      }
    ]
  };
}

export function getMockHistory(count = 120): HistoryData {
  const current_a: number[] = [];
  const pressure: number[] = [];
  const rpm: number[] = [];

  for (let i = count; i >= 0; i--) {
    const t = simTick - i;
    const isCut = Math.sin(t * 0.05) > 0.2;
    current_a.push(isCut ? 12 + Math.sin(t * 0.3) * 2 + Math.random() * 0.5 : 3.5 + Math.random() * 0.3);
    pressure.push(5.8 + Math.sin(t * 0.08) * 0.3);
    rpm.push(12000 + Math.sin(t * 0.1) * 100);
  }

  return { current_a, pressure, rpm };
}

export const mockSpindleConfigs: SpindleConfig[] = [
  {
    name: "Main Spindle (Left)",
    enabled: true,
    pressure_unit: "bar",
    bands: {
      lolo: { enabled: true, limit: 1.5, hysteresis: 0.2, trip_delay_ms: 100, clear_delay_ms: 200, latched: true },
      lo: { enabled: true, limit: 3.0, hysteresis: 0.3, trip_delay_ms: 200, clear_delay_ms: 200, latched: false },
      hi: { enabled: true, limit: 14.5, hysteresis: 0.5, trip_delay_ms: 150, clear_delay_ms: 300, latched: false },
      hihi: { enabled: true, limit: 18.0, hysteresis: 0.5, trip_delay_ms: 50, clear_delay_ms: 500, latched: true },
    },
    breakage_drop_pct: 40,
    crash_rise_pct: 60,
    wear: { enabled: true, baseline_a: 11.5, adaptive_k_warn: 3 },
    cut_detect: { active_threshold_a: 4.5, debounce_ms: 50, require_machine_running: true }
  },
  {
    name: "Sub Spindle (Right)",
    enabled: true,
    pressure_unit: "bar",
    bands: {
      lolo: { enabled: true, limit: 1.0, hysteresis: 0.2, trip_delay_ms: 100, clear_delay_ms: 200, latched: true },
      lo: { enabled: true, limit: 2.0, hysteresis: 0.3, trip_delay_ms: 200, clear_delay_ms: 200, latched: false },
      hi: { enabled: true, limit: 9.5, hysteresis: 0.4, trip_delay_ms: 150, clear_delay_ms: 300, latched: false },
      hihi: { enabled: true, limit: 12.0, hysteresis: 0.5, trip_delay_ms: 50, clear_delay_ms: 500, latched: true },
    },
    breakage_drop_pct: 40,
    crash_rise_pct: 60,
    wear: { enabled: false, baseline_a: 7.5, adaptive_k_warn: 3 },
    cut_detect: { active_threshold_a: 3.0, debounce_ms: 50, require_machine_running: true }
  }
];

export const mockTemplates: JobTemplate[] = [
  {
    name: "Alu-6061-Roughing-D12",
    description: "12mm carbide rougher on 6061 aluminium billet",
    created_at: "2026-08-20 14:32",
    bands: { ...mockSpindleConfigs[0].bands }
  },
  {
    name: "Steel-4140-Finishing-D8",
    description: "8mm finishing ballnose on pre-hardened 4140",
    created_at: "2026-08-25 09:15",
    bands: { ...mockSpindleConfigs[1].bands }
  },
  {
    name: "Titanium-Ti6Al4V-Drill-D6.5",
    description: "Through-coolant carbide twist drill high pressure feed",
    created_at: "2026-09-01 17:40",
    bands: { ...mockSpindleConfigs[0].bands }
  }
];

export const mockSystemInfo: SystemInfo = {
  version: "v2.1.0-react",
  build_date: "Sep 12 2026",
  build_time: "12:00:00",
  idf_version: "v6.0.2-dirty",
  schema_version: 4,
  uptime_s: 14285,
  free_heap: 184520,
  min_free_heap: 142100,
  reset_reason: "Software reset (ESP_RST_SW)",
  running_partition: "ota_0 (Active)",
  update_partition: "ota_1 (Standby)",
  pending_verify: false,
};

export const mockCommsConfig: CommsConfig = {
  wifi: {
    sta_ssid: "Factory-Floor-WiFi-5G",
    sta_ip: "192.168.1.142",
    sta_connected: true,
    ap_ssid: "CNC-Tool-Monitor-AP",
    ap_ip: "192.168.4.1",
    rssi: -58,
  },
  modbus: {
    rtu_enabled: true,
    rtu_addr: 1,
    rtu_baud: 115200,
    rtu_parity: "none",
    tcp_enabled: true,
    tcp_port: 502,
    tcp_clients: 2,
  },
};

export const mockCalibState: CalibState = {
  target: "pressure",
  raw_v: 1.654,
  calibrated: 5.92,
  points_captured: 2,
  zero_offset_v: 1.650,
};
