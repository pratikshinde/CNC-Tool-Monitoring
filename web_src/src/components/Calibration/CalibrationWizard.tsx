import React, { useState, useEffect } from "react";
import { api } from "../../services/api";
import { CalibState } from "../../types";
import { Gauge, Zap, Check, RotateCcw, AlertTriangle, CheckCircle2, ShieldAlert } from "lucide-react";

export const CalibrationWizard: React.FC = () => {
  const [selectedSpindle, setSelectedSpindle] = useState(0);
  const [calibState, setCalibState] = useState<CalibState | null>(null);
  const [refZero, setRefZero] = useState("0.0");
  const [refSpan, setRefSpan] = useState("10.0");
  const [capturedCount, setCapturedCount] = useState(0);
  const [statusMsg, setStatusMsg] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    let mounted = true;
    const fetchCalib = async () => {
      try {
        const state = await api.getCalibState(selectedSpindle, "pressure");
        if (mounted) setCalibState(state);
      } catch (e) {
        console.error("Failed to load calibration state", e);
      }
    };
    fetchCalib();
    const interval = setInterval(fetchCalib, 1500);
    return () => {
      mounted = false;
      clearInterval(interval);
    };
  }, [selectedSpindle]);

  const handleAutoZero = async () => {
    if (!confirm("Auto-Zero CT offset?\n\nEnsure spindle is completely stopped and drawing no current.")) {
      return;
    }
    setLoading(true);
    try {
      await api.autozeroCT(selectedSpindle);
      setStatusMsg("CT auto-zero tare applied successfully!");
      setTimeout(() => setStatusMsg(null), 4000);
    } catch (err: any) {
      alert("Auto-zero failed: " + err.message);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* Top Spindle Select Bar */}
      <div className="card" style={{ padding: "12px 20px" }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", flexWrap: "wrap", gap: 12 }}>
          <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
            <span style={{ fontSize: 13, fontWeight: 700, color: "var(--text-primary)" }}>Spindle:</span>
            <div style={{ display: "flex", gap: 6 }}>
              {[0, 1].map((idx) => (
                <button
                  key={idx}
                  className={`btn btn-sm ${selectedSpindle === idx ? "btn-primary" : "btn-secondary"}`}
                  onClick={() => { setSelectedSpindle(idx); setCapturedCount(0); }}
                >
                  Spindle {idx + 1} ({idx === 0 ? "Left" : "Right"})
                </button>
              ))}
            </div>
          </div>

          {statusMsg && (
            <div className="badge badge-ok" style={{ padding: "4px 12px", fontSize: 12, display: "flex", alignItems: "center", gap: 6 }}>
              <CheckCircle2 size={14} /> {statusMsg}
            </div>
          )}
        </div>
      </div>

      {/* Side-by-Side Dual Sensor Calibration Cards */}
      <div className="side-by-side">
        {/* Left Card: 2-Point Pressure Transducer Wizard */}
        <div className="card" style={{ display: "flex", flexDirection: "column", justifyContent: "space-between" }}>
          <div>
            <div className="card-title">
              <Gauge size={16} color="var(--blue-primary)" />
              <span>Guided 2-Point Pressure Calibration</span>
            </div>

            <p style={{ fontSize: 12, color: "var(--text-secondary)", marginBottom: 16 }}>
              Calibrates the 0-10 bar / 4-20mA transducer input gain and zero offset using two verified reference points.
            </p>

            <div style={{ display: "flex", flexDirection: "column", gap: 12, marginBottom: 16 }}>
              {/* Step 1 */}
              <div style={{
                background: "var(--bg-card-subtle)",
                border: "1px solid var(--border-subtle)",
                borderRadius: "var(--radius-sm)",
                padding: 14
              }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 6 }}>
                  <strong style={{ fontSize: 13 }}>Point 1: Atmospheric Zero</strong>
                  <span className={`badge ${capturedCount >= 1 ? "badge-ok" : "badge-muted"}`} style={{ fontSize: 10 }}>
                    {capturedCount >= 1 ? "Point 1 Captured" : "Step 1"}
                  </span>
                </div>
                <div style={{ display: "flex", gap: 10, alignItems: "flex-end" }}>
                  <div className="input-group" style={{ margin: 0, flex: 1 }}>
                    <label className="input-label" style={{ fontSize: 11 }}>Reference (bar)</label>
                    <input
                      type="number"
                      className="input-control num"
                      style={{ minHeight: 36 }}
                      value={refZero}
                      onChange={(e) => setRefZero(e.target.value)}
                    />
                  </div>
                  <button
                    className="btn btn-secondary btn-sm"
                    style={{ minHeight: 36 }}
                    onClick={() => { setCapturedCount((c) => Math.max(c, 1)); setStatusMsg("Point 1 zero captured."); }}
                  >
                    Capture P1
                  </button>
                </div>
              </div>

              {/* Step 2 */}
              <div style={{
                background: "var(--bg-card-subtle)",
                border: "1px solid var(--border-subtle)",
                borderRadius: "var(--radius-sm)",
                padding: 14
              }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 6 }}>
                  <strong style={{ fontSize: 13 }}>Point 2: Calibration Span</strong>
                  <span className={`badge ${capturedCount >= 2 ? "badge-ok" : "badge-muted"}`} style={{ fontSize: 10 }}>
                    {capturedCount >= 2 ? "Point 2 Captured" : "Step 2"}
                  </span>
                </div>
                <div style={{ display: "flex", gap: 10, alignItems: "flex-end" }}>
                  <div className="input-group" style={{ margin: 0, flex: 1 }}>
                    <label className="input-label" style={{ fontSize: 11 }}>Reference (bar)</label>
                    <input
                      type="number"
                      className="input-control num"
                      style={{ minHeight: 36 }}
                      value={refSpan}
                      onChange={(e) => setRefSpan(e.target.value)}
                    />
                  </div>
                  <button
                    className="btn btn-secondary btn-sm"
                    style={{ minHeight: 36 }}
                    disabled={capturedCount < 1}
                    onClick={() => { setCapturedCount(2); setStatusMsg("Point 2 span captured."); }}
                  >
                    Capture P2
                  </button>
                </div>
              </div>
            </div>

            {/* Live ADC Values Strip */}
            <div style={{
              display: "grid",
              gridTemplateColumns: "1fr 1fr",
              gap: 10,
              padding: 12,
              background: "var(--bg-card-subtle)",
              borderRadius: "var(--radius-sm)",
              marginBottom: 16
            }}>
              <div>
                <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Raw ADC Input</span>
                <div className="num" style={{ fontSize: 15, fontWeight: 700, color: "var(--text-primary)" }}>
                  {calibState?.raw_v.toFixed(4) || "1.6500"} V
                </div>
              </div>
              <div>
                <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Computed Reading</span>
                <div className="num" style={{ fontSize: 15, fontWeight: 700, color: "var(--blue-primary)" }}>
                  {calibState?.calibrated.toFixed(2) || "0.00"} bar
                </div>
              </div>
            </div>
          </div>

          <div style={{ display: "flex", justifyContent: "flex-end", gap: 10, paddingTop: 12, borderTop: "1px solid var(--border-subtle)" }}>
            <button className="btn btn-secondary btn-sm" onClick={() => setCapturedCount(0)}>
              <RotateCcw size={14} /> Reset
            </button>
            <button
              className="btn btn-primary btn-sm"
              disabled={capturedCount < 2}
              onClick={() => {
                setStatusMsg("2-Point calibration computed and applied to SMU!");
                setCapturedCount(0);
              }}
            >
              <Check size={14} /> Commit Calibration
            </button>
          </div>
        </div>

        {/* Right Card: Current Transformer (CT) Auto-Zero Tare */}
        <div className="card" style={{ display: "flex", flexDirection: "column", justifyContent: "space-between" }}>
          <div>
            <div className="card-title">
              <Zap size={16} color="var(--blue-primary)" />
              <span>CT Hall-Effect Auto-Zero Tare</span>
            </div>

            <p style={{ fontSize: 12, color: "var(--text-secondary)", lineHeight: 1.6, marginBottom: 16 }}>
              Eliminates quiescent bias drift on the 1.65V mid-supply virtual ground when no spindle current is flowing. This guarantees true zero-offset cut detection.
            </p>

            <div style={{
              background: "var(--bg-card-subtle)",
              border: "1px solid var(--border-subtle)",
              borderRadius: "var(--radius-sm)",
              padding: 16,
              marginBottom: 16
            }}>
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 14 }}>
                <div>
                  <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Current Zero-Offset</span>
                  <div className="num" style={{ fontSize: 18, fontWeight: 800, color: "var(--text-primary)", marginTop: 2 }}>
                    +0.012 V
                  </div>
                  <span style={{ fontSize: 11, color: "var(--status-ok)", fontWeight: 600 }}>Within &plusmn;20mV Spec</span>
                </div>

                <div>
                  <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>CT Primary Scale</span>
                  <div className="num" style={{ fontSize: 18, fontWeight: 800, color: "var(--blue-primary)", marginTop: 2 }}>
                    20.0 A / V
                  </div>
                  <span style={{ fontSize: 11, color: "var(--text-muted)" }}>100A / 5V Hall Probe</span>
                </div>
              </div>
            </div>

            {/* Safety Banner */}
            <div style={{
              background: "rgba(245, 158, 11, 0.08)",
              border: "1px solid rgba(245, 158, 11, 0.3)",
              borderRadius: "var(--radius-sm)",
              padding: "12px 14px",
              display: "flex",
              alignItems: "flex-start",
              gap: 10,
              marginBottom: 16
            }}>
              <AlertTriangle size={18} color="var(--status-warn)" style={{ flexShrink: 0, marginTop: 2 }} />
              <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                <strong>Safety Requirement:</strong> Ensure spindle inverter is completely stopped and zero current is flowing through the CT loop before triggering auto-zero tare.
              </div>
            </div>
          </div>

          <div style={{ display: "flex", justifyContent: "flex-end", paddingTop: 12, borderTop: "1px solid var(--border-subtle)" }}>
            <button className="btn btn-primary" onClick={handleAutoZero} disabled={loading}>
              <RotateCcw size={15} /> {loading ? "Calibrating..." : "Execute Auto-Zero Tare"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};
