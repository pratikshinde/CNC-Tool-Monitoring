import React, { useState, useEffect } from "react";
import { api } from "../../services/api";
import { JobTemplate } from "../../types";
import { Plus, ArrowRight, Trash2, CheckCircle2, ShieldCheck, Database, Layers } from "lucide-react";

export const JobManager: React.FC = () => {
  const [templates, setTemplates] = useState<JobTemplate[]>([]);
  const [newTemplateName, setNewTemplateName] = useState("");
  const [newTemplateDesc, setNewTemplateDesc] = useState("");
  const [sourceSpindle, setSourceSpindle] = useState(0);
  const [statusMsg, setStatusMsg] = useState<string | null>(null);

  useEffect(() => {
    loadTemplates();
  }, []);

  const loadTemplates = async () => {
    try {
      const data = await api.getJobTemplates();
      setTemplates(data);
    } catch (err) {
      console.error("Failed to load templates", err);
    }
  };

  const handleCreate = async () => {
    if (!newTemplateName.trim()) {
      alert("Please specify a template name");
      return;
    }
    try {
      const cfg = await api.getSpindleConfig(sourceSpindle);
      const newTemplate: JobTemplate = {
        name: newTemplateName.trim(),
        description: newTemplateDesc.trim() || `Saved from Spindle ${sourceSpindle + 1}`,
        bands: cfg.bands,
        breakage_drop_pct: cfg.breakage_drop_pct,
        crash_rise_pct: cfg.crash_rise_pct,
        cut_detect: cfg.cut_detect,
        wear: cfg.wear,
        created_at: new Date().toISOString().substring(0, 16).replace("T", " "),
      };

      await api.saveJobTemplate(newTemplate);
      setNewTemplateName("");
      setNewTemplateDesc("");
      setStatusMsg(`Job recipe "${newTemplate.name}" stored in LittleFS.`);
      setTimeout(() => setStatusMsg(null), 4000);
      loadTemplates();
    } catch (err: any) {
      alert("Error saving template: " + err.message);
    }
  };

  const handleApply = async (name: string, targetSpindle: number) => {
    if (!confirm(`Apply recipe "${name}" to Spindle ${targetSpindle + 1}?\n\nSafety Guarantee: Sensor calibration factors and hardware zero-offsets remain strictly untouched.`)) {
      return;
    }
    try {
      await api.applyJobTemplate(name, targetSpindle);
      setStatusMsg(`Recipe "${name}" applied to Spindle ${targetSpindle + 1}!`);
      setTimeout(() => setStatusMsg(null), 4000);
    } catch (err: any) {
      alert("Apply failed: " + err.message);
    }
  };

  const handleDelete = async (name: string) => {
    if (!confirm(`Delete template "${name}" from flash?`)) return;
    try {
      await api.deleteJobTemplate(name);
      loadTemplates();
    } catch (err: any) {
      alert("Failed to delete: " + err.message);
    }
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* Top Notification if any */}
      {statusMsg && (
        <div className="badge badge-ok" style={{ padding: "10px 16px", fontSize: 13, display: "flex", width: "fit-content" }}>
          <CheckCircle2 size={16} /> {statusMsg}
        </div>
      )}

      {/* High-Density Side-by-Side Layout */}
      <div className="jobs-layout">
        {/* Left Column: Form & Safety Specifications */}
        <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
          {/* Create Form Card */}
          <div className="card">
            <div className="card-title">
              <Plus size={16} color="var(--blue-primary)" />
              <span>Capture Job Recipe</span>
            </div>

            <div style={{ display: "flex", flexDirection: "column", gap: 14 }}>
              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label">Part / Recipe Name</label>
                <input
                  type="text"
                  placeholder="e.g. Inconel-Finish-M8"
                  className="input-control"
                  value={newTemplateName}
                  onChange={(e) => setNewTemplateName(e.target.value)}
                />
              </div>

              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label">Tooling Notes / Spec</label>
                <input
                  type="text"
                  placeholder="e.g. 8mm 4-flute @ 8500 RPM"
                  className="input-control"
                  value={newTemplateDesc}
                  onChange={(e) => setNewTemplateDesc(e.target.value)}
                />
              </div>

              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label">Source Spindle Profile</label>
                <select
                  className="input-control"
                  value={sourceSpindle}
                  onChange={(e) => setSourceSpindle(Number(e.target.value))}
                >
                  <option value={0}>Spindle 1 (Left Channel)</option>
                  <option value={1}>Spindle 2 (Right Channel)</option>
                </select>
              </div>

              <button className="btn btn-primary" style={{ width: "100%", marginTop: 6 }} onClick={handleCreate}>
                <Plus size={16} /> Save Recipe to Library
              </button>
            </div>
          </div>

          {/* Safety & LittleFS Info Card */}
          <div className="card">
            <div className="card-title">
              <ShieldCheck size={16} color="var(--blue-primary)" />
              <span>Safety Recipe Isolation</span>
            </div>

            <p style={{ fontSize: 12, color: "var(--text-secondary)", lineHeight: 1.6, marginBottom: 14 }}>
              Job templates store process thresholds (LoLo, Lo, Hi, HiHi) and cut triggers without carrying sensor calibration. Loading a recipe can <strong>never</strong> overwrite CT zero-tare or pressure calibration.
            </p>

            <div style={{
              background: "var(--bg-card-subtle)",
              borderRadius: "var(--radius-sm)",
              padding: "12px 14px",
              display: "flex",
              justifyContent: "space-between",
              alignItems: "center"
            }}>
              <span style={{ fontSize: 12, color: "var(--text-muted)", fontWeight: 600 }}>Flash Storage Slots</span>
              <span style={{ fontSize: 13, fontWeight: 700, color: "var(--text-primary)" }}>
                {templates.length} / 16 In Use
              </span>
            </div>
          </div>
        </div>

        {/* Right Column: Stored Templates Library */}
        <div className="card" style={{ display: "flex", flexDirection: "column", minHeight: 460 }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 16 }}>
            <div className="card-title" style={{ margin: 0 }}>
              <Layers size={17} color="var(--blue-primary)" />
              <span>Stored Job Template Library</span>
            </div>
            <span className="badge badge-ok" style={{ fontSize: 12 }}>{templates.length} Active Recipes</span>
          </div>

          {templates.length === 0 ? (
            <div style={{ padding: "50px 20px", textAlign: "center", color: "var(--text-muted)" }}>
              No job recipes stored yet. Use the form on the left to capture the active spindle parameters.
            </div>
          ) : (
            <div style={{ display: "flex", flexDirection: "column", gap: 14 }}>
              {templates.map((tmpl) => (
                <div key={tmpl.name} style={{
                  background: "var(--bg-card-subtle)",
                  border: "1px solid var(--border-subtle)",
                  borderRadius: "var(--radius-md)",
                  padding: 16,
                  transition: "all 0.2s ease"
                }}>
                  <div style={{ display: "flex", justifyContent: "space-between", alignItems: "flex-start", flexWrap: "wrap", gap: 12 }}>
                    <div>
                      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
                        <h4 style={{ fontSize: 15, fontWeight: 700, margin: 0, color: "var(--text-primary)" }}>
                          {tmpl.name}
                        </h4>
                        <span style={{ fontSize: 11, color: "var(--text-muted)" }}>{tmpl.created_at}</span>
                      </div>
                      <p style={{ fontSize: 12, color: "var(--text-secondary)", marginTop: 4 }}>
                        {tmpl.description}
                      </p>
                    </div>

                    <div style={{ display: "flex", gap: 8 }}>
                      <button
                        className="btn btn-secondary btn-sm"
                        onClick={() => handleApply(tmpl.name, 0)}
                      >
                        Apply to Spindle 1
                      </button>
                      <button
                        className="btn btn-secondary btn-sm"
                        onClick={() => handleApply(tmpl.name, 1)}
                      >
                        Apply to Spindle 2
                      </button>
                      <button
                        className="btn btn-danger btn-sm"
                        style={{ padding: "0 10px" }}
                        onClick={() => handleDelete(tmpl.name)}
                        title="Delete Template"
                      >
                        <Trash2 size={14} />
                      </button>
                    </div>
                  </div>

                  {/* Threshold Envelope Badges */}
                  <div style={{
                    display: "flex",
                    flexWrap: "wrap",
                    gap: 8,
                    marginTop: 14,
                    paddingTop: 12,
                    borderTop: "1px solid var(--border-subtle)"
                  }}>
                    <span className="badge badge-alarm num" style={{ fontSize: 11 }}>
                      LoLo: {tmpl.bands?.lolo?.limit ?? "-"}A
                    </span>
                    <span className="badge badge-warn num" style={{ fontSize: 11 }}>
                      Lo: {tmpl.bands?.lo?.limit ?? "-"}A
                    </span>
                    <span className="badge badge-warn num" style={{ fontSize: 11 }}>
                      Hi: {tmpl.bands?.hi?.limit ?? "-"}A
                    </span>
                    <span className="badge badge-alarm num" style={{ fontSize: 11 }}>
                      HiHi: {tmpl.bands?.hihi?.limit ?? "-"}A
                    </span>
                    <span className="badge badge-muted num" style={{ fontSize: 11 }}>
                      Cut: {tmpl.cut_detect?.active_threshold_a ?? "-"}A
                    </span>
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>
      </div>
    </div>
  );
};
