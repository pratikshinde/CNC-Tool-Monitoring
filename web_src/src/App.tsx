import React, { useState, useEffect } from "react";
import { Header } from "./components/Header";
import { Navigation, TabType } from "./components/Navigation";
import { DashboardView } from "./components/Dashboard/DashboardView";
import { TrendView } from "./components/Trends/TrendView";
import { ThresholdEditor } from "./components/Thresholds/ThresholdEditor";
import { JobManager } from "./components/Jobs/JobManager";
import { CalibrationWizard } from "./components/Calibration/CalibrationWizard";
import { CommsManager } from "./components/Comms/CommsManager";
import { SystemView } from "./components/System/SystemView";
import { api } from "./services/api";
import { TelemetrySnapshot } from "./types";

export const App: React.FC = () => {
  const [activeTab, setActiveTab] = useState<TabType>("dashboard");
  const [telemetry, setTelemetry] = useState<TelemetrySnapshot | null>(null);
  const [isAcknowledging, setIsAcknowledging] = useState(false);

  // Poll 1 Hz telemetry
  useEffect(() => {
    let mounted = true;
    const fetchTelemetry = async () => {
      try {
        const snap = await api.getTelemetry();
        if (mounted) setTelemetry(snap);
      } catch (err) {
        console.error("Telemetry error", err);
      }
    };

    fetchTelemetry();
    const interval = setInterval(fetchTelemetry, 1000);
    return () => {
      mounted = false;
      clearInterval(interval);
    };
  }, []);

  const handleAcknowledge = async () => {
    setIsAcknowledging(true);
    try {
      await api.acknowledgeLatches();
      const updated = await api.getTelemetry();
      setTelemetry(updated);
    } catch (err) {
      console.error("Ack failed", err);
    } finally {
      setIsAcknowledging(false);
    }
  };

  return (
    <div style={{ minHeight: "100vh", display: "flex", flexDirection: "column" }}>
      <Header
        telemetry={telemetry}
        onAcknowledge={handleAcknowledge}
        isAcknowledging={isAcknowledging}
      />
      <Navigation activeTab={activeTab} onTabChange={setActiveTab} />

      <main className="layout-container view-enter" key={activeTab} style={{ flex: 1 }}>
        {activeTab === "dashboard" && <DashboardView telemetry={telemetry} />}
        {activeTab === "trends" && <TrendView telemetry={telemetry} />}
        {activeTab === "thresholds" && <ThresholdEditor telemetry={telemetry} />}
        {activeTab === "jobs" && <JobManager />}
        {activeTab === "calibration" && <CalibrationWizard />}
        {activeTab === "comms" && <CommsManager />}
        {activeTab === "system" && <SystemView />}
      </main>

      <footer style={{
        borderTop: "1px solid var(--border-subtle)",
        padding: "18px 24px",
        textAlign: "center",
        fontSize: 12,
        color: "var(--text-muted)",
        background: "#ffffff"
      }}>
        CNC Tool Condition Monitor V2 · Dual-Spindle Autonomous Safety Architecture · ESP32 Master Firmware
      </footer>
    </div>
  );
};
