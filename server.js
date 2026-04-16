import express from "express";
import cors from "cors";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";

const app = express();
const PORT = 3001;

// Enable CORS for React app
app.use(cors());
app.use(express.json());

// Store latest sensor data
let sensorData = {
  temperature: 24,
  smoke: 87,
  flame: false,
  level: 0,
  pts: 0,
  sim: false,
  uptime: "00:00:00",
};

let serialPort;
let parser;
let isConnected = false;

// Function to initialize serial connection
async function initializeSerialPort() {
  try {
    // Try user-specified port first
    let portPath = process.env.ARDUINO_PORT;

    // Try common macOS patterns if no port is specified
    if (!portPath) {
      const commonPorts = [
        "/dev/tty.usbmodem14201",
        "/dev/tty.usbmodem14101",
        "/dev/tty.usbmodem14001",
        "/dev/tty.usbmodem13101",
        "/dev/tty.usbmodem11101",
        "/dev/tty.usbmodem11001",
        "/dev/tty.usbserial-0001",
        "/dev/ttyACM0",
        "COM3",
      ];

      for (const port of commonPorts) {
        try {
          await new Promise((resolve, reject) => {
            const test = new SerialPort({ path: port, baudRate: 9600 });
            test.on("open", () => {
              test.close();
              resolve();
            });
            test.on("error", reject);
            // Timeout after 1 second
            setTimeout(() => reject(new Error("Timeout")), 1000);
          });
          portPath = port;
          break;
        } catch (e) {
          // Port not available, continue
        }
      }
    }

    if (!portPath) {
      console.warn("⚠️  No Arduino found. Running in SIMULATION MODE.");
      console.log("Common Arduino ports (macOS): /dev/tty.usbmodemXXXX");
      console.log(
        "To connect an Arduino, set: export ARDUINO_PORT=/dev/tty.usbmodemXXXX",
      );
      console.log("Then run: npm run server");
      return;
    }

    serialPort = new SerialPort({ path: portPath, baudRate: 9600 });
    parser = serialPort.pipe(new ReadlineParser({ delimiter: "\n" }));

    serialPort.on("open", () => {
      isConnected = true;
      console.log(`✅ Connected to Arduino on ${portPath}`);
    });

    serialPort.on("error", (error) => {
      isConnected = false;
      console.error("❌ Serial port error:", error.message);
    });

    parser.on("data", (line) => {
      try {
        const data = JSON.parse(line.trim());
        sensorData = { ...sensorData, ...data };
        console.log("📡 Received data:", sensorData);
      } catch (e) {
        console.log("📝 Arduino log:", line.trim());
      }
    });
  } catch (error) {
    console.warn("⚠️  Could not initialize serial port:", error.message);
    console.log(
      "Running in SIMULATION MODE. Dashboard will show default values.",
    );
  }
}

// Initialize on startup
initializeSerialPort();

// API endpoint for sensor data
app.get("/api/sensors", (req, res) => {
  res.json(sensorData);
});

// API endpoint to check Arduino connection status
app.get("/api/status", (req, res) => {
  res.json({ connected: isConnected, sensorData });
});

// API endpoint to update simulation data
app.post("/api/sim/data", (req, res) => {
  const { temperature, smoke, flame } = req.body;
  if (sensorData.sim) {
    if (temperature !== undefined) sensorData.temperature = temperature;
    if (smoke !== undefined) sensorData.smoke = smoke;
    if (flame !== undefined) sensorData.flame = flame;
    updateFusionLevel();
  }
  res.json({ success: true, sensorData });
});

// API endpoint to toggle simulation mode
app.post("/api/sim/toggle", (req, res) => {
  sensorData.sim = !sensorData.sim;
  console.log(`🎮 Simulation mode: ${sensorData.sim ? "ON" : "OFF"}`);
  res.json({ success: true, sim: sensorData.sim });
});

// API endpoint to reset system
app.post("/api/reset", (req, res) => {
  sensorData = {
    temperature: 24,
    smoke: 87,
    flame: false,
    level: 0,
    pts: 0,
    sim: sensorData.sim,
    uptime: "00:00:00",
  };
  console.log("🔄 System reset");
  res.json({ success: true, sensorData });
});

// Function to update fusion level (fire detection logic)
function updateFusionLevel() {
  const T_THRESH = 45;
  const S_THRESH = 350;

  let pts = 0;
  if (sensorData.temperature > T_THRESH) pts++;
  if (sensorData.smoke > S_THRESH) pts++;
  if (sensorData.flame) pts++;

  sensorData.pts = pts;

  if (pts >= 2) {
    sensorData.level = 2; // DANGER
  } else if (pts === 1) {
    sensorData.level = 1; // WARNING
  } else {
    sensorData.level = 0; // SAFE
  }
}

// Start server
app.listen(PORT, () => {
  console.log("");
  console.log("╔════════════════════════════════════════╗");
  console.log("║  Fire Detection System Backend Server  ║");
  console.log("╚════════════════════════════════════════╝");
  console.log("");
  console.log(`🚀 Server running on http://localhost:${PORT}`);
  console.log(`📊 API endpoint: http://localhost:${PORT}/api/sensors`);
  console.log(
    `📡 Arduino connected: ${isConnected ? "✅ YES" : "❌ NO (Simulation Mode)"}`,
  );
  console.log("");
});
