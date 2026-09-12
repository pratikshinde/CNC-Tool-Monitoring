import React from "react";
import { LayoutDashboard, LineChart, Sliders, FileText, Gauge, Radio, Settings } from "lucide-react";

export type TabType = "dashboard" | "trends" | "thresholds" | "jobs" | "calibration" | "comms" | "system";

interface NavigationProps {
  activeTab: TabType;
  onTabChange: (tab: TabType) => void;
}

export const Navigation: React.FC<NavigationProps> = ({ activeTab, onTabChange }) => {
  const tabs: { id: TabType; label: string; icon: React.ReactNode }[] = [
    { id: "dashboard", label: "Dashboard", icon: <LayoutDashboard size={16} /> },
    { id: "trends", label: "Live Trends", icon: <LineChart size={16} /> },
    { id: "thresholds", label: "Thresholds & Alarms", icon: <Sliders size={16} /> },
    { id: "jobs", label: "Job Templates", icon: <FileText size={16} /> },
    { id: "calibration", label: "Sensor Calibration", icon: <Gauge size={16} /> },
    { id: "comms", label: "Communications", icon: <Radio size={16} /> },
    { id: "system", label: "System & OTA", icon: <Settings size={16} /> },
  ];

  return (
    <nav style={{
      padding: "16px 24px 8px",
      position: "sticky",
      top: 64,
      zIndex: 90,
      background: "transparent",
      pointerEvents: "none"
    }}>
      <div style={{
        maxWidth: 1360,
        margin: "0 auto",
        display: "flex",
        justifyContent: "flex-start",
        pointerEvents: "auto"
      }}>
        {/* Floating Pill Navigation Bar */}
        <div style={{
          display: "flex",
          gap: 6,
          background: "#ffffff",
          padding: "6px 8px",
          borderRadius: "9999px",
          border: "1px solid var(--border-subtle)",
          boxShadow: "0 4px 18px -2px rgba(15, 23, 42, 0.06), 0 1px 3px rgba(15, 23, 42, 0.03)",
          overflowX: "auto",
          maxWidth: "100%"
        }}>
          {tabs.map((tab) => {
            const isActive = activeTab === tab.id;
            return (
              <button
                key={tab.id}
                onClick={() => onTabChange(tab.id)}
                style={{
                  display: "flex",
                  alignItems: "center",
                  gap: 8,
                  padding: "10px 18px",
                  background: isActive
                    ? "linear-gradient(135deg, #2563eb 0%, #1d4ed8 100%)"
                    : "transparent",
                  border: "none",
                  borderRadius: "9999px",
                  color: isActive ? "#ffffff" : "var(--text-secondary)",
                  fontWeight: isActive ? 700 : 500,
                  fontSize: 13,
                  cursor: "pointer",
                  whiteSpace: "nowrap",
                  fontFamily: "inherit",
                  boxShadow: isActive ? "0 3px 12px rgba(37, 99, 235, 0.35)" : "none",
                  transform: isActive ? "scale(1.02)" : "scale(1)",
                  transition: "all 0.22s cubic-bezier(0.16, 1, 0.3, 1)"
                }}
                onMouseEnter={(e) => {
                  if (!isActive) {
                    e.currentTarget.style.color = "var(--blue-primary)";
                    e.currentTarget.style.background = "var(--blue-tint)";
                    e.currentTarget.style.transform = "translateY(-1px)";
                  }
                }}
                onMouseLeave={(e) => {
                  if (!isActive) {
                    e.currentTarget.style.color = "var(--text-secondary)";
                    e.currentTarget.style.background = "transparent";
                    e.currentTarget.style.transform = "translateY(0)";
                  }
                }}
              >
                <span style={{
                  display: "flex",
                  alignItems: "center",
                  color: isActive ? "#ffffff" : "var(--text-muted)",
                  transition: "color 0.2s ease"
                }}>
                  {tab.icon}
                </span>
                <span>{tab.label}</span>
              </button>
            );
          })}
        </div>
      </div>
    </nav>
  );
};
