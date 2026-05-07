import express from "express";
import cors from "cors";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";

const app = express();
const PORT = 3001;

// Set BLYNK_TOKEN env var to enable server-side push notifications as backup
const BLYNK_TOKEN =
  process.env.BLYNK_TOKEN || "4tXML9n4uCwtbIGCPw20kBQJt7WJDXgk";

app.use(cors());
app.use(express.json());

// ── Sensor state ──────────────────────────────────────────────────────────────
let sensorData = {
  temperature: 24,
  smoke: 87,
  flame: false,
  level: 0,
  pts: 0,
  sim: false,
  uptime: "00:00:00",
  connected: false,
  wifi: false,
  ip: "—",
  blynk: false,
  lastNotification: null,
};

let serialPort = null;
let reconnectTimer = null;
let prevLevel = 0; // for alarm-transition detection

// ── Thresholds — must match ESP32 sketch (12-bit ADC: 0–4095) ────────────────
const T_THRESH = 45;
const S_THRESH = 500;

// ── Blynk HTTP push notification (server-side backup) ────────────────────────
async function sendBlynkNotification(level, data) {
  if (!BLYNK_TOKEN) return;

  const event = level === 2 ? "fire_alert" : "smoke_warning";
  const tempStr =
    typeof data.temperature === "number" ? data.temperature.toFixed(1) : "ERR";
  const desc =
    level === 2
      ? `DANGER: Fire detected! T=${tempStr}°C Smoke=${data.smoke}`
      : `WARNING: Sensor threshold exceeded. T=${tempStr}°C Smoke=${data.smoke}`;

  try {
    const url =
      `https://blynk.cloud/external/api/logEvent` +
      `?token=${encodeURIComponent(BLYNK_TOKEN)}` +
      `&code=${event}` +
      `&description=${encodeURIComponent(desc)}`;
    const res = await fetch(url);
    const ts = new Date().toISOString();
    sensorData.lastNotification = {
      ts,
      event,
      status: res.ok ? "sent" : "failed",
    };
    console.log(`🔔 Blynk [${event}]: HTTP ${res.status}`);
  } catch (err) {
    console.error("❌ Blynk notification failed:", err.message);
    sensorData.lastNotification = {
      ts: new Date().toISOString(),
      event,
      status: "error",
    };
  }
}

// ── Find ESP32 serial port ────────────────────────────────────────────────────
async function findESP32Port() {
  if (process.env.ARDUINO_PORT) {
    console.log(`Using manually specified port: ${process.env.ARDUINO_PORT}`);
    return process.env.ARDUINO_PORT;
  }

  try {
    const ports = await SerialPort.list();
    console.log("\nAvailable serial ports:");
    ports.forEach((p) =>
      console.log(
        `  ${p.path} — ${p.manufacturer || "unknown"}${p.serialNumber ? ` [${p.serialNumber}]` : ""}`,
      ),
    );

    const esp32 = ports.find((p) => {
      const mfr = (p.manufacturer || "").toLowerCase();
      const path = (p.path || "").toLowerCase();
      return (
        mfr.includes("silicon") || // CP210x — most common ESP32 USB chip
        mfr.includes("cp210") ||
        mfr.includes("ch340") || // CH340G — cheap ESP32 clone boards
        mfr.includes("wch") || // WCH (CH340 vendor)
        mfr.includes("arduino") ||
        mfr.includes("ftdi") ||
        path.includes("usbmodem") ||
        path.includes("usbserial") ||
        path.includes("ttyacm") ||
        path.includes("ttyusb")
      );
    });

    if (esp32) {
      console.log(`\n✅ ESP32 found: ${esp32.path}`);
      return esp32.path;
    }

    console.warn("\n⚠️  No ESP32 detected. Waiting for connection…");
    console.log("Tip: set ARDUINO_PORT=/dev/tty.usbserialXXXX and restart.");
    return null;
  } catch (err) {
    console.error("Failed to list serial ports:", err.message);
    return null;
  }
}

// ── Open serial connection (ESP32 @ 115200 baud) ──────────────────────────────
async function connectToESP32() {
  if (serialPort && serialPort.isOpen) {
    try {
      serialPort.close();
    } catch (_) {}
  }

  const portPath = await findESP32Port();
  if (!portPath) {
    sensorData.connected = false;
    return;
  }

  try {
    // ESP32 uses 115200 baud (old Arduino sketch used 9600 — that was the bug)
    serialPort = new SerialPort({ path: portPath, baudRate: 115200 });
    const parser = serialPort.pipe(new ReadlineParser({ delimiter: "\n" }));

    serialPort.on("open", () => {
      sensorData.connected = true;
      console.log(`\n🔌 Connected: ${portPath} @ 115200 baud`);
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
    });

    serialPort.on("close", () => {
      sensorData.connected = false;
      console.warn("⚠️  Port closed. Reconnecting in 5 s…");
      scheduleReconnect();
    });

    serialPort.on("error", (err) => {
      sensorData.connected = false;
      console.error("❌ Serial error:", err.message);
      scheduleReconnect();
    });

    parser.on("data", (line) => {
      const trimmed = line.trim();
      if (!trimmed.startsWith("{")) {
        console.log("📟 ESP32:", trimmed);
        return;
      }
      try {
        const parsed = JSON.parse(trimmed);
        const newLevel = parsed.level ?? sensorData.level;

        // Send Blynk notification on alarm escalation (not in sim mode)
        if (newLevel > prevLevel && newLevel >= 1 && !sensorData.sim) {
          sendBlynkNotification(newLevel, { ...sensorData, ...parsed });
        }
        prevLevel = newLevel;

        sensorData = {
          ...sensorData,
          temperature: parsed.temperature ?? sensorData.temperature,
          smoke: parsed.smoke ?? sensorData.smoke,
          flame: parsed.flame ?? sensorData.flame,
          level: newLevel,
          pts: parsed.pts ?? sensorData.pts,
          sim: parsed.sim ?? sensorData.sim,
          uptime: parsed.uptime ?? sensorData.uptime,
          wifi: parsed.wifi ?? sensorData.wifi,
          ip: parsed.ip ?? sensorData.ip,
          blynk: parsed.blynk ?? sensorData.blynk,
          connected: true,
        };
      } catch (_) {
        /* malformed JSON line — ignore */
      }
    });
  } catch (err) {
    sensorData.connected = false;
    console.error("❌ Could not open port:", err.message);
    scheduleReconnect();
  }
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectToESP32();
  }, 5000);
}

// ── Fusion recalc (used only for sim-mode overrides) ─────────────────────────
function recalcFusion() {
  const pts =
    (sensorData.temperature > T_THRESH ? 1 : 0) +
    (sensorData.smoke > S_THRESH ? 1 : 0) +
    (sensorData.flame ? 1 : 0);
  sensorData.pts = pts;
  sensorData.level = pts >= 2 ? 2 : pts === 1 ? 1 : 0;
}

// ── API routes ────────────────────────────────────────────────────────────────
app.get("/api/sensors", (_req, res) => res.json(sensorData));

app.get("/api/status", (_req, res) =>
  res.json({ connected: sensorData.connected, port: serialPort?.path ?? null }),
);

app.get("/api/ports", async (_req, res) => {
  try {
    res.json(await SerialPort.list());
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.post("/api/sim/data", (req, res) => {
  if (!sensorData.sim)
    return res.status(400).json({ error: "Simulation mode is not active" });
  const { temperature, smoke, flame } = req.body;
  if (temperature !== undefined) sensorData.temperature = Number(temperature);
  if (smoke !== undefined) sensorData.smoke = Number(smoke);
  if (flame !== undefined) sensorData.flame = Boolean(flame);
  recalcFusion();
  res.json({ ok: true, sensorData });
});

app.post("/api/sim/toggle", (_req, res) => {
  sensorData.sim = !sensorData.sim;
  if (!sensorData.sim) {
    sensorData.temperature = 24;
    sensorData.smoke = 87;
    sensorData.flame = false;
    recalcFusion();
  }
  console.log(`🎮 Simulation: ${sensorData.sim ? "ON" : "OFF"}`);
  res.json({ ok: true, sim: sensorData.sim });
});

app.post("/api/reset", (_req, res) => {
  Object.assign(sensorData, {
    temperature: 24,
    smoke: 87,
    flame: false,
    level: 0,
    pts: 0,
    uptime: "00:00:00",
    lastNotification: null,
  });
  prevLevel = 0;
  console.log("🔄 Reset");
  res.json({ ok: true, sensorData });
});

// Manual Blynk test notification (requires BLYNK_TOKEN env var)
app.post("/api/notify/test", async (_req, res) => {
  if (!BLYNK_TOKEN)
    return res
      .status(400)
      .json({ error: "BLYNK_TOKEN not configured on server" });
  await sendBlynkNotification(2, sensorData);
  res.json({ ok: true, notification: sensorData.lastNotification });
});

// ── Start ─────────────────────────────────────────────────────────────────────
app.listen(PORT, async () => {
  console.log("╔══════════════════════════════════════════════╗");
  console.log("║  Fire Detection System — Backend v3.0 ESP32  ║");
  console.log("╚══════════════════════════════════════════════╝");
  console.log(`\n🚀 API     →  http://localhost:${PORT}/api/sensors`);
  console.log(`🔌 Ports   →  http://localhost:${PORT}/api/ports`);
  console.log(
    `🔔 Blynk   →  ${BLYNK_TOKEN ? "ENABLED (server-side backup)" : "DISABLED — set BLYNK_TOKEN env var to enable"}\n`,
  );
  await connectToESP32();
});
