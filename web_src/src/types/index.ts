export interface SpindleSnapshot {
  name: string;
  enabled: boolean;
  state: "IDLE" | "SPINUP" | "RUNNING" | "CUTTING" | "SPINDOWN" | "ERROR" | string;
  monitoring_armed: boolean;
  current_a: number;
  current_avg_a: number;
  current_peak_a: number;
  pressure: number;
  pressure_unit: "bar" | "psi" | string;
  rpm: number;
  severity: "none" | "diagnostic" | "wear trend" | "warning" | "alarm" | "tool breakage" | "crash" | string;
  any_alarm: boolean;
  any_warning: boolean;
  any_latched: boolean;
  breakage: boolean;
  crash: boolean;
  trend: boolean;
  current_status: string;
  pressure_status: string;
  rpm_sensor_suspect: boolean;
  cycle_count: number;
  last_cycle_ms: number;
  last_cycle_mean_a: number;
}

export interface TelemetrySnapshot {
  system_healthy: boolean;
  adc_healthy: boolean;
  diagnostic_fault: boolean;
  loop_period_ms: number;
  worst_loop_ms: number;
  uptime_s: number;
  spindles: SpindleSnapshot[];
}

export interface BandConfig {
  enabled: boolean;
  limit: number;
  hysteresis: number;
  trip_delay_ms: number;
  clear_delay_ms: number;
  latched: boolean;
}

export interface SpindleConfig {
  name: string;
  enabled: boolean;
  bands: {
    lolo: BandConfig;
    lo: BandConfig;
    hi: BandConfig;
    hihi: BandConfig;
  };
  breakage_drop_pct: number;
  crash_rise_pct: number;
  wear: {
    enabled: boolean;
    baseline_a: number;
    adaptive_k_warn: number;
  };
  cut_detect: {
    active_threshold_a: number;
    debounce_ms: number;
    require_machine_running: boolean;
  };
  pressure_unit: string;
}

export interface HistoryData {
  current_a: number[];
  pressure: number[];
  rpm: number[];
}

export interface JobTemplate {
  name: string;
  description?: string;
  bands?: SpindleConfig["bands"];
  breakage_drop_pct?: number;
  crash_rise_pct?: number;
  cut_detect?: SpindleConfig["cut_detect"];
  wear?: SpindleConfig["wear"];
  created_at?: string;
}

export interface SystemInfo {
  version: string;
  build_date: string;
  build_time: string;
  idf_version: string;
  schema_version: number;
  uptime_s: number;
  free_heap: number;
  min_free_heap: number;
  reset_reason: string;
  running_partition: string;
  update_partition: string;
  pending_verify: boolean;
}

export interface CommsConfig {
  wifi: {
    sta_ssid: string;
    sta_pass?: string;
    sta_ip: string;
    sta_connected: boolean;
    ap_ssid: string;
    ap_pass?: string;
    ap_ip: string;
    rssi: number;
  };
  modbus: {
    rtu_enabled: boolean;
    rtu_addr: number;
    rtu_baud: number;
    rtu_parity: "none" | "even" | "odd";
    tcp_enabled: boolean;
    tcp_port: number;
    tcp_clients: number;
  };
}

export interface CalibState {
  target: "pressure" | "current";
  raw_v: number;
  calibrated: number;
  points_captured: number;
  zero_offset_v: number;
}
