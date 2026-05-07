import express from "express";
import cors from "cors";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";

const app = express();
const PORT = 3001;

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
};

let serialPort = null;
let reconnectTimer = null;

// ── Thresholds (must match Arduino sketch) ────────────────────────────────────
const T_THRESH = 45;
const S_THRESH = 350;

// ── Find Arduino port using SerialPort.list() ─────────────────────────────────
async function findArduinoPort() {
  if (process.env.ARDUINO_PORT) {
    console.log(`Using manually specified port: ${process.env.ARDUINO_PORT}`);
    return process.env.ARDUINO_PORT;
  }

  try {
    const ports = await SerialPort.list();
    console.log("\nAvailable serial ports:");
    ports.forEach((p) =>
      console.log(
        `  ${p.path} — ${p.manufacturer || "unknown"} ${p.serialNumber ? `[${p.serialNumber}]` : ""}`,
      ),
    );

    const arduino = ports.find((p) => {
      const mfr = (p.manufacturer || "").toLowerCase();
      const path = (p.path || "").toLowerCase();
      return (
        mfr.includes("arduino") ||
        mfr.includes("wch") ||
        mfr.includes("silicon") ||
        mfr.includes("ftdi") ||
        path.includes("usbmodem") ||
        path.includes("usbserial") ||
        path.includes("ttyacm") ||
        path.includes("ttyusb")
      );
    });

    if (arduino) {
      console.log(`\n✅ Arduino found: ${arduino.path}`);
      return arduino.path;
    }

    console.warn("\n⚠️  No Arduino detected. Running in Simulation Mode.");
    console.log("Tip: set ARDUINO_PORT=/dev/tty.usbmodemXXXX and restart.");
    return null;
  } catch (err) {
    console.error("Failed to list serial ports:", err.message);
    return null;
  }
}

// ── Open serial connection ─────────────────────────────────────────────────────
async function connectToArduino() {
  if (serialPort && serialPort.isOpen) {
    try {
      serialPort.close();
    } catch (_) {}
  }

  const portPath = await findArduinoPort();
  if (!portPath) {
    sensorData.connected = false;
    return;
  }

  try {
    serialPort = new SerialPort({ path: portPath, baudRate: 9600 });
    const parser = serialPort.pipe(new ReadlineParser({ delimiter: "\n" }));

    serialPort.on("open", () => {
      sensorData.connected = true;
      console.log(`\n🔌 Connected: ${portPath}`);
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
    });

    serialPort.on("close", () => {
      sensorData.connected = false;
      console.warn("⚠️  Port closed. Reconnecting in 5s…");
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
        console.log("📟 Arduino:", trimmed);
        return;
      }
      try {
        const parsed = JSON.parse(trimmed);
        sensorData = {
          ...sensorData,
          temperature: parsed.temperature ?? sensorData.temperature,
          smoke: parsed.smoke ?? sensorData.smoke,
          flame: parsed.flame ?? sensorData.flame,
          level: parsed.level ?? sensorData.level,
          pts: parsed.pts ?? sensorData.pts,
          sim: parsed.sim ?? sensorData.sim,
          uptime: parsed.uptime ?? sensorData.uptime,
          connected: true,
        };
      } catch (_) {
        /* malformed line — ignore */
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
    connectToArduino();
  }, 5000);
}

// ── Fusion (used only for sim-mode API overrides) ─────────────────────────────
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
  });
  console.log("🔄 Reset");
  res.json({ ok: true, sensorData });
});

// ── Start ─────────────────────────────────────────────────────────────────────
app.listen(PORT, async () => {
  console.log("╔════════════════════════════════════════╗");
  console.log("║  Fire Detection System — Backend v2    ║");
  console.log("╚════════════════════════════════════════╝");
  console.log(`\n🚀 API  →  http://localhost:${PORT}/api/sensors`);
  console.log(`🔌 Ports →  http://localhost:${PORT}/api/ports\n`);
  await connectToArduino();
});
