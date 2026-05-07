import { useState, useEffect, useRef, useCallback } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
} from "recharts";
import "./Dashboard.css";

const API = "http://localhost:3001";
const T_THRESH = 45;
const S_THRESH = 350;
const HIST = 20;

// ── Custom hook: poll the backend every 2 s ───────────────────────────────────
function useSensorData() {
  const [data, setData] = useState({
    temperature: 24,
    smoke: 87,
    flame: false,
    level: 0,
    pts: 0,
    sim: false,
    uptime: "00:00:00",
    connected: false,
  });
  const [apiOk, setApiOk] = useState(true);

  useEffect(() => {
    let cancelled = false;
    const tick = async () => {
      try {
        const res = await fetch(`${API}/api/sensors`);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const json = await res.json();
        if (!cancelled) {
          setData(json);
          setApiOk(true);
        }
      } catch {
        if (!cancelled) setApiOk(false);
      }
    };
    tick();
    const id = setInterval(tick, 2000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  return { data, apiOk };
}

// ── History hook ──────────────────────────────────────────────────────────────
function useHistory(temp, smoke) {
  const [hist, setHist] = useState(() => ({
    t: Array(HIST).fill(24),
    s: Array(HIST).fill(87),
  }));
  useEffect(() => {
    setHist((prev) => ({
      t: [...prev.t.slice(1), parseFloat(temp) || 24],
      s: [...prev.s.slice(1), Number(smoke) || 87],
    }));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [temp, smoke]);
  return hist;
}

export default function App() {
  const { data, apiOk } = useSensorData();
  const hist = useHistory(data.temperature, data.smoke);
  const prevLevelRef = useRef(-1);
  const logRef = useRef(null);
  const [logs, setLogs] = useState([]);

  // Sim slider state (local — pushed to server only when sim is on)
  const [simTemp, setSimTemp] = useState(24);
  const [simSmoke, setSimSmoke] = useState(87);
  const [simFlame, setSimFlame] = useState(false);

  // ── Log transitions ─────────────────────────────────────────────────────────
  useEffect(() => {
    if (data.level === prevLevelRef.current) return;
    prevLevelRef.current = data.level;
    const names = ["SAFE", "WARNING", "DANGER"];
    const cls = ["s", "w", "d"];
    const entry = {
      ts: data.uptime,
      msg: `TRANSITION → Level ${names[data.level]} (pts=${data.pts}) | T=${typeof data.temperature === "number" ? data.temperature.toFixed(1) : "ERR"}C | Smoke=${data.smoke} | Flame=${data.flame ? "YES" : "NO"} | Sim:${data.sim ? "ON" : "OFF"}`,
      c: cls[data.level],
    };
    setLogs((prev) => [...prev.slice(-149), entry]);
  }, [
    data.level,
    data.pts,
    data.temperature,
    data.smoke,
    data.flame,
    data.sim,
    data.uptime,
  ]);

  // ── Auto-scroll log ─────────────────────────────────────────────────────────
  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [logs]);

  // ── Push sim values to server (debounced via useCallback + effect) ──────────
  const pushSimData = useCallback(() => {
    if (!data.sim) return;
    fetch(`${API}/api/sim/data`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        temperature: simTemp,
        smoke: simSmoke,
        flame: simFlame,
      }),
    }).catch(() => {});
  }, [data.sim, simTemp, simSmoke, simFlame]);

  useEffect(() => {
    const id = setTimeout(pushSimData, 150); // 150 ms debounce
    return () => clearTimeout(id);
  }, [pushSimData]);

  // ── API helpers ─────────────────────────────────────────────────────────────
  const post = (path) =>
    fetch(`${API}${path}`, { method: "POST" }).catch(() => {});

  function toggleSim() {
    post("/api/sim/toggle");
    if (!data.sim) {
      setSimTemp(24);
      setSimSmoke(87);
      setSimFlame(false);
    }
  }

  function resetSystem() {
    setSimTemp(24);
    setSimSmoke(87);
    setSimFlame(false);
    post("/api/reset");
    setLogs([]);
  }

  // ── Derived display ─────────────────────────────────────────────────────────
  const NAMES = ["SAFE", "WARNING", "DANGER"];
  const L_CLS = ["g", "a", "r"];
  const lv = data.level;

  const chartData = hist.t.map((t, i) => ({
    i,
    Temperature: t,
    Smoke: hist.s[i],
  }));

  const tempDisplay =
    typeof data.temperature === "number" ? data.temperature.toFixed(1) : "—";

  // ── Render ──────────────────────────────────────────────────────────────────
  return (
    <div className="app">
      {/* ── Top bar ── */}
      <div className="topbar">
        <div className="logo-row">
          <div className="logo-icon">
            <svg viewBox="0 0 24 24" fill="white" width="17" height="17">
              <path d="M17.66 11.2c-.23-.3-.51-.56-.77-.82-.67-.6-1.43-1.03-2.07-1.66C13.33 7.26 13 4.85 13.95 3c-.95.23-1.78.75-2.49 1.32C8.6 6.29 7.8 9.69 8.5 12.66c-.43-.42-.59-1.09-.59-1.67C7.5 10.59 7.73 9.86 8 9.2l-.5-.5c-.7.43-1.25 1.18-1.25 2.08 0 2.05 1.56 3.56 3.84 3.68C5.8 16.11 5 17.96 5 20h14c0-2.04-.8-3.89-2.09-5.24l-.25-.56z" />
            </svg>
          </div>
          <div>
            <div className="sys-name">Fire Detection System</div>
            <div className="sys-ver">
              Arduino Uno R3 · v3.0 · DHT11 + MQ-2 + IR
            </div>
          </div>
        </div>
        <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
          {/* Arduino connection chip */}
          <div className={`conn-chip ${data.connected ? "ok" : "off"}`}>
            <span className={`conn-dot ${data.connected ? "ok" : "off"}`} />
            {data.connected ? "Arduino connected" : "No Arduino"}
          </div>
          {/* API server chip */}
          {!apiOk && (
            <div className="conn-chip off">
              <span className="conn-dot off" />
              Server offline — run{" "}
              <code style={{ fontSize: 10 }}>npm run server</code>
            </div>
          )}
          <div className="status-badge">
            <div
              className={`sdot ${lv === 1 ? "warn" : lv === 2 ? "danger" : ""}`}
            />
            <span>
              {lv === 0
                ? "System secure"
                : lv === 1
                  ? "Warning — monitor sensors"
                  : "DANGER — alarm active"}
            </span>
          </div>
        </div>
      </div>

      {/* ── Alert banner ── */}
      <div
        className={`alert ${lv === 1 ? "warn show" : lv === 2 ? "danger show" : ""}`}
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
          <path d="M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z" />
        </svg>
        <span>
          {lv === 1
            ? "Warning — one sensor exceeded threshold. Continue monitoring."
            : "DANGER — fire detected. Full alarm active. Fan relay engaged."}
        </span>
      </div>

      {/* ── Metric cards ── */}
      <div className="metrics">
        <div className="met">
          <div className="met-label">Temperature</div>
          <div className={`met-val ${data.temperature > T_THRESH ? "r" : ""}`}>
            {tempDisplay}
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
          <div className={`met-val ${L_CLS[lv]}`}>{NAMES[lv]}</div>
          <div className="met-sub">{data.pts} / 3 points</div>
        </div>
      </div>

      {/* ── Chart + LED panel ── */}
      <div className="row2">
        <div className="card">
          <div className="ctitle">Sensor history</div>
          <div className="legend">
            <div className="leg-item">
              <div className="leg-sq" style={{ background: "#4bc0c0" }} />
              <span>Temperature (°C)</span>
            </div>
            <div className="leg-item">
              <div className="leg-sq" style={{ background: "#ff6384" }} />
              <span>Smoke (ADC)</span>
            </div>
          </div>
          <div className="chart-wrap" style={{ height: 220 }}>
            <ResponsiveContainer width="100%" height="100%">
              <LineChart
                data={chartData}
                margin={{ top: 5, right: 5, left: -20, bottom: 0 }}
              >
                <CartesianGrid strokeDasharray="3 3" stroke="#e5e3de" />
                <XAxis dataKey="i" hide />
                <YAxis
                  yAxisId="t"
                  domain={[0, 80]}
                  tick={{ fontSize: 10 }}
                  stroke="#4bc0c0"
                />
                <YAxis
                  yAxisId="s"
                  domain={[0, 1023]}
                  tick={{ fontSize: 10 }}
                  stroke="#ff6384"
                  orientation="right"
                />
                <Tooltip
                  contentStyle={{
                    background: "#fff",
                    border: "1px solid #e5e3de",
                    borderRadius: 6,
                    fontSize: 11,
                  }}
                  cursor={{ stroke: "#d0cec8" }}
                />
                <Line
                  yAxisId="t"
                  type="monotone"
                  dataKey="Temperature"
                  stroke="#4bc0c0"
                  dot={false}
                  strokeWidth={2}
                  isAnimationActive={false}
                />
                <Line
                  yAxisId="s"
                  type="monotone"
                  dataKey="Smoke"
                  stroke="#ff6384"
                  dot={false}
                  strokeWidth={2}
                  isAnimationActive={false}
                />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>

        <div className="card">
          <div className="ctitle">Output state</div>
          <div className="leds">
            <div className="led-row">
              <div className={`led g ${lv < 2 ? "on" : ""}`} />
              Green — safe &amp; warning
            </div>
            <div className="led-row">
              <div className={`led y ${lv === 1 ? "on" : ""}`} />
              Yellow — warning
            </div>
            <div className="led-row">
              <div className={`led r ${lv === 2 ? "on" : ""}`} />
              Red — danger
            </div>
          </div>
          <div className="hw-row">
            <div className="hw-chip">
              <div className="hw-chip-label">Relay/fan</div>
              <div
                className={`hw-chip-val ${lv === 2 && !data.sim ? "active" : ""}`}
              >
                {lv === 2 && !data.sim ? "ON" : "OFF"}
              </div>
            </div>
            <div className="hw-chip">
              <div className="hw-chip-label">Buzzer</div>
              <div className={`hw-chip-val ${lv === 2 ? "active" : ""}`}>
                {lv === 2 ? "Wailing" : "Silent"}
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

      {/* ── Fusion + Controls ── */}
      <div className="row3">
        <div className="card">
          <div className="ctitle">Sensor fusion</div>
          <div className="fusion-list">
            {[
              {
                label: "Temperature > 45°C",
                active: data.temperature > T_THRESH,
              },
              { label: "Smoke > 350 ADC", active: data.smoke > S_THRESH },
              { label: "Flame detected", active: data.flame },
            ].map(({ label, active }) => (
              <div className="fi" key={label}>
                <div className="fi-left">
                  <div className={`fi-dot ${active ? "active" : ""}`} />
                  <span>{label}</span>
                </div>
                <span className={`badge ${active ? "hi" : "ok"}`}>
                  {active ? "HIGH" : "OK"}
                </span>
              </div>
            ))}
          </div>
          <div className="pts-bar">
            {[1, 2, 3].map((i) => (
              <div
                key={i}
                className={`pb p${i} ${data.pts >= i ? "on" : ""}`}
              />
            ))}
          </div>
          <div className="pts-note">
            {data.pts} / 3 — {NAMES[lv].toLowerCase()}
          </div>
        </div>

        <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
          {/* Sliders (only active in sim mode) */}
          <div className="card">
            <div className="ctitle">
              Sensor overrides
              {!data.sim && (
                <span
                  style={{
                    color: "var(--text3)",
                    fontWeight: 400,
                    marginLeft: 6,
                  }}
                >
                  — enable simulation mode to use
                </span>
              )}
            </div>
            <div
              className="sliders"
              style={{
                opacity: data.sim ? 1 : 0.4,
                pointerEvents: data.sim ? "auto" : "none",
              }}
            >
              <div className="sl-group">
                <div className="sl-header">
                  <span>Temperature (°C)</span>
                  <span className="sl-val">{simTemp.toFixed(1)}°C</span>
                </div>
                <input
                  type="range"
                  min="20"
                  max="80"
                  value={simTemp}
                  onChange={(e) => setSimTemp(parseFloat(e.target.value))}
                />
                <div className="sl-marks">threshold: 45°C</div>
              </div>
              <div className="sl-group">
                <div className="sl-header">
                  <span>Smoke / gas (ADC)</span>
                  <span className="sl-val">{simSmoke}</span>
                </div>
                <input
                  type="range"
                  min="0"
                  max="1023"
                  value={simSmoke}
                  onChange={(e) => setSimSmoke(parseInt(e.target.value))}
                />
                <div className="sl-marks">threshold: 350</div>
              </div>
              <button
                className={`flame-btn ${simFlame ? "active" : ""}`}
                onClick={() => setSimFlame((v) => !v)}
              >
                <span>
                  {simFlame
                    ? "Flame detected — click to clear"
                    : "No flame — click to trigger"}
                </span>
                <svg
                  width="8"
                  height="8"
                  viewBox="0 0 24 24"
                  fill="currentColor"
                >
                  <circle cx="12" cy="12" r="6" />
                </svg>
              </button>
            </div>
          </div>

          {/* Mode buttons */}
          <div className="card">
            <div className="ctitle">Controls</div>
            <div className="mode-btns">
              <button
                className={`mbtn ${data.sim ? "sim-on" : ""}`}
                onClick={toggleSim}
              >
                {data.sim
                  ? "⏹ Disable simulation"
                  : "▶ Enable simulation mode"}
              </button>
              <button className="mbtn rst" onClick={resetSystem}>
                ↺ Reset system
              </button>
            </div>
            <div className="uptime-row">Uptime: {data.uptime}</div>
          </div>
        </div>
      </div>

      {/* ── Serial log ── */}
      <div className="card log-panel">
        <div className="log-head">
          <div className="ctitle" style={{ marginBottom: 0 }}>
            Serial monitor · 9600 baud
          </div>
          <button
            onClick={() => setLogs([])}
            style={{
              fontSize: 11,
              padding: "3px 9px",
              border: "1px solid var(--border)",
              borderRadius: 6,
              background: "transparent",
              cursor: "pointer",
              fontFamily: "var(--sans)",
              color: "var(--text2)",
            }}
          >
            Clear
          </button>
        </div>
        <div className="log-scroll" ref={logRef}>
          {logs.length === 0 ? (
            <div style={{ color: "var(--text3)", fontStyle: "italic" }}>
              No events yet…
            </div>
          ) : (
            logs.map((l, i) => (
              <div key={i} className="ll">
                <span className="ll-ts">[{l.ts}]</span>
                <span className={`ll-msg ${l.c}`}>{l.msg}</span>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  );
}
