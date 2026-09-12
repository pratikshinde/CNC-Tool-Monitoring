import React, { useState, useEffect, useRef } from "react";
import { api } from "../../services/api";
import { HistoryData, TelemetrySnapshot } from "../../types";
import { LineChart, Zap, Gauge, Disc, Activity, Layers, Clock } from "lucide-react";

interface TrendViewProps {
  telemetry: TelemetrySnapshot | null;
}

export const TrendView: React.FC<TrendViewProps> = ({ telemetry }) => {
  const [selectedSpindle, setSelectedSpindle] = useState<number>(0);
  const [selectedChannel, setSelectedChannel] = useState<"current_a" | "pressure" | "rpm">("current_a");
  const [windowSec, setWindowSec] = useState<number>(60);
  const [history, setHistory] = useState<HistoryData | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  useEffect(() => {
    let mounted = true;
    const fetchHistory = async () => {
      try {
        const data = await api.getHistory(selectedSpindle);
        if (mounted) setHistory(data);
      } catch (err) {
        console.error("Failed to load history", err);
      }
    };

    fetchHistory();
    const interval = setInterval(fetchHistory, 2500);
    return () => {
      mounted = false;
      clearInterval(interval);
    };
  }, [selectedSpindle]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !history) return;

    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    canvas.width = width * dpr;
    canvas.height = height * dpr;
    ctx.scale(dpr, dpr);

    ctx.clearRect(0, 0, width, height);

    let rawData = history[selectedChannel] || [];
    if (rawData.length > windowSec) {
      rawData = rawData.slice(rawData.length - windowSec);
    }

    const padLeft = 60;
    const padRight = 24;
    const padTop = 20;
    const padBottom = 35;
    const plotW = width - padLeft - padRight;
    const plotH = height - padTop - padBottom;

    if (rawData.length < 2) {
      ctx.fillStyle = "#64748b";
      ctx.font = "13px Inter, sans-serif";
      ctx.textAlign = "center";
      ctx.fillText("Acquiring high-resolution sensor trend...", width / 2, height / 2);
      return;
    }

    let minVal = Math.min(...rawData);
    let maxVal = Math.max(...rawData);
    if (maxVal - minVal < 0.001) {
      maxVal += 1;
      minVal -= 1;
    }
    const pad = (maxVal - minVal) * 0.15;
    minVal = Math.max(0, minVal - pad);
    maxVal = maxVal + pad;

    const getX = (i: number) => padLeft + (i / (rawData.length - 1)) * plotW;
    const getY = (val: number) => padTop + plotH - ((val - minVal) / (maxVal - minVal)) * plotH;

    // Gridlines & Y-axis labels
    ctx.strokeStyle = "#e2e8f0";
    ctx.lineWidth = 1;
    ctx.font = "11px 'JetBrains Mono', monospace";
    ctx.fillStyle = "#64748b";
    ctx.textAlign = "right";

    const steps = 4;
    for (let i = 0; i <= steps; i++) {
      const v = minVal + (maxVal - minVal) * (i / steps);
      const y = Math.round(getY(v)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(padLeft, y);
      ctx.lineTo(width - padRight, y);
      ctx.stroke();

      let label = v.toFixed(1);
      if (selectedChannel === "rpm") label = Math.round(v).toString();
      ctx.fillText(label, padLeft - 8, y + 4);
    }

    // X-axis Time Ago labels
    ctx.textAlign = "center";
    for (let i = 0; i <= steps; i++) {
      const idx = Math.round(((rawData.length - 1) * i) / steps);
      const x = getX(idx);
      const secAgo = rawData.length - 1 - idx;
      ctx.fillText(secAgo === 0 ? "now" : `-${secAgo}s`, x, height - 10);
    }

    // Gradient fill under curve
    const gradient = ctx.createLinearGradient(0, padTop, 0, padTop + plotH);
    gradient.addColorStop(0, "rgba(37, 99, 235, 0.22)");
    gradient.addColorStop(1, "rgba(37, 99, 235, 0.0)");

    ctx.beginPath();
    ctx.moveTo(getX(0), padTop + plotH);
    rawData.forEach((val, i) => {
      ctx.lineTo(getX(i), getY(val));
    });
    ctx.lineTo(getX(rawData.length - 1), padTop + plotH);
    ctx.closePath();
    ctx.fillStyle = gradient;
    ctx.fill();

    // The Stroke Line (Electric Blue)
    ctx.strokeStyle = "#2563eb";
    ctx.lineWidth = 2.5;
    ctx.beginPath();
    rawData.forEach((val, i) => {
      if (i === 0) ctx.moveTo(getX(i), getY(val));
      else ctx.lineTo(getX(i), getY(val));
    });
    ctx.stroke();

    // Highlight Current point
    const lastX = getX(rawData.length - 1);
    const lastY = getY(rawData[rawData.length - 1]);
    ctx.fillStyle = "#2563eb";
    ctx.beginPath();
    ctx.arc(lastX, lastY, 5, 0, Math.PI * 2);
    ctx.fill();
  }, [history, selectedChannel, windowSec]);

  const s1 = telemetry?.spindles[0];
  const s2 = telemetry?.spindles[1];
  const currentSpindle = telemetry?.spindles[selectedSpindle];
  const channelData = history ? history[selectedChannel] || [] : [];
  const currVal = channelData.length > 0 ? channelData[channelData.length - 1] : 0;
  const minVal = channelData.length > 0 ? Math.min(...channelData) : 0;
  const maxVal = channelData.length > 0 ? Math.max(...channelData) : 0;
  const avgVal = channelData.length > 0 ? channelData.reduce((a, b) => a + b, 0) / channelData.length : 0;

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* Top Header Filter Bar */}
      <div className="card" style={{ padding: "12px 20px" }}>
        <div style={{
          display: "flex",
          justifyContent: "space-between",
          alignItems: "center",
          flexWrap: "wrap",
          gap: 14
        }}>
          <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
            <span style={{ fontSize: 13, fontWeight: 700, color: "var(--text-primary)" }}>Spindle:</span>
            <div style={{ display: "flex", gap: 6 }}>
              {[0, 1].map((sIdx) => (
                <button
                  key={sIdx}
                  className={`btn btn-sm ${selectedSpindle === sIdx ? "btn-primary" : "btn-secondary"}`}
                  onClick={() => setSelectedSpindle(sIdx)}
                >
                  Spindle {sIdx + 1} ({sIdx === 0 ? "Left" : "Right"})
                </button>
              ))}
            </div>
            <span className={`badge ${currentSpindle?.state === "CUTTING" ? "badge-ok" : "badge-muted"}`} style={{ marginLeft: 8 }}>
              {currentSpindle?.state || "IDLE"}
            </span>
          </div>

          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <Clock size={15} color="var(--text-muted)" />
            <span style={{ fontSize: 13, fontWeight: 700, color: "var(--text-secondary)" }}>Window:</span>
            {[30, 60, 120, 300].map((sec) => (
              <button
                key={sec}
                className={`btn btn-sm ${windowSec === sec ? "btn-primary" : "btn-secondary"}`}
                onClick={() => setWindowSec(sec)}
              >
                {sec >= 60 ? `${sec / 60}m` : `${sec}s`}
              </button>
            ))}
          </div>
        </div>
      </div>

      {/* Main Side-by-Side High Density Layout */}
      <div className="trends-layout">
        {/* Left Column: High-DPI Waveform Chart */}
        <div className="card" style={{ display: "flex", flexDirection: "column", minHeight: 460 }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 16 }}>
            <div className="card-title" style={{ margin: 0 }}>
              <LineChart size={17} color="var(--blue-primary)" />
              <span>
                Spindle {selectedSpindle + 1} - {selectedChannel === "current_a" ? "Current Waveform Stream" : selectedChannel === "pressure" ? "Hydraulic Pressure Load" : "Spindle RPM"}
              </span>
            </div>

            <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
              <span style={{ fontSize: 12, color: "var(--text-muted)" }}>Live:</span>
              <span className="badge badge-ok num" style={{ fontSize: 14, padding: "4px 10px" }}>
                {currVal.toFixed(2)} {selectedChannel === "current_a" ? "A" : selectedChannel === "pressure" ? (currentSpindle?.pressure_unit || "bar") : "RPM"}
              </span>
            </div>
          </div>

          <div style={{ flex: 1, width: "100%", minHeight: 340, position: "relative" }}>
            <canvas
              ref={canvasRef}
              style={{ width: "100%", height: "100%", display: "block" }}
            />
          </div>

          <div style={{
            display: "grid",
            gridTemplateColumns: "repeat(4, 1fr)",
            gap: 12,
            marginTop: 16,
            paddingTop: 14,
            borderTop: "1px solid var(--border-subtle)"
          }}>
            <div>
              <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Minimum</span>
              <div className="num" style={{ fontSize: 15, fontWeight: 700, color: "var(--text-primary)" }}>{minVal.toFixed(2)}</div>
            </div>
            <div>
              <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Average</span>
              <div className="num" style={{ fontSize: 15, fontWeight: 700, color: "var(--text-primary)" }}>{avgVal.toFixed(2)}</div>
            </div>
            <div>
              <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Peak / Max</span>
              <div className="num" style={{ fontSize: 15, fontWeight: 700, color: "var(--blue-primary)" }}>{maxVal.toFixed(2)}</div>
            </div>
            <div>
              <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Delta (P-P)</span>
              <div className="num" style={{ fontSize: 15, fontWeight: 700, color: "var(--status-warn)" }}>{(maxVal - minVal).toFixed(2)}</div>
            </div>
          </div>
        </div>

        {/* Right Column: Telemetry Sidebar & Stream Controls */}
        <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
          <div className="card">
            <div className="card-title">
              <Layers size={16} color="var(--blue-primary)" />
              <span>Select Active Waveform</span>
            </div>

            <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
              <button
                className={`btn ${selectedChannel === "current_a" ? "btn-primary" : "btn-secondary"}`}
                style={{ justifyContent: "flex-start", width: "100%" }}
                onClick={() => setSelectedChannel("current_a")}
              >
                <Zap size={16} />
                <span style={{ flex: 1, textAlign: "left" }}>Motor Current (A)</span>
                <span className="num" style={{ fontSize: 12, opacity: 0.9 }}>{currentSpindle?.current_a.toFixed(2)} A</span>
              </button>

              <button
                className={`btn ${selectedChannel === "pressure" ? "btn-primary" : "btn-secondary"}`}
                style={{ justifyContent: "flex-start", width: "100%" }}
                onClick={() => setSelectedChannel("pressure")}
              >
                <Gauge size={16} />
                <span style={{ flex: 1, textAlign: "left" }}>Coolant / Load</span>
                <span className="num" style={{ fontSize: 12, opacity: 0.9 }}>{currentSpindle?.pressure.toFixed(1)} {currentSpindle?.pressure_unit || "bar"}</span>
              </button>

              <button
                className={`btn ${selectedChannel === "rpm" ? "btn-primary" : "btn-secondary"}`}
                style={{ justifyContent: "flex-start", width: "100%" }}
                onClick={() => setSelectedChannel("rpm")}
              >
                <Disc size={16} />
                <span style={{ flex: 1, textAlign: "left" }}>Spindle Speed (RPM)</span>
                <span className="num" style={{ fontSize: 12, opacity: 0.9 }}>{currentSpindle?.rpm.toLocaleString()}</span>
              </button>
            </div>
          </div>

          <div className="card">
            <div className="card-title">
              <Activity size={16} color="var(--blue-primary)" />
              <span>Instantaneous Telemetry</span>
            </div>

            <div style={{ display: "flex", flexDirection: "column", gap: 14 }}>
              <div>
                <div style={{ display: "flex", justifyContent: "space-between", fontSize: 12, marginBottom: 4 }}>
                  <span style={{ color: "var(--text-secondary)", fontWeight: 600 }}>Spindle 1 Load</span>
                  <span className="num" style={{ fontWeight: 700, color: "var(--text-primary)" }}>{s1?.current_a.toFixed(2)} A</span>
                </div>
                <div style={{ width: "100%", height: 6, background: "var(--bg-card-subtle)", borderRadius: 3, overflow: "hidden" }}>
                  <div style={{
                    width: `${Math.min(100, ((s1?.current_a || 0) / 25) * 100)}%`,
                    height: "100%",
                    background: "var(--blue-primary)",
                    borderRadius: 3,
                    transition: "width 0.25s ease"
                  }} />
                </div>
              </div>

              <div>
                <div style={{ display: "flex", justifyContent: "space-between", fontSize: 12, marginBottom: 4 }}>
                  <span style={{ color: "var(--text-secondary)", fontWeight: 600 }}>Spindle 2 Load</span>
                  <span className="num" style={{ fontWeight: 700, color: "var(--text-primary)" }}>{s2?.current_a.toFixed(2)} A</span>
                </div>
                <div style={{ width: "100%", height: 6, background: "var(--bg-card-subtle)", borderRadius: 3, overflow: "hidden" }}>
                  <div style={{
                    width: `${Math.min(100, ((s2?.current_a || 0) / 25) * 100)}%`,
                    height: "100%",
                    background: "var(--blue-primary)",
                    borderRadius: 3,
                    transition: "width 0.25s ease"
                  }} />
                </div>
              </div>

              <div>
                <div style={{ display: "flex", justifyContent: "space-between", fontSize: 12, marginBottom: 4 }}>
                  <span style={{ color: "var(--text-secondary)", fontWeight: 600 }}>Coolant Pressure</span>
                  <span className="num" style={{ fontWeight: 700, color: "var(--text-primary)" }}>{currentSpindle?.pressure.toFixed(1)} bar</span>
                </div>
                <div style={{ width: "100%", height: 6, background: "var(--bg-card-subtle)", borderRadius: 3, overflow: "hidden" }}>
                  <div style={{
                    width: `${Math.min(100, ((currentSpindle?.pressure || 0) / 10) * 100)}%`,
                    height: "100%",
                    background: "#0ea5e9",
                    borderRadius: 3,
                    transition: "width 0.25s ease"
                  }} />
                </div>
              </div>

              <div style={{
                background: "var(--bg-card-subtle)",
                borderRadius: "var(--radius-sm)",
                padding: "10px 12px",
                display: "flex",
                justifyContent: "space-between",
                alignItems: "center",
                marginTop: 4
              }}>
                <span style={{ fontSize: 12, color: "var(--text-muted)", fontWeight: 600 }}>Sampling Engine</span>
                <span style={{ fontSize: 12, fontWeight: 700, color: "var(--text-primary)" }}>10 Hz RTOS Stream</span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};
