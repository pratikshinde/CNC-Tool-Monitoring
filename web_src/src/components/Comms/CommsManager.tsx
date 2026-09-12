import React, { useState, useEffect } from "react";
import { api } from "../../services/api";
import { CommsConfig } from "../../types";
import { Wifi, Save, CheckCircle2, Network, Radio, Table } from "lucide-react";

export const CommsManager: React.FC = () => {
  const [comms, setComms] = useState<CommsConfig | null>(null);
  const [saving, setSaving] = useState(false);
  const [statusMsg, setStatusMsg] = useState<string | null>(null);

  useEffect(() => {
    let mounted = true;
    api.getCommsConfig().then((cfg) => {
      if (mounted) setComms(cfg);
    });
    return () => { mounted = false; };
  }, []);

  if (!comms) {
    return <div style={{ padding: "40px", textAlign: "center", color: "var(--text-muted)" }}>Loading communications configuration...</div>;
  }

  const handleSave = async () => {
    setSaving(true);
    try {
      await api.saveCommsConfig(comms);
      setStatusMsg("Communications configuration updated.");
      setTimeout(() => setStatusMsg(null), 4000);
    } catch (err: any) {
      alert("Failed to save: " + err.message);
    } finally {
      setSaving(false);
    }
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
      {/* Status Alert if any */}
      {statusMsg && (
        <div className="badge badge-ok" style={{ padding: "10px 16px", fontSize: 13, width: "fit-content" }}>
          <CheckCircle2 size={16} /> {statusMsg}
        </div>
      )}

      {/* Side-by-Side Dual Networking & Industrial Modbus Cards */}
      <div className="side-by-side">
        {/* Left Card: Wi-Fi Networking */}
        <div className="card" style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          <div className="card-title" style={{ margin: 0 }}>
            <Wifi size={16} color="var(--blue-primary)" />
            <span>Wi-Fi Network Configuration</span>
          </div>

          {/* Station Mode (Factory LAN) */}
          <div style={{
            background: "var(--bg-card-subtle)",
            border: "1px solid var(--border-subtle)",
            borderRadius: "var(--radius-sm)",
            padding: 16
          }}>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 12 }}>
              <h4 style={{ fontSize: 13, fontWeight: 700, margin: 0, color: "var(--text-primary)" }}>
                Station Mode (Factory LAN)
              </h4>
              <span className={`badge ${comms.wifi.sta_connected ? "badge-ok" : "badge-warn"}`} style={{ fontSize: 10 }}>
                {comms.wifi.sta_connected ? "Connected" : "Disconnected"}
              </span>
            </div>

            <div className="input-group">
              <label className="input-label" style={{ fontSize: 11 }}>Network SSID</label>
              <input
                type="text"
                className="input-control"
                style={{ minHeight: 38 }}
                value={comms.wifi.sta_ssid}
                onChange={(e) => setComms({
                  ...comms,
                  wifi: { ...comms.wifi, sta_ssid: e.target.value }
                })}
              />
            </div>

            <div className="input-group">
              <label className="input-label" style={{ fontSize: 11 }}>WPA2 / WPA3 Password</label>
              <input
                type="password"
                placeholder="••••••••••••"
                className="input-control"
                style={{ minHeight: 38 }}
                value={comms.wifi.sta_pass || ""}
                onChange={(e) => setComms({
                  ...comms,
                  wifi: { ...comms.wifi, sta_pass: e.target.value }
                })}
              />
            </div>

            <div style={{ fontSize: 12, color: "var(--text-muted)", marginTop: 8 }}>
              Assigned IP: <strong className="num" style={{ color: "var(--text-primary)" }}>{comms.wifi.sta_ip || "192.168.1.145 (DHCP)"}</strong>
            </div>
          </div>

          {/* Fallback SoftAP Mode */}
          <div style={{
            background: "var(--bg-card-subtle)",
            border: "1px solid var(--border-subtle)",
            borderRadius: "var(--radius-sm)",
            padding: 16
          }}>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 12 }}>
              <h4 style={{ fontSize: 13, fontWeight: 700, margin: 0, color: "var(--text-primary)" }}>
                Fallback SoftAP (Direct Connect)
              </h4>
              <span className="badge badge-ok" style={{ fontSize: 10 }}>Always Active</span>
            </div>

            <div className="input-group">
              <label className="input-label" style={{ fontSize: 11 }}>AP SSID</label>
              <input
                type="text"
                className="input-control"
                style={{ minHeight: 38 }}
                value={comms.wifi.ap_ssid}
                onChange={(e) => setComms({
                  ...comms,
                  wifi: { ...comms.wifi, ap_ssid: e.target.value }
                })}
              />
            </div>

            <div className="input-group">
              <label className="input-label" style={{ fontSize: 11 }}>AP Password</label>
              <input
                type="password"
                placeholder="Leave blank for open network"
                className="input-control"
                style={{ minHeight: 38 }}
                value={comms.wifi.ap_pass || ""}
                onChange={(e) => setComms({
                  ...comms,
                  wifi: { ...comms.wifi, ap_pass: e.target.value }
                })}
              />
            </div>

            <div style={{ fontSize: 12, color: "var(--text-muted)", marginTop: 8 }}>
              AP Gateway IP: <strong className="num" style={{ color: "var(--blue-primary)" }}>{comms.wifi.ap_ip || "192.168.4.1"}</strong>
            </div>
          </div>
        </div>

        {/* Right Card: Industrial Modbus PLC / SCADA Telemetry */}
        <div className="card" style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          <div className="card-title" style={{ margin: 0 }}>
            <Network size={16} color="var(--blue-primary)" />
            <span>Industrial Modbus PLC / SCADA</span>
          </div>

          <div style={{
            background: "var(--bg-card-subtle)",
            border: "1px solid var(--border-subtle)",
            borderRadius: "var(--radius-sm)",
            padding: 16
          }}>
            <h4 style={{ fontSize: 13, fontWeight: 700, margin: "0 0 12px 0", color: "var(--text-primary)" }}>
              Modbus RTU (RS-485 Isolated)
            </h4>

            <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 12 }}>
              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label" style={{ fontSize: 11 }}>Slave Node Address</label>
                <input
                  type="number"
                  min="1"
                  max="247"
                  className="input-control num"
                  style={{ minHeight: 38 }}
                  value={comms.modbus.rtu_addr}
                  onChange={(e) => setComms({
                    ...comms,
                    modbus: { ...comms.modbus, rtu_addr: parseInt(e.target.value) || 1 }
                  })}
                />
              </div>

              <div className="input-group" style={{ margin: 0 }}>
                <label className="input-label" style={{ fontSize: 11 }}>Baud Rate</label>
                <select
                  className="input-control num"
                  style={{ minHeight: 38 }}
                  value={comms.modbus.rtu_baud}
                  onChange={(e) => setComms({
                    ...comms,
                    modbus: { ...comms.modbus, rtu_baud: parseInt(e.target.value) || 115200 }
                  })}
                >
                  <option value={9600}>9600 bps</option>
                  <option value={19200}>19200 bps</option>
                  <option value={38400}>38400 bps</option>
                  <option value={57600}>57600 bps</option>
                  <option value={115200}>115200 bps</option>
                </select>
              </div>
            </div>
          </div>

          <div style={{
            background: "var(--bg-card-subtle)",
            border: "1px solid var(--border-subtle)",
            borderRadius: "var(--radius-sm)",
            padding: 16
          }}>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 12 }}>
              <h4 style={{ fontSize: 13, fontWeight: 700, margin: 0, color: "var(--text-primary)" }}>
                Modbus TCP (Ethernet / Wi-Fi)
              </h4>
              <span className="badge badge-ok" style={{ fontSize: 10 }}>{comms.modbus.tcp_clients} Active Clients</span>
            </div>

            <div className="input-group" style={{ margin: 0 }}>
              <label className="input-label" style={{ fontSize: 11 }}>TCP Port</label>
              <input
                type="number"
                className="input-control num"
                style={{ minHeight: 38 }}
                value={comms.modbus.tcp_port}
                onChange={(e) => setComms({
                  ...comms,
                  modbus: { ...comms.modbus, tcp_port: parseInt(e.target.value) || 502 }
                })}
              />
            </div>
          </div>

          {/* Quick Register Reference Table */}
          <div style={{
            background: "var(--bg-card-subtle)",
            border: "1px solid var(--border-subtle)",
            borderRadius: "var(--radius-sm)",
            padding: 14
          }}>
            <div style={{ fontSize: 12, fontWeight: 700, color: "var(--text-primary)", marginBottom: 8, display: "flex", alignItems: "center", gap: 6 }}>
              <Table size={14} color="var(--blue-primary)" /> Holding Register Map
            </div>
            <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8, fontSize: 11, color: "var(--text-secondary)" }}>
              <div><code>40001</code> : Spindle 1 Amps (x100)</div>
              <div><code>40002</code> : Spindle 1 RPM</div>
              <div><code>40003</code> : Spindle 1 Pressure (x10)</div>
              <div><code>40004</code> : Alarm Trip Bitmask</div>
            </div>
          </div>
        </div>
      </div>

      <div style={{ display: "flex", justifyContent: "flex-end" }}>
        <button className="btn btn-primary" onClick={handleSave} disabled={saving}>
          <Save size={16} /> {saving ? "Applying Network Changes..." : "Save & Apply Network Configuration"}
        </button>
      </div>
    </div>
  );
};
