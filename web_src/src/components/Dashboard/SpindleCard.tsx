import React from "react";
import { SpindleSnapshot } from "../../types";
import { Zap, Gauge, Disc, ShieldCheck, ShieldAlert, AlertTriangle, AlertCircle, Clock, RotateCw } from "lucide-react";

interface SpindleCardProps {
  spindle: SpindleSnapshot;
  index: number;
}

export const SpindleCard: React.FC<SpindleCardProps> = ({ spindle, index }) => {
  const isCutting = spindle.state === "CUTTING";
  const isRunning = spindle.state === "RUNNING";

  const getStateBadge = (state: string) => {
    switch (state) {
      case "CUTTING":
        return (
          <span className="badge badge-ok" style={{ boxShadow: "0 2px 8px rgba(22, 163, 74, 0.25)" }}>
            <span className="pulse-dot" /> CUTTING
          </span>
        );
      case "RUNNING":
        return (
          <span className="badge badge-ok" style={{ background: "#e0f2fe", color: "#0369a1", borderColor: "#bae6fd" }}>
            <span className="pulse-dot" /> RUNNING
          </span>
        );
      case "SPINUP":
      case "SPINDOWN":
        return <span className="badge badge-warn">{state}</span>;
      case "ERROR":
        return (
          <span className="badge badge-alarm">
            <span className="pulse-dot" /> ERROR
          </span>
        );
      default:
        return <span className="badge badge-muted">IDLE</span>;
    }
  };

  const getSeverityBadge = (severity: string) => {
    if (severity === "none") return null;
    if (severity === "warning" || severity === "wear trend") {
      return <span className="badge badge-warn"><AlertTriangle size={12} /> {severity}</span>;
    }
    return <span className="badge badge-alarm"><AlertCircle size={12} /> {severity}</span>;
  };

  // Compute metric visual percentages for mini level bars
  const currentPct = Math.min(100, Math.max(4, (spindle.current_a / 20) * 100));
  const pressurePct = Math.min(100, Math.max(4, (spindle.pressure / 10) * 100));
  const rpmPct = Math.min(100, Math.max(4, (spindle.rpm / 15000) * 100));

  return (
    <div className="card" style={{
      display: "flex",
      flexDirection: "column",
      gap: 18,
      borderTop: `4px solid ${isCutting ? "var(--status-ok)" : "var(--blue-primary)"}`,
      position: "relative"
    }}>
      {/* Header */}
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "flex-start", flexWrap: "wrap", gap: 10 }}>
        <div>
          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <span style={{
              fontSize: 11,
              fontWeight: 800,
              color: "#ffffff",
              background: "linear-gradient(135deg, #2563eb, #1d4ed8)",
              padding: "4px 10px",
              borderRadius: "9999px",
              letterSpacing: "0.04em",
              boxShadow: "0 2px 6px rgba(37, 99, 235, 0.3)"
            }}>
              SPINDLE {index + 1}
            </span>
            <h2 style={{ fontSize: 17, fontWeight: 700, margin: 0, color: "var(--text-primary)" }}>
              {spindle.name}
            </h2>
          </div>
          <span style={{ fontSize: 12, color: "var(--text-muted)", marginTop: 3, display: "block" }}>
            {spindle.enabled ? "Active Continuous Monitoring Channel" : "Channel Disabled"}
          </span>
        </div>

        <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
          {getSeverityBadge(spindle.severity)}
          {getStateBadge(spindle.state)}
        </div>
      </div>

      {/* Primary Metrics 3-Column Grid */}
      <div className="spindle-metrics-grid">
        {/* Motor Current */}
        <div style={{
          background: "var(--bg-card-subtle)",
          border: "1px solid var(--border-subtle)",
          borderRadius: "16px",
          padding: "16px",
          display: "flex",
          flexDirection: "column",
          justifyContent: "space-between",
          gap: 10,
          boxShadow: "0 2px 6px rgba(15, 23, 42, 0.02)"
        }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-secondary)", textTransform: "uppercase", letterSpacing: "0.04em" }}>
              Motor Current
            </span>
            <div style={{
              width: 28,
              height: 28,
              borderRadius: "50%",
              background: "linear-gradient(135deg, #3b82f6, #2563eb)",
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              color: "#ffffff",
              boxShadow: "0 2px 6px rgba(37, 99, 235, 0.25)"
            }}>
              <Zap size={14} />
            </div>
          </div>

          <div>
            <div style={{ display: "flex", alignItems: "baseline", gap: 4 }}>
              <span className="num" style={{ fontSize: 30, fontWeight: 800, color: "var(--text-primary)", letterSpacing: "-0.03em" }}>
                {spindle.current_a.toFixed(2)}
              </span>
              <span style={{ fontSize: 14, color: "var(--text-muted)", fontWeight: 600 }}>A</span>
            </div>

            {/* Load Progress Bar */}
            <div style={{ width: "100%", height: 6, background: "var(--border-subtle)", borderRadius: "9999px", marginTop: 8, overflow: "hidden" }}>
              <div style={{
                width: `${currentPct}%`,
                height: "100%",
                background: spindle.current_a > 15 ? "var(--status-warn)" : "linear-gradient(90deg, #60a5fa, #2563eb)",
                borderRadius: "9999px",
                transition: "width 0.6s cubic-bezier(0.34, 1.56, 0.64, 1)"
              }} />
            </div>
          </div>

          <div style={{ fontSize: 11, color: "var(--text-muted)", display: "flex", justifyContent: "space-between", paddingTop: 6, borderTop: "1px dashed var(--border-subtle)" }}>
            <span>Avg: <strong className="num" style={{ color: "var(--text-secondary)" }}>{spindle.current_avg_a.toFixed(2)}A</strong></span>
            <span>Peak: <strong className="num" style={{ color: "var(--text-secondary)" }}>{spindle.current_peak_a.toFixed(2)}A</strong></span>
          </div>
        </div>

        {/* Coolant Pressure */}
        <div style={{
          background: "var(--bg-card-subtle)",
          border: "1px solid var(--border-subtle)",
          borderRadius: "16px",
          padding: "16px",
          display: "flex",
          flexDirection: "column",
          justifyContent: "space-between",
          gap: 10,
          boxShadow: "0 2px 6px rgba(15, 23, 42, 0.02)"
        }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-secondary)", textTransform: "uppercase", letterSpacing: "0.04em" }}>
              Coolant / Load
            </span>
            <div style={{
              width: 28,
              height: 28,
              borderRadius: "50%",
              background: "linear-gradient(135deg, #0284c7, #0369a1)",
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              color: "#ffffff",
              boxShadow: "0 2px 6px rgba(2, 132, 199, 0.25)"
            }}>
              <Gauge size={14} />
            </div>
          </div>

          <div>
            <div style={{ display: "flex", alignItems: "baseline", gap: 4 }}>
              <span className="num" style={{ fontSize: 30, fontWeight: 800, color: "var(--text-primary)", letterSpacing: "-0.03em" }}>
                {spindle.pressure.toFixed(1)}
              </span>
              <span style={{ fontSize: 14, color: "var(--text-muted)", fontWeight: 600 }}>
                {spindle.pressure_unit}
              </span>
            </div>

            {/* Pressure Gauge Bar */}
            <div style={{ width: "100%", height: 6, background: "var(--border-subtle)", borderRadius: "9999px", marginTop: 8, overflow: "hidden" }}>
              <div style={{
                width: `${pressurePct}%`,
                height: "100%",
                background: "linear-gradient(90deg, #38bdf8, #0284c7)",
                borderRadius: "9999px",
                transition: "width 0.6s cubic-bezier(0.34, 1.56, 0.64, 1)"
              }} />
            </div>
          </div>

          <div style={{ fontSize: 11, color: "var(--text-muted)", display: "flex", justifyContent: "space-between", paddingTop: 6, borderTop: "1px dashed var(--border-subtle)" }}>
            <span>Sensor Status</span>
            <strong style={{ color: spindle.pressure_status === "ok" ? "var(--status-ok)" : "var(--status-alarm)", fontWeight: 700 }}>
              {spindle.pressure_status.toUpperCase()}
            </strong>
          </div>
        </div>

        {/* Spindle Speed (Animated Spinner Rotor) */}
        <div style={{
          background: "var(--bg-card-subtle)",
          border: "1px solid var(--border-subtle)",
          borderRadius: "16px",
          padding: "16px",
          display: "flex",
          flexDirection: "column",
          justifyContent: "space-between",
          gap: 10,
          boxShadow: "0 2px 6px rgba(15, 23, 42, 0.02)"
        }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
            <span style={{ fontSize: 11, fontWeight: 700, color: "var(--text-secondary)", textTransform: "uppercase", letterSpacing: "0.04em" }}>
              Spindle Speed
            </span>
            <div style={{
              width: 28,
              height: 28,
              borderRadius: "50%",
              background: "linear-gradient(135deg, #8b5cf6, #7c3aed)",
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              color: "#ffffff",
              boxShadow: "0 2px 6px rgba(124, 58, 237, 0.25)"
            }}>
              <Disc size={14} className={isCutting ? "spin-cutting" : isRunning ? "spin-running" : ""} />
            </div>
          </div>

          <div>
            <div style={{ display: "flex", alignItems: "baseline", gap: 4 }}>
              <span className="num" style={{ fontSize: 30, fontWeight: 800, color: "var(--text-primary)", letterSpacing: "-0.03em" }}>
                {Math.round(spindle.rpm).toLocaleString()}
              </span>
              <span style={{ fontSize: 14, color: "var(--text-muted)", fontWeight: 600 }}>RPM</span>
            </div>

            {/* RPM Gauge Bar */}
            <div style={{ width: "100%", height: 6, background: "var(--border-subtle)", borderRadius: "9999px", marginTop: 8, overflow: "hidden" }}>
              <div style={{
                width: `${rpmPct}%`,
                height: "100%",
                background: "linear-gradient(90deg, #a78bfa, #7c3aed)",
                borderRadius: "9999px",
                transition: "width 0.6s cubic-bezier(0.34, 1.56, 0.64, 1)"
              }} />
            </div>
          </div>

          <div style={{ fontSize: 11, color: "var(--text-muted)", display: "flex", justifyContent: "space-between", paddingTop: 6, borderTop: "1px dashed var(--border-subtle)" }}>
            <span>Tachometer</span>
            <strong style={{ color: spindle.rpm_sensor_suspect ? "var(--status-warn)" : "var(--status-ok)", fontWeight: 700 }}>
              {spindle.rpm_sensor_suspect ? "SUSPECT" : "SYNCHRONIZED"}
            </strong>
          </div>
        </div>
      </div>

      {/* Cycle Statistics & Armed State */}
      <div style={{
        background: "var(--bg-card-subtle)",
        borderRadius: "14px",
        border: "1px solid var(--border-subtle)",
        padding: "14px 18px",
        display: "grid",
        gridTemplateColumns: "repeat(auto-fit, minmax(180px, 1fr))",
        gap: 14,
        fontSize: 12
      }}>
        <div>
          <span style={{ color: "var(--text-muted)", display: "flex", alignItems: "center", gap: 6 }}>
            {spindle.monitoring_armed ? <ShieldCheck size={15} color="var(--status-ok)" /> : <ShieldAlert size={15} color="var(--text-muted)" />}
            Monitoring Engine
          </span>
          <div style={{ fontWeight: 700, color: spindle.monitoring_armed ? "var(--status-ok)" : "var(--text-muted)", marginTop: 3, fontSize: 13 }}>
            {spindle.monitoring_armed ? "ARMED (REALTIME SAFETY EVAL)" : "DISARMED (BELOW CUT THRESHOLD)"}
          </div>
        </div>

        <div>
          <span style={{ color: "var(--text-muted)", display: "flex", alignItems: "center", gap: 6 }}>
            <RotateCw size={15} /> Total Machine Cuts
          </span>
          <div className="num" style={{ fontWeight: 700, color: "var(--text-primary)", marginTop: 3, fontSize: 13 }}>
            {spindle.cycle_count} cycles completed
          </div>
        </div>

        {spindle.cycle_count > 0 && (
          <div>
            <span style={{ color: "var(--text-muted)", display: "flex", alignItems: "center", gap: 6 }}>
              <Clock size={15} /> Last Cycle Duration & Mean
            </span>
            <div className="num" style={{ fontWeight: 700, color: "var(--text-primary)", marginTop: 3, fontSize: 13 }}>
              {(spindle.last_cycle_ms / 1000).toFixed(1)}s @ {spindle.last_cycle_mean_a.toFixed(2)}A
            </div>
          </div>
        )}
      </div>

      {/* Latched / Alarm Fault Banner */}
      {spindle.any_latched && (
        <div style={{
          background: "var(--status-alarm-bg)",
          border: "1px solid var(--status-alarm-border)",
          borderRadius: "12px",
          padding: "12px 16px",
          color: "var(--status-alarm)",
          fontSize: 13,
          fontWeight: 700,
          display: "flex",
          alignItems: "center",
          gap: 10,
          boxShadow: "0 4px 14px rgba(220, 38, 38, 0.2)"
        }}>
          <AlertCircle size={18} />
          <span>LATCHED FAULT DETECTED — Safety cutoff active. Manual acknowledgment required.</span>
        </div>
      )}
    </div>
  );
};
