import { useState, useEffect, useRef } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
  ResponsiveContainer,
} from "recharts";
import "./Dashboard.css";

const T_THRESH = 45,
  S_THRESH = 350,
  HIST = 20;

function App() {
  const [data, setData] = useState({
    temperature: 24,
    smoke: 87,
    flame: false,
    level: 0,
    pts: 0,
    sim: false,
    uptime: "00:00:00",
  });
  const [logs, setLogs] = useState([]);
  const [hT, setHT] = useState(Array(HIST).fill(24));
  const [hS, setHS] = useState(Array(HIST).fill(87));
  const [simTemp, setSimTemp] = useState(24);
  const [simSmoke, setSimSmoke] = useState(87);
  const [simFlame, setSimFlame] = useState(false);
  const logRef = useRef(null);

  useEffect(() => {
    const fetchData = async () => {
      try {
        const response = await fetch("http://localhost:3001/api/sensors");
        const newData = await response.json();
        setData(newData);

        // Update history
        setHT((prev) => [
          ...prev.slice(1),
          parseFloat(newData.temperature || 24),
        ]);
        setHS((prev) => [...prev.slice(1), newData.smoke || 87]);

        // Add log if level changed
        const levelNames = ["SAFE", "WARNING", "DANGER"];
        const colors = ["s", "w", "d"];
        if (newData.level !== data.level) {
          const logEntry = {
            ts: newData.uptime,
            msg: `TRANSITION → Level ${levelNames[newData.level]} (pts=${newData.pts}) | T=${newData.temperature?.toFixed(1) || "ERR"}C | Smoke=${newData.smoke} | Flame=${newData.flame ? "YES" : "NO"} | Sim:${newData.sim ? "ON" : "OFF"}`,
            c: colors[newData.level],
          };
          setLogs((prev) => [...prev.slice(-149), logEntry]);
        }
      } catch (error) {
        console.error("Failed to fetch data:", error);
      }
    };

    fetchData();
    const interval = setInterval(fetchData, 2000);
    return () => clearInterval(interval);
  }, [data.level]);

  // Send simulation values to backend
  useEffect(() => {
    if (data.sim) {
      fetch("http://localhost:3001/api/sim/data", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          temperature: simTemp,
          smoke: simSmoke,
          flame: simFlame,
        }),
      }).catch(() => {});
    }
  }, [simTemp, simSmoke, simFlame, data.sim]);

  useEffect(() => {
    if (logRef.current) {
      logRef.current.scrollTop = logRef.current.scrollHeight;
    }
  }, [logs]);

  const levelNames = ["SAFE", "WARNING", "DANGER"];
  const levelClasses = ["g", "a", "r"];

  // Create chart data for Recharts
  const chartData = hT.map((temp, idx) => ({
    index: idx,
    Temperature: temp,
    Smoke: hS[idx],
  }));

  return (
    <div className="app">
      <div className="topbar">
        <div className="logo-row">
          <div className="logo-icon">
            <svg viewBox="0 0 24 24" fill="white" width="17" height="17">
              <path d="M12 2L3 7v5c0 6 9 11 9 11s9-5 9-11V7l-9-5z" />
            </svg>
          </div>
          <div>
            <div className="sys-name">Fire Detection System</div>
            <div className="sys-ver">v3.0 — React</div>
          </div>
        </div>
        <div className="status-badge">
          <div
            className={`sdot ${data.level === 1 ? "warn" : data.level === 2 ? "danger" : ""}`}
          ></div>
          <span>
            {data.level === 0
              ? "System secure"
              : data.level === 1
                ? "Warning — monitor sensors"
                : "DANGER — alarm active"}
          </span>
        </div>
      </div>

      <div
        className={`alert ${data.level === 1 ? "warn show" : data.level === 2 ? "danger show" : ""}`}
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
          <path d="M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z" />
        </svg>
        <span>
          {data.level === 1
            ? "Warning — one sensor exceeded threshold. Continue monitoring."
            : "DANGER — fire detected. Full alarm active. Fan relay engaged."}
        </span>
      </div>

      <div className="metrics">
        <div className="met">
          <div className="met-label">Temperature</div>
          <div className={`met-val ${data.temperature > T_THRESH ? "r" : ""}`}>
            {data.temperature?.toFixed(1) || "—"}
            <span className="met-unit"> °C</span>
          </div>
          <div className="met-sub">
            {data.temperature > T_THRESH
              ? "above threshold ↑"
              : "threshold 45°C"}
          </div>
        </div>
        <div className="met">
          <div className="met-label">Smoke / gas</div>
          <div className={`met-val ${data.smoke > S_THRESH ? "r" : ""}`}>
            {data.smoke}
            <span className="met-unit"> ADC</span>
          </div>
          <div className="met-sub">
            {data.smoke > S_THRESH ? "above threshold ↑" : "threshold 350"}
          </div>
        </div>
        <div className="met">
          <div className="met-label">Flame</div>
          <div className={`met-val ${data.flame ? "r" : "g"}`}>
            {data.flame ? "YES" : "NO"}
          </div>
          <div className="met-sub">pin 2 · active low</div>
        </div>
        <div className="met">
          <div className="met-label">Alarm level</div>
          <div className={`met-val ${levelClasses[data.level]}`}>
            {levelNames[data.level]}
          </div>
          <div className="met-sub">{data.pts} / 3 points</div>
        </div>
      </div>

      <div className="row2">
        <div className="card">
          <div className="ctitle">Sensor history</div>
          <div className="legend">
            <div className="leg-item">
              <div
                className="leg-sq"
                style={{ background: "rgb(75, 192, 192)" }}
              ></div>
              <span>Temperature</span>
            </div>
            <div className="leg-item">
              <div
                className="leg-sq"
                style={{ background: "rgb(255, 99, 132)" }}
              ></div>
              <span>Smoke</span>
            </div>
          </div>
          <div className="chart-wrap">
            <ResponsiveContainer width="100%" height={300}>
              <LineChart data={chartData}>
                <CartesianGrid strokeDasharray="3 3" stroke="#333" />
                <XAxis dataKey="index" stroke="#888" />
                <YAxis stroke="#888" />
                <Tooltip
                  contentStyle={{
                    background: "#1a1a1a",
                    border: "1px solid #555",
                    borderRadius: "4px",
                  }}
                  cursor={{ stroke: "#fff", strokeWidth: 2 }}
                />
                <Legend />
                <Line
                  type="monotone"
                  dataKey="Temperature"
                  stroke="#4bc0c0"
                  dot={false}
                  isAnimationActive={false}
                  strokeWidth={2}
                />
                <Line
                  type="monotone"
                  dataKey="Smoke"
                  stroke="#ff6384"
                  dot={false}
                  isAnimationActive={false}
                  strokeWidth={2}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
        <div className="card">
          <div className="ctitle">LED status</div>
          <div className="leds">
            <div className="led-row">
              <div className={`led g ${data.level < 2 ? "on" : ""}`}></div>
              <span>Green LED (Safe + Warning)</span>
            </div>
            <div className="led-row">
              <div className={`led y ${data.level === 1 ? "on" : ""}`}></div>
              <span>Yellow LED (Warning only)</span>
            </div>
            <div className="led-row">
              <div className={`led r ${data.level === 2 ? "on" : ""}`}></div>
              <span>Red LED (Danger — blinks)</span>
            </div>
          </div>
          <div className="hw-row">
            <div className="hw-chip">
              <div className="hw-chip-label">Relay</div>
              <div
                className={`hw-chip-val ${data.level === 2 && !data.sim ? "active" : ""}`}
              >
                {data.level === 2 && !data.sim ? "ON" : "OFF"}
              </div>
            </div>
            <div className="hw-chip">
              <div className="hw-chip-label">Buzzer</div>
              <div
                className={`hw-chip-val ${data.level === 2 ? "active" : ""}`}
              >
                {data.level === 2 ? "Wailing" : "Silent"}
              </div>
            </div>
            <div className="hw-chip">
              <div className="hw-chip-label">Mode</div>
              <div className={`hw-chip-val ${data.sim ? "active" : ""}`}>
                {data.sim ? "SIM" : "Real"}
              </div>
            </div>
          </div>
        </div>
      </div>

      <div className="row3">
        <div className="card">
          <div className="ctitle">Sensor fusion</div>
          <div className="fusion-list">
            <div className="fi">
              <div className="fi-left">
                <div
                  className={`fi-dot ${data.temperature > T_THRESH ? "active" : ""}`}
                ></div>
                <span>Temperature &gt; 45°C</span>
              </div>
              <span
                className={`badge ${data.temperature > T_THRESH ? "hi" : "ok"}`}
              >
                {data.temperature > T_THRESH ? "HIGH" : "OK"}
              </span>
            </div>
            <div className="fi">
              <div className="fi-left">
                <div
                  className={`fi-dot ${data.smoke > S_THRESH ? "active" : ""}`}
                ></div>
                <span>Smoke &gt; 350 ADC</span>
              </div>
              <span className={`badge ${data.smoke > S_THRESH ? "hi" : "ok"}`}>
                {data.smoke > S_THRESH ? "HIGH" : "OK"}
              </span>
            </div>
            <div className="fi">
              <div className="fi-left">
                <div className={`fi-dot ${data.flame ? "active" : ""}`}></div>
                <span>Flame detected</span>
              </div>
              <span className={`badge ${data.flame ? "hi" : "ok"}`}>
                {data.flame ? "HIGH" : "OK"}
              </span>
            </div>
          </div>
          <div className="pts-bar">
            {[1, 2, 3].map((i) => (
              <div
                key={i}
                className={`pb p${i} ${data.pts >= i ? "on" : ""}`}
              ></div>
            ))}
          </div>
          <div className="pts-note">
            {data.pts} / 3 — {levelNames[data.level].toLowerCase()}
          </div>
        </div>

        <div className="card">
          <div className="ctitle">Controls</div>
          <div className="sliders">
            <div className="sl-group">
              <div className="sl-header">
                <span>Temperature (°C)</span>
                <span className="sl-val">{simTemp?.toFixed(1)}°C</span>
              </div>
              <input
                type="range"
                min="20"
                max="80"
                value={simTemp}
                onChange={(e) => setSimTemp(parseFloat(e.target.value))}
              />
              <div className="sl-marks">
                <span>20</span>
                <span>80</span>
              </div>
            </div>
            <div className="sl-group">
              <div className="sl-header">
                <span>Smoke (ADC)</span>
                <span className="sl-val">{simSmoke}</span>
              </div>
              <input
                type="range"
                min="50"
                max="600"
                value={simSmoke}
                onChange={(e) => setSimSmoke(parseInt(e.target.value))}
              />
              <div className="sl-marks">
                <span>50</span>
                <span>600</span>
              </div>
            </div>
          </div>
          <button
            className={`flame-btn ${simFlame ? "active" : ""}`}
            onClick={() => setSimFlame(!simFlame)}
          >
            <span>
              {simFlame
                ? "Flame detected — click to clear"
                : "No flame — click to trigger"}
            </span>
            <svg width="8" height="8" viewBox="0 0 24 24" fill="currentColor">
              <circle cx="12" cy="12" r="3" />
            </svg>
          </button>
          <div className="mode-btns">
            <button
              className={`mbtn ${data.sim ? "sim-on" : ""}`}
              onClick={() => {
                // Toggle simulation mode
                fetch(`http://localhost:3001/api/sim/toggle`, {
                  method: "POST",
                }).catch(() => {});
              }}
            >
              Simulation mode: {data.sim ? "ON" : "OFF"}
            </button>
            <button
              className="mbtn rst"
              onClick={() => {
                // Reset system
                setSimTemp(24);
                setSimSmoke(87);
                setSimFlame(false);
                fetch(`http://localhost:3001/api/reset`, {
                  method: "POST",
                }).catch(() => {});
              }}
            >
              Reset system
            </button>
          </div>
        </div>
      </div>

      <div className="card log-panel">
        <div className="log-head">
          <span>System logs</span>
          <span>Auto-scroll</span>
        </div>
        <div className="log-scroll" ref={logRef}>
          {logs.map((log, i) => (
            <div key={i} className="ll">
              <span className="ll-ts">[{log.ts}]</span>
              <span className={`ll-msg ${log.c}`}>{log.msg}</span>
            </div>
          ))}
        </div>
      </div>

      <div className="uptime-row">Uptime: {data.uptime}</div>
    </div>
  );
}

export default App;
