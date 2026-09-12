import React from "react";
import { TelemetrySnapshot } from "../../types";
import { SpindleCard } from "./SpindleCard";
import { HardDrive, Server, Shield, CheckCircle2, RotateCw, Activity, Cpu, Layers } from "lucide-react";

interface DashboardViewProps {
  telemetry: TelemetrySnapshot | null;
}

export const DashboardView: React.FC<DashboardViewProps> = ({ telemetry }) => {
  if (!telemetry) {
    return (
      <div style={{ textAlign: "center", padding: "60px 0", color: "var(--text-muted)" }}>
        Loading real-time telemetry stream...
      </div>
    );
  }

  const s1 = telemetry.spindles[0];
  const s2 = telemetry.spindles[1];
  const totalCuts = (s1?.cycle_count || 0) + (s2?.cycle_count || 0);

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* Top 4-Column KPI Overview Banner (Eliminates empty space) */}
      <div className="kpi-grid">
        {/* KPI 1 */}
        <div className="card" style={{ padding: "14px 18px", display: "flex", alignItems: "center", gap: 14 }}>
          <div style={{
            width: 42,
            height: 42,
            borderRadius: "12px",
            background: "var(--blue-tint)",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            color: "var(--blue-primary)"
          }}>
            <Activity size={20} />
          </div>
          <div>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-muted)", textTransform: "uppercase" }}>
              Spindle 1 Load
            </span>
            <div style={{ display: "flex", alignItems: "baseline", gap: 6, marginTop: 2 }}>
              <span className="num" style={{ fontSize: 20, fontWeight: 800, color: "var(--text-primary)" }}>
                {s1?.current_a.toFixed(2)} A
              </span>
              <span className={`badge ${s1?.state === "CUTTING" ? "badge-ok" : "badge-muted"}`} style={{ fontSize: 10, padding: "2px 6px" }}>
                {s1?.state}
              </span>
            </div>
          </div>
        </div>

        {/* KPI 2 */}
        <div className="card" style={{ padding: "14px 18px", display: "flex", alignItems: "center", gap: 14 }}>
          <div style={{
            width: 42,
            height: 42,
            borderRadius: "12px",
            background: "var(--blue-tint)",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            color: "var(--blue-primary)"
          }}>
            <Activity size={20} />
          </div>
          <div>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-muted)", textTransform: "uppercase" }}>
              Spindle 2 Load
            </span>
            <div style={{ display: "flex", alignItems: "baseline", gap: 6, marginTop: 2 }}>
              <span className="num" style={{ fontSize: 20, fontWeight: 800, color: "var(--text-primary)" }}>
                {s2?.current_a.toFixed(2)} A
              </span>
              <span className={`badge ${s2?.state === "CUTTING" ? "badge-ok" : "badge-muted"}`} style={{ fontSize: 10, padding: "2px 6px" }}>
                {s2?.state}
              </span>
            </div>
          </div>
        </div>

        {/* KPI 3 */}
        <div className="card" style={{ padding: "14px 18px", display: "flex", alignItems: "center", gap: 14 }}>
          <div style={{
            width: 42,
            height: 42,
            borderRadius: "12px",
            background: "#dcfce7",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            color: "#16a34a"
          }}>
            <RotateCw size={20} />
          </div>
          <div>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-muted)", textTransform: "uppercase" }}>
              Machine Cut Count
            </span>
            <div style={{ display: "flex", alignItems: "baseline", gap: 6, marginTop: 2 }}>
              <span className="num" style={{ fontSize: 20, fontWeight: 800, color: "var(--text-primary)" }}>
                {totalCuts}
              </span>
              <span style={{ fontSize: 12, color: "var(--text-muted)", fontWeight: 600 }}>cuts today</span>
            </div>
          </div>
        </div>

        {/* KPI 4 */}
        <div className="card" style={{ padding: "14px 18px", display: "flex", alignItems: "center", gap: 14 }}>
          <div style={{
            width: 42,
            height: 42,
            borderRadius: "12px",
            background: "#f1f5f9",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            color: "var(--text-secondary)"
          }}>
            <Cpu size={20} />
          </div>
          <div>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-muted)", textTransform: "uppercase" }}>
              ESP32 Free Memory
            </span>
            <div style={{ display: "flex", alignItems: "baseline", gap: 6, marginTop: 2 }}>
              <span className="num" style={{ fontSize: 20, fontWeight: 800, color: "var(--text-primary)" }}>
                184 kB
              </span>
              <span style={{ fontSize: 11, color: "var(--status-ok)", fontWeight: 700 }}>Optimal</span>
            </div>
          </div>
        </div>
      </div>

      {/* Dual Spindles Grid - Side by Side */}
      <div className="grid-2">
        {telemetry.spindles.map((spindle, idx) => (
          <SpindleCard key={idx} spindle={spindle} index={idx} />
        ))}
      </div>

      {/* System Diagnostics Footer */}
      <div className="card" style={{ padding: "14px 20px" }}>
        <div style={{
          display: "flex",
          justifyContent: "space-between",
          alignItems: "center",
          flexWrap: "wrap",
          gap: 16
        }}>
          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <Server size={16} color="var(--blue-primary)" />
            <span style={{ fontSize: 13, fontWeight: 700, color: "var(--text-primary)" }}>Dual SMU Bus Status:</span>
            <span style={{ fontSize: 12, color: "var(--status-ok)", fontWeight: 700, display: "flex", alignItems: "center", gap: 4 }}>
              <CheckCircle2 size={13} /> Active Master Aggregator Link
            </span>
          </div>

          <div style={{ display: "flex", alignItems: "center", gap: 24, fontSize: 12, color: "var(--text-secondary)" }}>
            <span style={{ display: "flex", alignItems: "center", gap: 6 }}>
              <Shield size={14} color="var(--status-ok)" />
              ADS1115 ADC: <strong style={{ color: telemetry.adc_healthy ? "var(--status-ok)" : "var(--status-alarm)" }}>
                {telemetry.adc_healthy ? "Calibrated" : "Fault"}
              </strong>
            </span>

            <span style={{ display: "flex", alignItems: "center", gap: 6 }}>
              <HardDrive size={14} color="var(--blue-primary)" />
              Worst Loop Latency: <strong className="num" style={{ color: "var(--text-primary)" }}>
                {telemetry.worst_loop_ms} ms
              </strong>
            </span>

            <span style={{ display: "flex", alignItems: "center", gap: 6 }}>
              <Layers size={14} color="var(--blue-primary)" />
              Flash Partition: <strong style={{ color: "var(--text-primary)" }}>LittleFS Web (512 kB)</strong>
            </span>
          </div>
        </div>
      </div>
    </div>
  );
};
