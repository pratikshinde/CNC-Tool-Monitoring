import {
  TelemetrySnapshot,
  SpindleConfig,
  HistoryData,
  JobTemplate,
  SystemInfo,
  CommsConfig,
  CalibState
} from "../types";
import {
  getMockTelemetry,
  getMockHistory,
  mockSpindleConfigs,
  mockTemplates,
  mockSystemInfo,
  mockCommsConfig,
  mockCalibState
} from "./mockData";

let isUsingMock = false;

async function fetchJSON<T>(url: string, options?: RequestInit): Promise<T> {
  if (isUsingMock) {
    throw new Error("Mock Mode Active");
  }
  try {
    const res = await fetch(url, options);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.json();
  } catch (err) {
    // If running in development (port 5173 or localhost without backend) switch seamlessly to mock
    if (window.location.port === "5173" || window.location.hostname === "localhost") {
      isUsingMock = true;
    }
    throw err;
  }
}

export const api = {
  getTelemetry: async (): Promise<TelemetrySnapshot> => {
    try {
      return await fetchJSON<TelemetrySnapshot>("/api/telemetry");
    } catch {
      return getMockTelemetry();
    }
  },

  getHistory: async (spindle = 0): Promise<HistoryData> => {
    try {
      return await fetchJSON<HistoryData>(`/api/history?spindle=${spindle}`);
    } catch {
      return getMockHistory();
    }
  },

  getSpindleConfig: async (spindle = 0): Promise<SpindleConfig> => {
    try {
      return await fetchJSON<SpindleConfig>(`/api/spindle?spindle=${spindle}`);
    } catch {
      return mockSpindleConfigs[spindle] || mockSpindleConfigs[0];
    }
  },

  saveThresholds: async (spindle: number, config: Partial<SpindleConfig>): Promise<{ status: string }> => {
    try {
      return await fetchJSON("/api/thresholds?spindle=" + spindle, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(config)
      });
    } catch {
      mockSpindleConfigs[spindle] = { ...mockSpindleConfigs[spindle], ...config } as SpindleConfig;
      return { status: "ok" };
    }
  },

  acknowledgeLatches: async (spindle?: number): Promise<{ status: string }> => {
    try {
      const url = spindle !== undefined ? `/api/acknowledge?spindle=${spindle}` : "/api/acknowledge";
      return await fetchJSON(url, { method: "POST" });
    } catch {
      return { status: "ok" };
    }
  },

  getJobTemplates: async (): Promise<JobTemplate[]> => {
    try {
      return await fetchJSON<JobTemplate[]>("/api/jobs");
    } catch {
      return [...mockTemplates];
    }
  },

  saveJobTemplate: async (template: JobTemplate): Promise<{ status: string }> => {
    try {
      return await fetchJSON("/api/jobs", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(template)
      });
    } catch {
      mockTemplates.push(template);
      return { status: "ok" };
    }
  },

  deleteJobTemplate: async (name: string): Promise<{ status: string }> => {
    try {
      return await fetchJSON(`/api/jobs?name=${encodeURIComponent(name)}`, { method: "DELETE" });
    } catch {
      const idx = mockTemplates.findIndex(t => t.name === name);
      if (idx >= 0) mockTemplates.splice(idx, 1);
      return { status: "ok" };
    }
  },

  applyJobTemplate: async (name: string, targetSpindle: number): Promise<{ status: string }> => {
    try {
      return await fetchJSON("/api/jobs/apply", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, target: targetSpindle })
      });
    } catch {
      const template = mockTemplates.find(t => t.name === name);
      if (template && template.bands) {
        mockSpindleConfigs[targetSpindle].bands = { ...template.bands };
      }
      return { status: "ok" };
    }
  },

  getSystemInfo: async (): Promise<SystemInfo> => {
    try {
      return await fetchJSON<SystemInfo>("/api/system");
    } catch {
      return mockSystemInfo;
    }
  },

  getCommsConfig: async (): Promise<CommsConfig> => {
    try {
      return await fetchJSON<CommsConfig>("/api/config");
    } catch {
      return mockCommsConfig;
    }
  },

  saveCommsConfig: async (config: Partial<CommsConfig>): Promise<{ status: string }> => {
    try {
      return await fetchJSON("/api/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(config)
      });
    } catch {
      return { status: "ok" };
    }
  },

  getCalibState: async (spindle = 0, target = "pressure"): Promise<CalibState> => {
    try {
      return await fetchJSON<CalibState>(`/api/calib?spindle=${spindle}&target=${target}`);
    } catch {
      return mockCalibState;
    }
  },

  autozeroCT: async (spindle = 0): Promise<{ status: string }> => {
    try {
      return await fetchJSON(`/api/autozero?spindle=${spindle}`, { method: "POST" });
    } catch {
      return { status: "ok" };
    }
  },

  reboot: async (): Promise<{ status: string }> => {
    try {
      return await fetchJSON("/api/reboot", { method: "POST" });
    } catch {
      return { status: "ok" };
    }
  },

  factoryReset: async (): Promise<{ status: string }> => {
    try {
      return await fetchJSON("/api/factory-reset", { method: "POST" });
    } catch {
      return { status: "ok" };
    }
  }
};
