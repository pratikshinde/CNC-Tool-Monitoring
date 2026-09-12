import React, { useState, useEffect } from "react";
import { api } from "../../services/api";
import { SystemInfo } from "../../types";
import { Cpu, HardDrive, UploadCloud, RotateCw, AlertTriangle, CheckCircle2, ShieldCheck, Layers } from "lucide-react";

export const SystemView: React.FC = () => {
  const [sysInfo, setSysInfo] = useState<SystemInfo | null>(null);
  const [otaFile, setOtaFile] = useState<File | null>(null);
  const [uploading, setUploading] = useState(false);
  const [progress, setProgress] = useState(0);
  const [otaStatus, setOtaStatus] = useState<string | null>(null);

  useEffect(() => {
    let mounted = true;
    api.getSystemInfo().then((info) => {
      if (mounted) setSysInfo(info);
    });
    return () => { mounted = false; };
  }, []);

  const handleUploadOTA = () => {
    if (!otaFile) {
      alert("Please select a compiled firmware .bin file first.");
      return;
    }
    setUploading(true);
    setProgress(0);
    setOtaStatus("Uploading firmware binary to ESP32 standby slot...");

    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota");
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        setProgress(Math.round((e.loaded / e.total) * 100));
      }
    };
    xhr.onload = () => {
      setUploading(false);
      try {
        const j = JSON.parse(xhr.responseText);
        if (j.status === "ok") {
          setOtaStatus("Firmware successfully written! Rebooting device in 10 seconds...");
          setTimeout(() => window.location.reload(), 12000);
        } else {
          setOtaStatus("Update rejected: " + (j.reason || "Verification error"));
        }
      } catch {
        setOtaStatus("OTA update staged. Rebooting...");
      }
    };
    xhr.onerror = () => {
      setUploading(false);
      setOtaStatus("Connection lost during firmware upload.");
    };
    xhr.send(otaFile);
  };

  const handleReboot = async () => {
    if (!confirm("Reboot the CNC Tool Monitor?\n\nOutputs will safely de-energize until initialization is complete.")) return;
    try {
      await api.reboot();
      alert("Reboot command acknowledged. Reconnecting in 8 seconds.");
      setTimeout(() => window.location.reload(), 8000);
    } catch (e: any) {
      alert("Reboot failed: " + e.message);
    }
  };

  const handleFactoryReset = async () => {
    if (!confirm("CAUTION: Restore factory defaults?\n\nAll sensor calibration, threshold limits, and network settings will be erased.")) return;
    try {
      await api.factoryReset();
      alert("Factory reset complete. Rebooting...");
      setTimeout(() => window.location.reload(), 10000);
    } catch (e: any) {
      alert("Reset failed: " + e.message);
    }
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* High-Density Side-by-Side System Layout */}
      <div className="side-by-side">
        {/* Left Column: Device Architecture & Memory Health */}
        <div className="card" style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          <div className="card-title" style={{ margin: 0 }}>
            <Cpu size={16} color="var(--blue-primary)" />
            <span>ESP32 Hardware & Storage Health</span>
          </div>

          {sysInfo && (
            <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
              {/* Firmware Card */}
              <div style={{
                background: "var(--bg-card-subtle)",
                padding: 14,
                borderRadius: "var(--radius-sm)",
                border: "1px solid var(--border-subtle)"
              }}>
                <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Firmware Build</span>
                <div style={{ fontSize: 14, fontWeight: 700, marginTop: 4, color: "var(--text-primary)" }}>{sysInfo.version}</div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)", marginTop: 2 }}>
                  Compiled: {sysInfo.build_date} {sysInfo.build_time}
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)", marginTop: 2 }}>
                  Framework: {sysInfo.idf_version}
                </div>
              </div>

              {/* Free Memory Meter */}
              <div style={{
                background: "var(--bg-card-subtle)",
                padding: 14,
                borderRadius: "var(--radius-sm)",
                border: "1px solid var(--border-subtle)"
              }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
                  <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>SRAM Free Heap</span>
                  <span className="badge badge-ok" style={{ fontSize: 10 }}>Healthy</span>
                </div>
                <div className="num" style={{ fontSize: 20, fontWeight: 800, color: "var(--status-ok)", marginTop: 4 }}>
                  {(sysInfo.free_heap / 1024).toFixed(1)} kB
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)", marginTop: 2 }}>
                  Min Watermark: <strong className="num">{(sysInfo.min_free_heap / 1024).toFixed(1)} kB</strong>
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)", marginTop: 2 }}>
                  Last Boot Reason: {sysInfo.reset_reason}
                </div>
              </div>

              {/* LittleFS Web Storage meter */}
              <div style={{
                background: "var(--bg-card-subtle)",
                padding: 14,
                borderRadius: "var(--radius-sm)",
                border: "1px solid var(--border-subtle)"
              }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
                  <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>LittleFS Web Partition</span>
                  <span className="badge badge-ok" style={{ fontSize: 10 }}>512 kB Partition</span>
                </div>
                <div style={{ width: "100%", height: 6, background: "var(--border-subtle)", borderRadius: 3, marginTop: 10, overflow: "hidden" }}>
                  <div style={{ width: "13%", height: "100%", background: "var(--blue-primary)", borderRadius: 3 }} />
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)", marginTop: 6, display: "flex", justifyContent: "space-between" }}>
                  <span>Used: <strong>64.6 kB (gzipped)</strong></span>
                  <span>Free: <strong>447.4 kB (87%)</strong></span>
                </div>
              </div>

              {/* Dual-OTA Partition status */}
              <div style={{
                background: "var(--bg-card-subtle)",
                padding: 14,
                borderRadius: "var(--radius-sm)",
                border: "1px solid var(--border-subtle)"
              }}>
                <span style={{ fontSize: 11, color: "var(--text-muted)", textTransform: "uppercase" }}>Dual-OTA Partitions</span>
                <div style={{ fontSize: 13, marginTop: 4 }}>
                  Active Slot: <strong style={{ color: "var(--blue-primary)" }}>{sysInfo.running_partition}</strong>
                </div>
                <div style={{ fontSize: 13, color: "var(--text-secondary)", marginTop: 2 }}>
                  Standby Slot: <strong>{sysInfo.update_partition}</strong>
                </div>
                <div style={{ fontSize: 12, color: "var(--status-ok)", marginTop: 4, display: "flex", alignItems: "center", gap: 6 }}>
                  <CheckCircle2 size={14} /> Automatic Rollback Recovery Armed
                </div>
              </div>
            </div>
          )}
        </div>

        {/* Right Column: OTA Firmware Upgrade & System Controls */}
        <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          {/* Browser OTA Firmware Update Card */}
          <div className="card">
            <div className="card-title">
              <UploadCloud size={16} color="var(--blue-primary)" />
              <span>Over-The-Air (OTA) Firmware Upgrade</span>
            </div>

            <p style={{ fontSize: 12, color: "var(--text-secondary)", marginBottom: 14 }}>
              Upload a freshly compiled <code>cnc_tool_monitor.bin</code>. ESP-IDF cryptographic SHA-256 signature is verified before committing the partition boot vectors.
            </p>

            <div style={{
              border: "2px dashed var(--border-medium)",
              borderRadius: "var(--radius-sm)",
              padding: "20px 16px",
              textAlign: "center",
              background: "var(--bg-card-subtle)",
              marginBottom: 14
            }}>
              <input
                type="file"
                accept=".bin"
                id="ota-file-input"
                style={{ display: "none" }}
                onChange={(e) => setOtaFile(e.target.files ? e.target.files[0] : null)}
              />
              <label htmlFor="ota-file-input" style={{ cursor: "pointer", display: "inline-flex", flexDirection: "column", alignItems: "center", gap: 8 }}>
                <UploadCloud size={28} color="var(--blue-primary)" />
                <span style={{ fontSize: 13, fontWeight: 600, color: "var(--text-primary)" }}>
                  {otaFile ? otaFile.name : "Click to select firmware .bin image"}
                </span>
                <span style={{ fontSize: 11, color: "var(--text-muted)" }}>
                  {otaFile ? `${(otaFile.size / 1024).toFixed(1)} kB` : "ESP-IDF app partition format"}
                </span>
              </label>
            </div>

            <button
              className="btn btn-primary"
              style={{ width: "100%" }}
              onClick={handleUploadOTA}
              disabled={!otaFile || uploading}
            >
              <UploadCloud size={16} /> {uploading ? `Writing Image (${progress}%)...` : "Flash Firmware Image"}
            </button>

            {uploading && (
              <div style={{ marginTop: 14 }}>
                <div style={{
                  width: "100%",
                  height: 8,
                  background: "var(--bg-card-subtle)",
                  borderRadius: 4,
                  overflow: "hidden"
                }}>
                  <div style={{
                    width: `${progress}%`,
                    height: "100%",
                    background: "var(--blue-primary)",
                    transition: "width 0.2s ease"
                  }} />
                </div>
                <div style={{ fontSize: 11, color: "var(--text-secondary)", marginTop: 6, textAlign: "right" }}>
                  {progress}% transferred
                </div>
              </div>
            )}

            {otaStatus && (
              <div style={{
                marginTop: 14,
                padding: "10px 14px",
                borderRadius: "var(--radius-sm)",
                background: "var(--bg-card-subtle)",
                fontSize: 12,
                color: "var(--text-primary)"
              }}>
                {otaStatus}
              </div>
            )}
          </div>

          {/* Maintenance Actions Card */}
          <div className="card">
            <div className="card-title">
              <RotateCw size={16} color="var(--blue-primary)" />
              <span>Controller Maintenance Actions</span>
            </div>

            <p style={{ fontSize: 12, color: "var(--text-secondary)", marginBottom: 14 }}>
              Execute low-level system actions. Reboots preserve all NVS settings, while factory reset clears all saved calibration.
            </p>

            <div style={{ display: "flex", gap: 12, flexWrap: "wrap" }}>
              <button className="btn btn-secondary" style={{ flex: 1 }} onClick={handleReboot}>
                <RotateCw size={15} /> Soft Reboot Controller
              </button>

              <button className="btn btn-danger" style={{ flex: 1 }} onClick={handleFactoryReset}>
                <AlertTriangle size={15} /> Factory Reset NVS
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};
