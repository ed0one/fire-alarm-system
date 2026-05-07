import { useState, useEffect, useRef, useCallback } from "react";
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer,
} from "recharts";
import "./Dashboard.css";

// Vite proxy forwards /api → localhost:3001 (see vite.config.js)
const API = "";
const T_THRESH = 45;
const S_THRESH = 500;   // ESP32 12-bit ADC (was 350 for Arduino 10-bit — that was the bug)
const HIST     = 30;

// ── Data hook: poll backend every 2 s ────────────────────────────────────────
function useSensorData() {
  const [data, setData] = useState({
    temperature:      24,
    smoke:            87,
    flame:            false,
    level:            0,
    pts:              0,
    sim:              false,
    uptime:           "00:00:00",
    connected:        false,
    wifi:             false,
    ip:               "—",
    blynk:            false,
    lastNotification: null,
  });
  const [apiOk, setApiOk] = useState(true);

  useEffect(() => {
    let cancelled = false;
    const tick = async () => {
      try {
        const res  = await fetch(`${API}/api/sensors`);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const json = await res.json();
        if (!cancelled) { setData(json); setApiOk(true); }
      } catch {
        if (!cancelled) setApiOk(false);
      }
    };
    tick();
    const id = setInterval(tick, 2000);
    return () => { cancelled = true; clearInterval(id); };
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
  }, [temp, smoke]); // eslint-disable-line react-hooks/exhaustive-deps
  return hist;
}

// ── Root component ────────────────────────────────────────────────────────────
export default function App() {
  const { data, apiOk } = useSensorData();
  const hist             = useHistory(data.temperature, data.smoke);
  const prevLevelRef     = useRef(-1);
  const logRef           = useRef(null);
  const [logs, setLogs]  = useState([]);
  const [notifications, setNotifications] = useState([]);

  // Simulation slider state
  const [simTemp,  setSimTemp]  = useState(24);
  const [simSmoke, setSimSmoke] = useState(87);
  const [simFlame, setSimFlame] = useState(false);

  // ── Log alarm transitions ────────────────────────────────────────────────────
  useEffect(() => {
    if (data.level === prevLevelRef.current) return;
    prevLevelRef.current = data.level;

    const NAMES = ["SAFE", "WARNING", "DANGER"];
    const cls   = ["s",    "w",       "d"];
    const entry = {
      ts:  data.uptime,
      msg: `→ ${NAMES[data.level]} (pts=${data.pts}) | T=${typeof data.temperature === "number" ? data.temperature.toFixed(1) : "ERR"}°C | Smoke=${data.smoke} | Flame=${data.flame ? "YES" : "NO"}`,
      c:   cls[data.level],
    };
    setLogs((prev) => [...prev.slice(-149), entry]);

    if (data.level >= 1) {
      setNotifications((prev) => [
        {
          ts:    data.uptime,
          level: data.level,
          label: NAMES[data.level],
          via:   data.blynk ? "ESP32 → Blynk" : "Server → Blynk",
        },
        ...prev.slice(0, 9),
      ]);
    }
  }, [data.level, data.pts, data.temperature, data.smoke, data.flame, data.uptime, data.blynk]);

  // ── Auto-scroll log ──────────────────────────────────────────────────────────
  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [logs]);

  // ── Push sim values to server (150 ms debounce) ───────────────────────────────
  const pushSimData = useCallback(() => {
    if (!data.sim) return;
    fetch(`${API}/api/sim/data`, {
      method:  "POST",
      headers: { "Content-Type": "application/json" },
      body:    JSON.stringify({ temperature: simTemp, smoke: simSmoke, flame: simFlame }),
    }).catch(() => {});
  }, [data.sim, simTemp, simSmoke, simFlame]);

  useEffect(() => {
    const id = setTimeout(pushSimData, 150);
    return () => clearTimeout(id);
  }, [pushSimData]);

  const post = (path) => fetch(`${API}${path}`, { method: "POST" }).catch(() => {});

  function toggleSim() {
    post("/api/sim/toggle");
    if (!data.sim) { setSimTemp(24); setSimSmoke(87); setSimFlame(false); }
  }

  function resetSystem() {
    setSimTemp(24); setSimSmoke(87); setSimFlame(false);
    post("/api/reset");
    setLogs([]);
    setNotifications([]);
  }

  // ── Derived values ─────────────────────────────────────────────────────────
  const NAMES   = ["SAFE", "WARNING", "DANGER"];
  const lv      = data.level;
  const tempStr = typeof data.temperature === "number"
    ? data.temperature.toFixed(1) : "—";

  const chartData = hist.t.map((t, i) => ({
    i,
    "Temp (°C)": t,
    "Smoke (ADC)": hist.s[i],
  }));

  // ── Render ─────────────────────────────────────────────────────────────────
  return (
    <div className="app">

      {/* ── Top bar ── */}
      <header className="topbar">
        <div className="logo-row">
          <div className="logo-icon">
            <svg viewBox="0 0 24 24" fill="white" width="17" height="17">
              <path d="M17.66 11.2c-.23-.3-.51-.56-.77-.82-.67-.6-1.43-1.03-2.07-1.66C13.33 7.26 13 4.85 13.95 3c-.95.23-1.78.75-2.49 1.32C8.6 6.29 7.8 9.69 8.5 12.66c-.43-.42-.59-1.09-.59-1.67C7.5 10.59 7.73 9.86 8 9.2l-.5-.5c-.7.43-1.25 1.18-1.25 2.08 0 2.05 1.56 3.56 3.84 3.68C5.8 16.11 5 17.96 5 20h14c0-2.04-.8-3.89-2.09-5.24l-.25-.56z" />
            </svg>
          </div>
          <div>
            <div className="sys-name">Fire Detection System</div>
            <div className="sys-ver">ESP32 DevKit · v4.0 · DHT11 + MQ-2 + IR Flame</div>
          </div>
        </div>

        <div className="topbar-chips">
          <Chip ok={apiOk}          label={apiOk ? "Server" : "Server offline"} />
          <Chip ok={data.connected} label={data.connected ? "ESP32" : "No ESP32"} />
          {data.connected && (
            <Chip ok={data.wifi} icon="wifi" label={data.wifi ? (data.ip !== "—" ? data.ip : "WiFi") : "No WiFi"} />
          )}
          {data.connected && (
            <Chip ok={data.blynk} variant="blynk" label={data.blynk ? "Blynk" : "Blynk offline"} />
          )}
          <div className={`status-badge lv${lv}`}>
            <div className={`sdot ${lv === 1 ? "warn" : lv === 2 ? "danger" : ""}`} />
            <span>{lv === 0 ? "Secure" : lv === 1 ? "Warning" : "DANGER"}</span>
          </div>
        </div>
      </header>

      {/* ── Alert banner ── */}
      {lv > 0 && (
        <div className={`alert ${lv === 1 ? "warn" : "danger"}`}>
          <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor" style={{ flexShrink: 0 }}>
            <path d="M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z" />
          </svg>
          <span>
            {lv === 1
              ? "Warning — a sensor threshold has been exceeded. Continue monitoring."
              : "DANGER — fire conditions detected. Alarm active · Fan relay engaged · Blynk notified."}
          </span>
        </div>
      )}

      {/* ── Metric cards ── */}
      <div className="metrics">
        <MetCard
          label="Temperature"
          value={tempStr} unit="°C"
          sub={data.temperature > T_THRESH ? "above 45°C threshold ↑" : "threshold · 45°C"}
          state={data.temperature > T_THRESH ? "danger" : "ok"}
        />
        <MetCard
          label="Smoke / Gas"
          value={data.smoke} unit="ADC"
          sub={data.smoke > S_THRESH ? "above 500 threshold ↑" : "threshold · 500 (12-bit)"}
          state={data.smoke > S_THRESH ? "danger" : "ok"}
        />
        <MetCard
          label="Flame"
          value={data.flame ? "YES" : "NO"}
          sub="IR sensor · active low"
          state={data.flame ? "danger" : "safe"}
        />
        <MetCard
          label="Alarm Level"
          value={NAMES[lv]}
          sub={`${data.pts} / 3 triggers active`}
          state={lv === 2 ? "danger" : lv === 1 ? "warn" : "safe"}
        />
      </div>

      {/* ── Chart + Output state ── */}
      <div className="row2">
        <div className="card">
          <div className="card-head">
            <span className="ctitle">Sensor History</span>
            <div className="legend">
              <LegendItem color="#4bc0c0" label="Temp (°C)" />
              <LegendItem color="#f97316" label="Smoke (ADC)" />
            </div>
          </div>
          <div className="chart-wrap">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={chartData} margin={{ top: 5, right: 5, left: -20, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="var(--border)" />
                <XAxis dataKey="i" hide />
                <YAxis yAxisId="t" domain={[0, 80]}   tick={{ fontSize: 10 }} stroke="#4bc0c0" />
                <YAxis yAxisId="s" domain={[0, 4095]} tick={{ fontSize: 10 }} stroke="#f97316" orientation="right" />
                <Tooltip
                  contentStyle={{
                    background: "var(--surface)", border: "1px solid var(--border)",
                    borderRadius: 8, fontSize: 11,
                  }}
                  cursor={{ stroke: "var(--border2)" }}
                />
                <Line yAxisId="t" type="monotone" dataKey="Temp (°C)"   stroke="#4bc0c0" dot={false} strokeWidth={2} isAnimationActive={false} />
                <Line yAxisId="s" type="monotone" dataKey="Smoke (ADC)" stroke="#f97316" dot={false} strokeWidth={2} isAnimationActive={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>

        <div className="card">
          <div className="ctitle">Output State</div>
          <div className="leds">
            <LedRow color="g" on={lv < 2}    label="Green — safe & warning" />
            <LedRow color="y" on={lv === 1}  label="Yellow — warning only"  />
            <LedRow color="r" on={lv === 2}  label="Red — danger (blinks)"  />
          </div>
          <div className="hw-row">
            <HwChip label="Fan relay" value={lv === 2 && !data.sim ? "ON"      : "OFF"}    active={lv === 2 && !data.sim} />
            <HwChip label="Buzzer"    value={lv === 2              ? "Wailing" : "Silent"} active={lv === 2}              />
            <HwChip label="Mode"      value={data.sim              ? "SIM"     : "Real"}   active={data.sim}              />
          </div>
        </div>
      </div>

      {/* ── Sensor fusion + Controls ── */}
      <div className="row3">

        {/* Fusion */}
        <div className="card">
          <div className="ctitle">Sensor Fusion</div>
          <div className="fusion-list">
            {[
              { label: "Temperature > 45°C",  active: data.temperature > T_THRESH },
              { label: "Smoke > 500 ADC",     active: data.smoke > S_THRESH        },
              { label: "Flame detected",      active: data.flame                   },
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
              <div key={i} className={`pb p${i} ${data.pts >= i ? "on" : ""}`} />
            ))}
          </div>
          <div className="pts-note">{data.pts} / 3 — {NAMES[lv].toLowerCase()}</div>
        </div>

        {/* Overrides + Controls */}
        <div className="col-stack">
          <div className="card">
            <div className="ctitle">
              Sensor Overrides
              {!data.sim && (
                <span className="ctitle-note"> — enable sim mode first</span>
              )}
            </div>
            <div className="sliders" style={{ opacity: data.sim ? 1 : 0.4, pointerEvents: data.sim ? "auto" : "none" }}>
              <SliderGroup
                label="Temperature"
                display={`${simTemp.toFixed(1)}°C`}
                value={simTemp} min={20} max={80}
                onChange={(e) => setSimTemp(parseFloat(e.target.value))}
                mark="threshold: 45°C"
              />
              <SliderGroup
                label="Smoke / Gas"
                display={`${simSmoke} ADC`}
                value={simSmoke} min={0} max={4095}
                onChange={(e) => setSimSmoke(parseInt(e.target.value))}
                mark="threshold: 500"
              />
              <button
                className={`flame-btn ${simFlame ? "active" : ""}`}
                onClick={() => setSimFlame((v) => !v)}
              >
                <span>{simFlame ? "Flame detected — click to clear" : "No flame — click to trigger"}</span>
                <svg width="8" height="8" viewBox="0 0 24 24" fill="currentColor">
                  <circle cx="12" cy="12" r="6" />
                </svg>
              </button>
            </div>
          </div>

          <div className="card">
            <div className="ctitle">Controls</div>
            <div className="mode-btns">
              <button className={`mbtn ${data.sim ? "sim-on" : ""}`} onClick={toggleSim}>
                {data.sim ? "⏹ Disable simulation" : "▶ Enable simulation mode"}
              </button>
              <button className="mbtn rst" onClick={resetSystem}>↺ Reset system</button>
            </div>
            <div className="uptime-row">Uptime: {data.uptime}</div>
          </div>
        </div>
      </div>

      {/* ── Blynk IoT notifications ── */}
      <div className="card blynk-card">
        <div className="blynk-head">
          <div>
            <div className="blynk-title">
              <svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14">
                <path d="M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zM12 17c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z" />
              </svg>
              Blynk IoT — Push Notifications
            </div>
            <div className="blynk-sub">
              {data.blynk
                ? "Connected · notifications sent directly from ESP32"
                : data.wifi
                  ? "ESP32 not connected to Blynk — check auth token"
                  : "Waiting for WiFi connection…"}
              {!data.blynk && " · Server-side backup active when BLYNK_TOKEN is set"}
            </div>
          </div>
          <div className={`blynk-status ${data.blynk ? "on" : "off"}`}>
            <span className="blynk-dot" />
            {data.blynk ? "Live" : "Offline"}
          </div>
        </div>

        <div className="notif-list">
          {notifications.length === 0 ? (
            <div className="notif-empty">
              No alerts triggered yet. System is monitoring sensors…
            </div>
          ) : (
            notifications.map((n, i) => (
              <div key={i} className={`notif-row ${n.level === 2 ? "d" : "w"}`}>
                <span className="notif-ts">[{n.ts}]</span>
                <span className={`notif-label ${n.level === 2 ? "danger" : "warn"}`}>
                  {n.label}
                </span>
                <span className="notif-msg">→ {n.via}</span>
              </div>
            ))
          )}
        </div>

        {/* Virtual pin map for Blynk dashboard setup */}
        <div className="vpin-row">
          <span className="vpin-title">Virtual pins:</span>
          {[
            { pin: "V0", label: "Temp °C" },
            { pin: "V1", label: "Smoke ADC" },
            { pin: "V2", label: "Flame 0/1" },
            { pin: "V3", label: "Alarm level" },
          ].map(({ pin, label }) => (
            <span key={pin} className="vpin-chip">{pin} · {label}</span>
          ))}
          <span className="vpin-chip event">Event: fire_alert</span>
          <span className="vpin-chip event">Event: smoke_warning</span>
        </div>
      </div>

      {/* ── Serial log ── */}
      <div className="card log-panel">
        <div className="log-head">
          <div className="ctitle" style={{ marginBottom: 0 }}>
            ESP32 Serial Monitor · 115200 baud
          </div>
          <button className="clear-btn" onClick={() => setLogs([])}>Clear</button>
        </div>
        <div className="log-scroll" ref={logRef}>
          {logs.length === 0 ? (
            <span style={{ color: "var(--text3)", fontStyle: "italic" }}>No events yet…</span>
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

// ── Sub-components ─────────────────────────────────────────────────────────────

function Chip({ ok, label, variant, icon }) {
  const cls = variant === "blynk"
    ? (ok ? "blynk" : "off")
    : (ok ? "ok" : "off");
  return (
    <div className={`conn-chip ${cls}`}>
      {icon === "wifi" ? (
        <svg viewBox="0 0 24 24" fill="currentColor" width="9" height="9" style={{ flexShrink: 0 }}>
          <path d="M1 9l2 2c2.88-2.88 6.79-4.08 10.53-3.62l1.19-1.94C10.12 4.67 4.83 6.1 1 9zm3.5 3.5l2 2C8.11 13.23 10 12 12 12s3.89 1.23 5.5 2.5l2-2C17.56 10.74 14.93 9.5 12 9.5s-5.56 1.24-7.5 3zm5 5l2.5 2.5 2.5-2.5C14.73 16.37 13.42 16 12 16s-2.73.37-3.5 1z" />
        </svg>
      ) : (
        <span className={`conn-dot ${cls}`} />
      )}
      {label}
    </div>
  );
}

function MetCard({ label, value, unit, sub, state }) {
  return (
    <div className={`met met-${state}`}>
      <div className="met-label">{label}</div>
      <div className={`met-val val-${state}`}>
        {value}
        {unit && <span className="met-unit"> {unit}</span>}
      </div>
      <div className="met-sub">{sub}</div>
    </div>
  );
}

function LegendItem({ color, label }) {
  return (
    <div className="leg-item">
      <div className="leg-sq" style={{ background: color }} />
      <span>{label}</span>
    </div>
  );
}

function LedRow({ color, on, label }) {
  return (
    <div className="led-row">
      <div className={`led ${color} ${on ? "on" : ""}`} />
      <span>{label}</span>
    </div>
  );
}

function HwChip({ label, value, active }) {
  return (
    <div className="hw-chip">
      <div className="hw-chip-label">{label}</div>
      <div className={`hw-chip-val ${active ? "active" : ""}`}>{value}</div>
    </div>
  );
}

function SliderGroup({ label, value, display, min, max, onChange, mark }) {
  return (
    <div className="sl-group">
      <div className="sl-header">
        <span>{label}</span>
        <span className="sl-val">{display}</span>
      </div>
      <input type="range" min={min} max={max} value={value} onChange={onChange} />
      <div className="sl-marks">{mark}</div>
    </div>
  );
}
