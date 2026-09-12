import React from "react";
import { TelemetrySnapshot } from "../types";
import { Activity, Bell, Wifi, Cpu, CheckCircle, AlertTriangle, AlertOctagon } from "lucide-react";

interface HeaderProps {
  telemetry: TelemetrySnapshot | null;
  onAcknowledge: () => void;
  isAcknowledging: boolean;
}

export const Header: React.FC<HeaderProps> = ({ telemetry, onAcknowledge, isAcknowledging }) => {
  const formatUptime = (seconds: number) => {
    const d = Math.floor(seconds / 86400);
    const h = Math.floor((seconds % 86400) / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    if (d > 0) return `${d}d ${h}h`;
    if (h > 0) return `${h}h ${m}m`;
    return `${m}m ${s}s`;
  };

  const hasLatchedAlarms = telemetry?.spindles.some((s) => s.any_latched);
  const hasAlarms = telemetry?.spindles.some((s) => s.any_alarm);
  const hasWarnings = telemetry?.spindles.some((s) => s.any_warning);

  return (
    <header style={{
      background: "linear-gradient(135deg, #090d16 0%, #0f172a 60%, #1e293b 100%)",
      borderBottom: "1px solid rgba(255, 255, 255, 0.08)",
      padding: "12px 28px",
      position: "sticky",
      top: 0,
      zIndex: 100,
      boxShadow: "0 4px 20px rgba(15, 23, 42, 0.3)"
    }}>
      <div style={{
        maxWidth: 1360,
        margin: "0 auto",
        display: "flex",
        alignItems: "center",
        justifyContent: "space-between",
        flexWrap: "wrap",
        gap: 16
      }}>
        {/* Brand & Title */}
        <div style={{ display: "flex", alignItems: "center", gap: 14 }}>
          <div style={{
            width: 40,
            height: 40,
            borderRadius: "var(--radius-sm)",
            background: "linear-gradient(135deg, #3b82f6, #2563eb)",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            boxShadow: "0 0 16px rgba(37, 99, 235, 0.4)"
          }}>
            <Activity size={22} color="#ffffff" strokeWidth={2.5} />
          </div>
          <div>
            <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
              <h1 style={{ fontSize: 17, fontWeight: 700, letterSpacing: "-0.02em", color: "#ffffff", margin: 0 }}>
                CNC Tool Monitor
              </h1>
              <span style={{
                fontSize: 10,
                fontWeight: 700,
                background: "rgba(37, 99, 235, 0.2)",
                color: "#93c5fd",
                padding: "2px 7px",
                borderRadius: 4,
                border: "1px solid rgba(37, 99, 235, 0.4)",
                letterSpacing: "0.04em"
              }}>
                V2 MASTER
              </span>
            </div>
            <p style={{ fontSize: 11, color: "#94a3b8", margin: "2px 0 0" }}>
              Industrial Spindle Condition & Machine Supervisor
            </p>
          </div>
        </div>

        {/* System Health Indicators */}
        <div style={{ display: "flex", alignItems: "center", gap: 12, flexWrap: "wrap" }}>
          {/* Health Status */}
          {hasAlarms ? (
            <div className="badge badge-alarm" style={{ boxShadow: "0 0 12px rgba(220, 38, 38, 0.4)" }}>
              <AlertOctagon size={14} /> ALARM ACTIVE
            </div>
          ) : hasWarnings ? (
            <div className="badge badge-warn">
              <AlertTriangle size={14} /> WARNING
            </div>
          ) : (
            <div style={{
              display: "inline-flex",
              alignItems: "center",
              gap: 6,
              padding: "5px 12px",
              borderRadius: "9999px",
              fontSize: 11,
              fontWeight: 700,
              background: "rgba(22, 163, 74, 0.2)",
              color: "#4ade80",
              border: "1px solid rgba(22, 163, 74, 0.4)",
              letterSpacing: "0.04em"
            }}>
              <CheckCircle size={14} /> ALL SYSTEMS OK
            </div>
          )}

          {/* Loop / ADC Health */}
          {telemetry && (
            <div style={{
              display: "flex",
              alignItems: "center",
              gap: 8,
              background: "rgba(255, 255, 255, 0.08)",
              border: "1px solid rgba(255, 255, 255, 0.12)",
              borderRadius: "9999px",
              padding: "5px 14px",
              fontSize: 12,
              color: "#cbd5e1"
            }}>
              <Cpu size={14} color="#60a5fa" />
              <span>
                Loop: <strong className="num" style={{ color: "#ffffff" }}>{telemetry.loop_period_ms}ms</strong>
              </span>
              <span style={{ opacity: 0.3 }}>|</span>
              <span>
                Up: <strong className="num" style={{ color: "#ffffff" }}>{formatUptime(telemetry.uptime_s)}</strong>
              </span>
            </div>
          )}

          {/* Wi-Fi Pill */}
          <div style={{
            display: "flex",
            alignItems: "center",
            gap: 6,
            background: "rgba(255, 255, 255, 0.08)",
            border: "1px solid rgba(255, 255, 255, 0.12)",
            borderRadius: "9999px",
            padding: "5px 14px",
            fontSize: 12,
            color: "#cbd5e1"
          }}>
            <Wifi size={14} color="#4ade80" />
            <span style={{ color: "#ffffff" }}>Online</span>
          </div>

          {/* Latched Alarm Acknowledge Button */}
          {hasLatchedAlarms && (
            <button
              onClick={onAcknowledge}
              disabled={isAcknowledging}
              className="btn btn-danger btn-sm"
              title="Clear all acknowledged latches"
            >
              <Bell size={14} />
              {isAcknowledging ? "Clearing..." : "Acknowledge Latches"}
            </button>
          )}
        </div>
      </div>
    </header>
  );
};
