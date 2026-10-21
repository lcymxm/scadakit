#include "scada/dashboard.hpp"

namespace scada {

// The whole operator screen ships inside the binary: no build step, no CDN, no
// framework. It polls the JSON API and repaints.
std::string dashboard_html() {
    return R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScadaKit 中控系统</title>
<style>
  :root {
    --bg: #0d1117; --panel: #161b22; --line: #263041; --text: #e6edf3;
    --muted: #8b96a5; --ok: #3fb950; --warn: #d29922; --bad: #f85149;
    --accent: #58a6ff;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text);
         font-family: "Segoe UI", "Microsoft YaHei", system-ui, sans-serif; }
  header { display: flex; align-items: center; gap: 18px; padding: 14px 22px;
           background: var(--panel); border-bottom: 1px solid var(--line); }
  header h1 { font-size: 17px; margin: 0; font-weight: 600; letter-spacing: .5px; }
  .spacer { flex: 1; }
  .pill { font-size: 12px; padding: 4px 10px; border-radius: 999px;
          border: 1px solid var(--line); color: var(--muted); }
  .pill.online { color: var(--ok); border-color: rgba(63,185,80,.45); }
  .pill.offline { color: var(--bad); border-color: rgba(248,81,73,.45); }
  .pill.alarm { color: #fff; background: var(--bad); border-color: var(--bad); }
  .clock { font-variant-numeric: tabular-nums; color: var(--muted); font-size: 13px; }
  main { padding: 18px 22px 40px; }
  .alarms { background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
            padding: 12px 16px; margin-bottom: 18px; }
  .alarms h2 { font-size: 13px; margin: 0 0 10px; color: var(--muted); font-weight: 600;
               text-transform: uppercase; letter-spacing: .08em; }
  .alarm-row { display: flex; align-items: center; gap: 10px; padding: 6px 0;
               border-top: 1px solid var(--line); font-size: 13px; }
  .alarm-row:first-of-type { border-top: none; }
  .badge { font-size: 11px; font-weight: 700; padding: 2px 7px; border-radius: 4px; }
  .badge.HH, .badge.LL { background: var(--bad); color: #fff; }
  .badge.HI, .badge.LO { background: var(--warn); color: #1b1b1b; }
  .badge.rtn { background: #30363d; color: var(--muted); }
  .empty { color: var(--muted); font-size: 13px; }
  button { background: #21262d; color: var(--text); border: 1px solid var(--line);
           border-radius: 6px; padding: 4px 10px; font-size: 12px; cursor: pointer; }
  button:hover { border-color: var(--accent); color: var(--accent); }
  .grid { display: grid; gap: 14px; grid-template-columns: repeat(auto-fill, minmax(215px, 1fr)); }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
          padding: 14px 16px; position: relative; overflow: hidden; }
  .card.alarm { border-color: var(--bad); box-shadow: inset 3px 0 0 var(--bad); }
  .card.stale { opacity: .55; }
  .card .name { font-size: 12px; color: var(--muted); letter-spacing: .04em; }
  .card .desc { font-size: 13px; margin-top: 2px; }
  .card .value { font-size: 30px; font-variant-numeric: tabular-nums; margin-top: 8px;
                 line-height: 1.1; }
  .card .unit { font-size: 13px; color: var(--muted); margin-left: 4px; }
  .card .foot { display: flex; align-items: center; gap: 8px; margin-top: 10px;
                font-size: 11px; color: var(--muted); }
  .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; }
  .limits { display: flex; gap: 6px; margin-top: 8px; font-size: 10px; }
  .limits span { color: #4b5563; }
  .limits span.hit { color: var(--bad); font-weight: 700; }
  .trend { background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
           padding: 12px 16px; margin-top: 18px; }
  .trend h2 { font-size: 13px; margin: 0 0 8px; color: var(--muted); font-weight: 600;
              text-transform: uppercase; letter-spacing: .08em; }
  canvas { width: 100%; height: 190px; display: block; }
  .legend { display: flex; gap: 16px; font-size: 11px; color: var(--muted); margin-top: 8px; }
  .legend i { width: 10px; height: 3px; display: inline-block; margin-right: 5px;
              vertical-align: middle; }
</style>
</head>
<body>
<header>
  <h1>ScadaKit · 变电站中控系统</h1>
  <span class="pill" id="link">连接中…</span>
  <span class="spacer"></span>
  <span class="pill alarm" id="unacked" style="display:none">0 条未确认</span>
  <span class="clock" id="clock">--:--:--</span>
</header>
<main>
  <section class="alarms">
    <h2>实时报警</h2>
    <div id="alarm-list"><span class="empty">无报警</span></div>
  </section>

  <section class="grid" id="cards"></section>

  <section class="trend">
    <h2>趋势曲线</h2>
    <canvas id="chart" width="1200" height="220"></canvas>
    <div class="legend" id="legend"></div>
  </section>
</main>
<script>
const QUALITY_COLOR = { "good": "#3fb950", "uncertain": "#d29922", "bad": "#f85149",
                        "stale": "#8b96a5", "comm-failure": "#f85149" };
const LEVEL_TEXT = { "HH": "高高", "HI": "高", "LO": "低", "LL": "低低" };

let cards = {};
let trendNames = [];

function fmt(value) {
  if (value === null || value === undefined) return "--";
  return Math.abs(value) >= 100 ? value.toFixed(0) : value.toFixed(2);
}

async function poll() {
  try {
    const snapshot = await (await fetch("/api/snapshot")).json();
    render(snapshot);
  } catch (error) {
    document.getElementById("link").className = "pill offline";
    document.getElementById("link").textContent = "网关断开";
  }
}

function render(snapshot) {
  const link = document.getElementById("link");
  link.className = "pill " + (snapshot.online ? "online" : "offline");
  link.textContent = snapshot.online ? "通信正常" : "通信中断";
  document.getElementById("clock").textContent = snapshot.time_text.slice(11);

  const badge = document.getElementById("unacked");
  if (snapshot.unacked > 0) {
    badge.style.display = "";
    badge.textContent = snapshot.unacked + " 条未确认";
  } else {
    badge.style.display = "none";
  }

  const host = document.getElementById("cards");
  if (host.children.length !== snapshot.tags.length) {
    host.innerHTML = "";
    for (const tag of snapshot.tags) {
      const card = document.createElement("div");
      card.className = "card";
      card.innerHTML = `
        <div class="name">${tag.name}</div>
        <div class="desc">${tag.description || ""}</div>
        <div class="value"><span class="num">--</span><span class="unit">${tag.unit}</span></div>
        <div class="limits">
          <span data-l="3">LL</span><span data-l="2">LO</span>
          <span data-l="1">HI</span><span data-l="0">HH</span>
        </div>
        <div class="foot"><span class="dot"></span><span class="q"></span></div>`;
      host.appendChild(card);
      cards[tag.name] = card;
    }
  }

  for (const tag of snapshot.tags) {
    const card = cards[tag.name];
    if (!card) continue;
    card.querySelector(".num").textContent = fmt(tag.value);
    card.querySelector(".q").textContent = tag.quality;
    const dot = card.querySelector(".dot");
    dot.style.background = QUALITY_COLOR[tag.quality] || "#8b96a5";
    card.className = "card" + (tag.alarm === "unacked-active" || tag.alarm === "acked-active"
                              ? " alarm" : "")
                             + (tag.quality === "good" ? "" : " stale");
    const flags = card.querySelectorAll(".limits span");
    for (const flag of flags) {
      const index = Number(flag.dataset.l);
      // limits are [LL, LO, HI, HH] in the API
      flag.classList.toggle("hit", tag.limits[index] === 1);
    }
  }
}

async function pollAlarms() {
  try {
    const data = await (await fetch("/api/alarms")).json();
    const list = document.getElementById("alarm-list");
    if (data.standing.length === 0) {
      list.innerHTML = '<span class="empty">无报警</span>';
      return;
    }
    list.innerHTML = "";
    for (const alarm of data.standing) {
      const row = document.createElement("div");
      row.className = "alarm-row";
      const running = alarm.state === "unacked-active" || alarm.state === "acked-active";
      row.innerHTML = `<span class="badge ${alarm.level}">${LEVEL_TEXT[alarm.level] || alarm.level}</span>
                       <strong>${alarm.tag}</strong>
                       <span class="empty">${running ? "报警中" : "已恢复"} · ${alarm.state}</span>
                       <span class="spacer"></span>
                       <button data-tag="${alarm.tag}">确认</button>`;
      row.querySelector("button").onclick = () => ack(alarm.tag);
      list.appendChild(row);
    }
  } catch (error) { /* the next poll will report it */ }
}

async function ack(tag) {
  await fetch("/api/ack?tag=" + encodeURIComponent(tag), { method: "POST" });
  pollAlarms();
  poll();
}

const COLORS = ["#58a6ff", "#3fb950", "#d29922", "#f85149", "#a371f7"];
let seriesCache = [];

async function pollTrends() {
  if (trendNames.length === 0) {
    const snapshot = await (await fetch("/api/snapshot")).json();
    // The first few numeric tags make a readable trend panel.
    trendNames = snapshot.tags.filter(t => t.unit !== "" && t.limits).slice(0, 4).map(t => t.name);
    document.getElementById("legend").innerHTML = trendNames.map((name, i) =>
      `<span><i style="background:${COLORS[i % COLORS.length]}"></i>${name}</span>`).join("");
  }
  seriesCache = [];
  for (const name of trendNames) {
    try {
      seriesCache.push(await (await fetch("/api/history?tag=" + encodeURIComponent(name))).json());
    } catch (error) { seriesCache.push([]); }
  }
  drawChart();
}

function drawChart() {
  const canvas = document.getElementById("chart");
  const ctx = canvas.getContext("2d");
  const width = canvas.width, height = canvas.height;
  ctx.clearRect(0, 0, width, height);

  let longest = 0;
  for (const series of seriesCache) longest = Math.max(longest, series.length);
  if (longest < 2) {
    ctx.fillStyle = "#8b96a5";
    ctx.font = "14px system-ui";
    ctx.fillText("采样中…", 16, height / 2);
    return;
  }

  const pad = 34;
  let low = Infinity, high = -Infinity;
  for (const series of seriesCache) {
    for (const point of series) { low = Math.min(low, point[1]); high = Math.max(high, point[1]); }
  }
  if (!isFinite(low)) return;
  const span = Math.max(high - low, 1e-6);

  ctx.strokeStyle = "#263041";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; ++i) {
    const y = pad + (height - 2 * pad) * i / 4;
    ctx.beginPath(); ctx.moveTo(pad, y); ctx.lineTo(width - 8, y); ctx.stroke();
    ctx.fillStyle = "#8b96a5";
    ctx.font = "11px ui-monospace, monospace";
    ctx.fillText((high - span * i / 4).toFixed(1), 2, y + 4);
  }

  seriesCache.forEach((series, index) => {
    if (series.length < 2) return;
    ctx.strokeStyle = COLORS[index % COLORS.length];
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    series.forEach((point, i) => {
      const x = pad + (width - pad - 8) * i / (longest - 1);
      const y = pad + (height - 2 * pad) * (1 - (point[1] - low) / span);
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
  });
}

poll();
pollAlarms();
pollTrends().catch(() => {});
setInterval(poll, 800);
setInterval(pollAlarms, 1500);
setInterval(pollTrends, 2000);
</script>
</body>
</html>
)HTML";
}

}  // namespace scada
