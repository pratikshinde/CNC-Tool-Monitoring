import React, { useState, useEffect } from "react";
import { api } from "../../services/api";
import { SpindleConfig, BandConfig, TelemetrySnapshot } from "../../types";
import { Sliders, Save, AlertCircle, ChevronDown, ChevronUp, Check, ShieldAlert, Zap, Activity, CheckCircle2 } from "lucide-react";

interface ThresholdEditorProps {
  telemetry: TelemetrySnapshot | null;
}

type BandKey = "lolo" | "lo" | "hi" | "hihi";

export const ThresholdEditor: React.FC<ThresholdEditorProps> = ({ telemetry }) => {
  const [selectedSpindle, setSelectedSpindle] = useState<number>(0);
  const [config, setConfig] = useState<SpindleConfig | null>(null);
  const [expandedBands, setExpandedBands] = useState<Record<BandKey, boolean>>({
    lolo: false,
    lo: false,
    hi: false,
    hihi: false,
  });
  const [saving, setSaving] = useState(false);
  const [saveStatus, setSaveStatus] = useState<string | null>(null);

  useEffect(() => {
    let mounted = true;
    api.getSpindleConfig(selectedSpindle).then((cfg) => {
      if (mounted) setConfig(JSON.parse(JSON.stringify(cfg)));
    });
    return () => { mounted = false; };
  }, [selectedSpindle]);

  if (!config) {
    return <div style={{ textAlign: "center", padding: "40px", color: "var(--text-muted)" }}>Loading threshold configurations...</div>;
  }

  const liveCurrent = telemetry?.spindles[selectedSpindle]?.current_a || 0;

  // Validation: LoLo <= Lo <= Hi <= HiHi
  const b = config.bands;
  const orderingError =
    b.lolo.limit > b.lo.limit
      ? "LoLo limit must be <= Lo limit"
      : b.lo.limit > b.hi.limit
      ? "Lo limit must be <= Hi limit"
      : b.hi.limit > b.hihi.limit
      ? "Hi limit must be <= HiHi limit"
      : null;

  const toggleExpand = (band: BandKey) => {
    setExpandedBands((prev) => ({ ...prev, [band]: !prev[band] }));
  };

  const updateBand = (band: BandKey, field: keyof BandConfig, value: any) => {
    setConfig((prev) => {
      if (!prev) return prev;
      return {
        ...prev,
        bands: {
          ...prev.bands,
          [band]: {
            ...prev.bands[band],
            [field]: value,
          },
        },
      };
    });
  };

  const handleSave = async () => {
    if (orderingError) {
      alert("Please resolve the threshold ordering constraint first:\n" + orderingError);
      return;
    }
    setSaving(true);
    setSaveStatus(null);
    try {
      await api.saveThresholds(selectedSpindle, config);
      setSaveStatus("Saved and applied to monitor!");
      setTimeout(() => setSaveStatus(null), 4000);
    } catch (err: any) {
      alert("Failed to save: " + err.message);
    } finally {
      setSaving(false);
    }
  };

  const bandMeta: { key: BandKey; name: string; desc: string; type: "alarm" | "warn" }[] = [
    { key: "lolo", name: "LoLo (Broken Cutter / Severe Underload)", desc: "Immediate trip on broken cutter or sudden tool loss", type: "alarm" },
    { key: "lo", name: "Lo (Underload Warning)", desc: "Air-cutting warning or partial tool disengagement", type: "warn" },
    { key: "hi", name: "Hi (Overload / Tool Wear Warning)", desc: "Warning on flank wear or excessive stock allowance", type: "warn" },
    { key: "hihi", name: "HiHi (Crash / Jam Immediate Trip)", desc: "Instant trip on collision, spindle stall or catastrophic crash", type: "alarm" },
  ];

  // Visual Gauge calculations
  const maxScale = Math.max(25, config.bands.hihi.limit * 1.2);
  const livePct = Math.min(100, Math.max(0, (liveCurrent / maxScale) * 100));

  // Determine current active zone
  let activeZone = "NORMAL CUTTING ENVELOPE";
  let activeZoneColor = "var(--status-ok)";
  if (liveCurrent < config.bands.lolo.limit && config.bands.lolo.enabled) {
    activeZone = "LOLO SEVERE UNDERLOAD TRIP";
    activeZoneColor = "var(--status-alarm)";
  } else if (liveCurrent < config.bands.lo.limit && config.bands.lo.enabled) {
    activeZone = "LO AIR CUTTING WARNING";
    activeZoneColor = "var(--status-warn)";
  } else if (liveCurrent > config.bands.hihi.limit && config.bands.hihi.enabled) {
    activeZone = "HIHI COLLISION / JAM TRIP";
    activeZoneColor = "var(--status-alarm)";
  } else if (liveCurrent > config.bands.hi.limit && config.bands.hi.enabled) {
    activeZone = "HI TOOL WEAR WARNING";
    activeZoneColor = "var(--status-warn)";
  }

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* Top Selector Bar */}
      <div className="card" style={{ padding: "12px 20px" }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", flexWrap: "wrap", gap: 14 }}>
          <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
            <span style={{ fontSize: 13, fontWeight: 700, color: "var(--text-primary)" }}>Spindle:</span>
            <div style={{ display: "flex", gap: 6 }}>
              {[0, 1].map((idx) => (
                <button
                  key={idx}
                  className={`btn btn-sm ${selectedSpindle === idx ? "btn-primary" : "btn-secondary"}`}
                  onClick={() => setSelectedSpindle(idx)}
                >
                  Spindle {idx + 1} ({idx === 0 ? "Left" : "Right"})
                </button>
              ))}
            </div>
            <span style={{ fontSize: 13, color: "var(--text-muted)", marginLeft: 8 }}>
              Live Current: <strong className="num" style={{ color: "var(--blue-primary)", fontSize: 15 }}>{liveCurrent.toFixed(2)} A</strong>
            </span>
          </div>

          <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
            {saveStatus && (
              <span style={{ fontSize: 12, color: "var(--status-ok)", fontWeight: 600, display: "flex", alignItems: "center", gap: 4 }}>
                <Check size={14} /> {saveStatus}
              </span>
            )}
            <button className="btn btn-primary btn-sm" onClick={handleSave} disabled={saving}>
              <Save size={14} /> {saving ? "Saving..." : "Save Thresholds"}
            </button>
          </div>
        </div>
      </div>

      {/* Ordering Constraint Alert if any */}
      {orderingError && (
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
          gap: 10
        }}>
          <AlertCircle size={18} />
          <span>Ordering Rule Violated: {orderingError} (must satisfy LoLo &lt;= Lo &lt;= Hi &lt;= HiHi)</span>
        </div>
      )}

      {/* Main Side-by-Side High-Density Layout */}
      <div className="thresholds-layout">
        {/* Left Column: 4 Process Alarm Bands */}
        <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
          {bandMeta.map(({ key, name, desc, type }) => {
            const band = config.bands[key];
            const isExpanded = expandedBands[key];
            const pctOfLimit = band.limit > 0 ? (liveCurrent / band.limit) * 100 : 0;

            return (
              <div key={key} className="card" style={{
                padding: "14px 18px",
                borderLeft: `5px solid ${type === "alarm" ? "var(--status-alarm)" : "var(--status-warn)"}`
              }}>
                <div style={{
                  display: "flex",
                  justifyContent: "space-between",
                  alignItems: "center",
                  flexWrap: "wrap",
                  gap: 12
                }}>
                  <div style={{ flex: 1, minWidth: 200 }}>
                    <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
                      <h3 style={{ fontSize: 14, fontWeight: 700, margin: 0, color: "var(--text-primary)" }}>{name}</h3>
                      <label style={{ display: "flex", alignItems: "center", gap: 5, fontSize: 11, cursor: "pointer", fontWeight: 700 }}>
                        <input
                          type="checkbox"
                          checked={band.enabled}
                          onChange={(e) => updateBand(key, "enabled", e.target.checked)}
                        />
                        <span className={`badge ${band.enabled ? "badge-ok" : "badge-muted"}`} style={{ fontSize: 10, padding: "2px 7px" }}>
                          {band.enabled ? "Armed" : "Bypassed"}
                        </span>
                      </label>
                    </div>
                    <p style={{ fontSize: 11, color: "var(--text-muted)", marginTop: 2 }}>{desc}</p>
                  </div>

                  {/* Limit Input */}
                  <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
                    <div style={{ textAlign: "right" }}>
                      <div style={{ fontSize: 10, color: "var(--text-muted)", textTransform: "uppercase" }}>Load Ratio</div>
                      <div className="num" style={{ fontSize: 13, fontWeight: 700, color: "var(--text-primary)" }}>
                        {pctOfLimit.toFixed(0)}%
                      </div>
                    </div>

                    <div style={{ position: "relative", width: 120 }}>
                      <input
                        type="number"
                        step="0.1"
                        className="input-control num"
                        style={{ width: "100%", paddingRight: 28, fontSize: 15, fontWeight: 800, color: "var(--text-primary)", minHeight: 38 }}
                        value={band.limit}
                        onChange={(e) => updateBand(key, "limit", parseFloat(e.target.value) || 0)}
                      />
                      <span style={{
                        position: "absolute",
                        right: 10,
                        top: "50%",
                        transform: "translateY(-50%)",
                        color: "var(--text-muted)",
                        fontSize: 12,
                        fontWeight: 700,
                        pointerEvents: "none"
                      }}>
                        A
                      </span>
                    </div>

                    <button
                      className="btn btn-secondary btn-sm"
                      style={{ minHeight: 36, padding: "0 10px" }}
                      onClick={() => toggleExpand(key)}
                    >
                      {isExpanded ? <ChevronUp size={14} /> : <ChevronDown size={14} />}
                      <span style={{ fontSize: 11 }}>Timing</span>
                    </button>
                  </div>
                </div>

                {/* Collapsible Advanced Timing */}
                {isExpanded && (
                  <div style={{
                    marginTop: 12,
                    paddingTop: 12,
                    borderTop: "1px solid var(--border-subtle)",
                    display: "grid",
                    gridTemplateColumns: "repeat(4, 1fr)",
                    gap: 10
                  }}>
                    <div className="input-group" style={{ margin: 0 }}>
                      <label className="input-label" style={{ fontSize: 11 }}>Hysteresis (A)</label>
                      <input
                        type="number"
                        step="0.05"
                        className="input-control num"
                        style={{ minHeight: 34, fontSize: 12 }}
                        value={band.hysteresis}
                        onChange={(e) => updateBand(key, "hysteresis", parseFloat(e.target.value) || 0)}
                      />
                    </div>

                    <div className="input-group" style={{ margin: 0 }}>
                      <label className="input-label" style={{ fontSize: 11 }}>Trip Delay (ms)</label>
                      <input
                        type="number"
                        step="10"
                        className="input-control num"
                        style={{ minHeight: 34, fontSize: 12 }}
                        value={band.trip_delay_ms}
                        onChange={(e) => updateBand(key, "trip_delay_ms", parseInt(e.target.value) || 0)}
                      />
                    </div>

                    <div className="input-group" style={{ margin: 0 }}>
                      <label className="input-label" style={{ fontSize: 11 }}>Clear Delay (ms)</label>
                      <input
                        type="number"
                        step="10"
                        className="input-control num"
                        style={{ minHeight: 34, fontSize: 12 }}
                        value={band.clear_delay_ms}
                        onChange={(e) => updateBand(key, "clear_delay_ms", parseInt(e.target.value) || 0)}
                      />
                    </div>

                    <div className="input-group" style={{ margin: 0, justifyContent: "center" }}>
                      <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 12, cursor: "pointer", marginTop: 12, fontWeight: 600 }}>
                        <input
                          type="checkbox"
                          checked={band.latched}
                          onChange={(e) => updateBand(key, "latched", e.target.checked)}
                        />
                        <span>Latch Trip</span>
                      </label>
                    </div>
                  </div>
                )}
              </div>
            );
          })}
        </div>

        {/* Right Column: Visual Process Trip Envelope + Transient Detectors */}
        <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          {/* Visual Trip Zone Envelope Gauge */}
          <div className="card">
            <div className="card-title">
              <Activity size={16} color="var(--blue-primary)" />
              <span>Real-Time Process Envelope Map</span>
            </div>

            {/* Active Zone Status Pill */}
            <div style={{
              background: "var(--bg-card-subtle)",
              borderRadius: "var(--radius-sm)",
              padding: "10px 14px",
              display: "flex",
              justifyContent: "space-between",
              alignItems: "center",
              marginBottom: 16
            }}>
              <span style={{ fontSize: 12, color: "var(--text-muted)", fontWeight: 600 }}>Active Status</span>
              <span style={{ fontSize: 12, fontWeight: 800, color: activeZoneColor }}>
                {activeZone}
              </span>
            </div>

            {/* Graphical Spectrum Bar */}
            <div style={{ position: "relative", marginBottom: 28, marginTop: 10 }}>
              <div style={{
                width: "100%",
                height: 18,
                borderRadius: 9,
                background: "linear-gradient(to right, #fee2e2 0%, #fee2e2 12%, #fef3c7 12%, #fef3c7 25%, #eff6ff 25%, #eff6ff 70%, #fef3c7 70%, #fef3c7 85%, #fee2e2 85%, #fee2e2 100%)",
                border: "1px solid var(--border-subtle)",
                position: "relative"
              }}>
                {/* Needle Indicator for Live Current */}
                <div style={{
                  position: "absolute",
                  left: `${livePct}%`,
                  top: -6,
                  bottom: -6,
                  width: 4,
                  background: "var(--blue-primary)",
                  borderRadius: 2,
                  boxShadow: "0 0 6px rgba(37, 99, 235, 0.8)",
                  transition: "left 0.2s ease"
                }}>
                  <div style={{
                    position: "absolute",
                    top: -18,
                    left: "50%",
                    transform: "translateX(-50%)",
                    fontSize: 10,
                    fontWeight: 800,
                    color: "var(--blue-primary)",
                    whiteSpace: "nowrap"
                  }}>
                    {liveCurrent.toFixed(1)}A
                  </div>
                </div>
              </div>

              {/* Threshold Limit Markers below bar */}
              <div style={{ display: "flex", justifyContent: "space-between", marginTop: 8, fontSize: 10, color: "var(--text-muted)" }}>
                <span>0A</span>
                <span style={{ color: "var(--status-alarm)" }}>LoLo ({b.lolo.limit}A)</span>
                <span style={{ color: "var(--status-warn)" }}>Lo ({b.lo.limit}A)</span>
                <span style={{ color: "var(--blue-primary)", fontWeight: 700 }}>Nominal</span>
                <span style={{ color: "var(--status-warn)" }}>Hi ({b.hi.limit}A)</span>
                <span style={{ color: "var(--status-alarm)" }}>HiHi ({b.hihi.limit}A)</span>
              </div>
            </div>

            {/* Invariant Rule Status */}
            <div style={{
              display: "flex",
              alignItems: "center",
              gap: 8,
              fontSize: 12,
              color: orderingError ? "var(--status-alarm)" : "var(--status-ok)",
              background: orderingError ? "var(--status-alarm-bg)" : "var(--status-ok-bg)",
              padding: "8px 12px",
              borderRadius: "var(--radius-sm)",
              fontWeight: 600
            }}>
              <CheckCircle2 size={16} />
              <span>Ordering Rule: LoLo ({b.lolo.limit}A) &le; Lo ({b.lo.limit}A) &le; Hi ({b.hi.limit}A) &le; HiHi ({b.hihi.limit}A)</span>
            </div>
          </div>

          {/* Transient & Cut Detection Card */}
          <div className="card">
            <div className="card-title">
              <ShieldAlert size={16} color="var(--blue-primary)" />
              <span>Transient Detectors & Cut Trigger</span>
            </div>

            <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label" style={{ fontSize: 11 }}>Breakage Drop Threshold (%)</label>
                <input
                  type="number"
                  className="input-control num"
                  style={{ minHeight: 38 }}
                  value={config.breakage_drop_pct}
                  onChange={(e) => setConfig({ ...config, breakage_drop_pct: parseInt(e.target.value) || 0 })}
                />
                <span style={{ fontSize: 11, color: "var(--text-muted)" }}>Triggers trip when motor load suddenly drops during cut</span>
              </div>

              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label" style={{ fontSize: 11 }}>Spindle Crash Rise Threshold (%)</label>
                <input
                  type="number"
                  className="input-control num"
                  style={{ minHeight: 38 }}
                  value={config.crash_rise_pct}
                  onChange={(e) => setConfig({ ...config, crash_rise_pct: parseInt(e.target.value) || 0 })}
                />
                <span style={{ fontSize: 11, color: "var(--text-muted)" }}>Triggers instant hardware trip on steep current spike</span>
              </div>

              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label" style={{ fontSize: 11 }}>Active Cut Engagement Level (A)</label>
                <input
                  type="number"
                  step="0.1"
                  className="input-control num"
                  style={{ minHeight: 38 }}
                  value={config.cut_detect.active_threshold_a}
                  onChange={(e) => setConfig({
                    ...config,
                    cut_detect: { ...config.cut_detect, active_threshold_a: parseFloat(e.target.value) || 0 }
                  })}
                />
                <span style={{ fontSize: 11, color: "var(--text-muted)" }}>Current threshold where monitoring state enters CUTTING</span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};
