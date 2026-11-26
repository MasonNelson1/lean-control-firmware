// LEAN braking story: loads the sections listed in story.json, runs each
// section's inline <script>, and gives them a small chart helper (Story.line).
(() => {
const NS = "http://www.w3.org/2000/svg";
const el = (tag, attrs = {}, parent) => {
  const e = document.createElementNS(NS, tag);
  for (const a in attrs) e.setAttribute(a, attrs[a]);
  if (parent) parent.appendChild(e);
  return e;
};

// Shared numbers. Change them here and every chart follows.
const C = {
  VB: 53.5,            // battery volts mid-charge (measured 53.4–53.6 V on 11/22)
  V_PER_MPS: 6.6,      // motor voltage per m/s of chair speed (fit of 2,329 VESC samples, 11/22)
  N_PER_A: 5.1,        // push per motor amp, per wheel (VESC motor model: 1.5 × 4 pole pairs × 0.0177 Wb × 14.55 gear ÷ 0.305 m)
  MASS: 120,           // chair + rider, kg (estimate)
  MOTOR_A_PER_BATT_A: 11.27, // motor amps per battery amp at 1 m/s (from the logs: iin ≈ 0.72 × duty × motor amps)
  RPM_TO_MPS: 0.61 * Math.PI / 60, // 24" wheel
  TRIP_L: 49.03, TRIP_R: 52.22,      // l_abs_current_max, left / right (VESC config 11/22)
};
// Motor current the battery cap allows at a speed (per motor). Above the motor limit, the limit wins.
C.allowed = (v, battCapA) => battCapA * C.MOTOR_A_PER_BATT_A / Math.max(v, 1e-3);

function line(host, o) {
  const W = Math.round(Math.max(300, Math.min(900, host.clientWidth || 600)));
  const H = o.h || 240, ml = 46, mr = o.mr ?? 14, mt = 14, mb = 34;
  const xMin = o.xMin ?? 0, yMin = o.yMin ?? 0;
  const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, role: "img", "aria-label": o.aria || "chart" }, host);
  const X = v => ml + (v - xMin) / (o.xMax - xMin) * (W - ml - mr);
  const Y = v => mt + (1 - (v - yMin) / (o.yMax - yMin)) * (H - mt - mb);
  const clampY = v => Math.min(Math.max(v, yMin), o.yMax);
  for (const b of o.bands || []) el("rect", { x: X(b.x0), y: mt, width: X(b.x1) - X(b.x0), height: H - mt - mb, fill: b.fill }, svg);
  for (const b of o.hbands || []) el("rect", { x: ml, y: Y(b.y1), width: W - ml - mr, height: Y(b.y0) - Y(b.y1), fill: b.fill }, svg);
  const ax = el("g", { class: "axis" }, svg);
  for (const t of o.yTicks) {
    el("line", { x1: ml, x2: W - mr, y1: Y(t), y2: Y(t), stroke: "var(--grid)" }, ax);
    el("text", { x: ml - 6, y: Y(t) + 4, "text-anchor": "end" }, ax).textContent = (o.yFmt || (v => v))(t);
  }
  for (const t of o.xTicks) el("text", { x: X(t), y: H - mb + 16, "text-anchor": "middle" }, ax).textContent = (o.xFmt || (v => v))(t);
  const y0 = yMin <= 0 && o.yMax >= 0 ? 0 : yMin;
  el("line", { x1: ml, x2: W - mr, y1: Y(y0), y2: Y(y0), stroke: "var(--ink-3)" }, ax);
  if (o.xLabel) el("text", { x: W - mr, y: H - 3, "text-anchor": "end" }, ax).textContent = o.xLabel;
  if (o.yLabel) el("text", { x: 4, y: mt - 3 }, ax).textContent = o.yLabel;
  for (const r of o.refs || []) {
    el("line", { x1: ml, x2: W - mr, y1: Y(r.y), y2: Y(r.y), stroke: r.color, "stroke-width": 1.5, "stroke-dasharray": r.dash || "5 4" }, svg);
    if (r.label) el("text", { x: r.right ? W - mr - 4 : ml + 4, y: Y(r.y) + (r.below ? 13 : -5), "text-anchor": r.right ? "end" : "start", class: "lbl", style: `fill:${r.color}` }, svg).textContent = r.label;
  }
  for (const m of o.marks || []) {
    el("line", { x1: X(m.x), x2: X(m.x), y1: mt, y2: H - mb, stroke: m.color, "stroke-width": 1.5, "stroke-dasharray": m.dash || "3 3" }, svg);
    if (m.label) el("text", { x: X(m.x) + (m.left ? -4 : 4), y: mt + 10 + (m.dy || 0), "text-anchor": m.left ? "end" : "start", class: "lbl", style: `fill:${m.color}` }, svg).textContent = m.label;
  }
  const paths = (o.series || []).map(s => el("path", { fill: "none", stroke: s.color, "stroke-width": s.width || 2, "stroke-linejoin": "round", "stroke-linecap": "round", "stroke-dasharray": s.dash || "none", opacity: s.opacity ?? 1 }, svg));
  const dotsLayer = el("g", {}, svg);
  const cursor = el("line", { y1: mt, y2: H - mb, stroke: "var(--ink-3)", "stroke-dasharray": "3 3", opacity: 0 }, svg);
  const cdots = (o.series || []).map(s => el("circle", { r: 4, fill: s.color, stroke: "var(--panel)", "stroke-width": 2, opacity: 0 }, svg));
  function draw(data) { // data: array per series of [x,y] points (null y breaks the line)
    (o.series || []).forEach((s, i) => {
      let d = "", pen = false;
      for (const p of data[i] || []) {
        if (p[1] == null || p[1] > o.yMax || p[1] < yMin) { pen = false; continue; }
        d += (pen ? "L" : "M") + X(p[0]).toFixed(1) + " " + Y(clampY(p[1])).toFixed(1); pen = true;
      }
      paths[i].setAttribute("d", d);
    });
  }
  function dots(list) { // [{x,y,color,r,label}]
    dotsLayer.innerHTML = "";
    for (const d of list) {
      el("circle", { cx: X(d.x), cy: Y(clampY(d.y)), r: d.r || 5, fill: d.color, stroke: "var(--panel)", "stroke-width": 2 }, dotsLayer);
      if (d.label) el("text", { x: X(d.x) + (d.left ? -9 : 9), y: Y(clampY(d.y)) + 4 + (d.dy || 0), "text-anchor": d.left ? "end" : "start", class: "lbl", style: `fill:${d.color};font-weight:600` }, dotsLayer).textContent = d.label;
    }
  }
  function setCursor(x, ys) {
    cursor.setAttribute("x1", X(x)); cursor.setAttribute("x2", X(x)); cursor.setAttribute("opacity", 1);
    cdots.forEach((d, i) => { if (ys[i] == null) { d.setAttribute("opacity", 0); return; } d.setAttribute("cx", X(x)); d.setAttribute("cy", Y(clampY(ys[i]))); d.setAttribute("opacity", 1); });
  }
  function hideCursor() { cursor.setAttribute("opacity", 0); cdots.forEach(d => d.setAttribute("opacity", 0)); }
  if (o.tip) {
    const tip = document.createElement("div"); tip.className = "tip"; tip.hidden = true; host.appendChild(tip);
    const hit = el("rect", { x: ml, y: mt, width: W - ml - mr, height: H - mt - mb, fill: "transparent" }, svg);
    hit.addEventListener("pointermove", e => {
      const r = svg.getBoundingClientRect();
      const x = xMin + Math.max(0, Math.min(1, ((e.clientX - r.left) / r.width * W - ml) / (W - ml - mr))) * (o.xMax - xMin);
      const info = o.tip(x); if (!info) return;
      setCursor(info.x ?? x, info.ys || []); tip.innerHTML = info.html; tip.hidden = false;
      // the host may be zoomed (a pinned figure), so convert screen px to its own px
      const hr = host.getBoundingClientRect(), k = hr.width / (host.offsetWidth || hr.width);
      const px = (e.clientX - hr.left) / k, py = (e.clientY - hr.top) / k;
      tip.style.left = (px > host.offsetWidth - 200 ? px - 200 : px + 14) + "px"; tip.style.top = (py - 10) + "px";
    });
    hit.addEventListener("pointerleave", () => { tip.hidden = true; hideCursor(); o.onLeave && o.onLeave(); });
  }
  return { svg, X, Y, draw, dots, setCursor, hideCursor, el: (t, a) => el(t, a, svg) };
}

function legend(host, items) {
  const d = document.createElement("div"); d.className = "legend";
  d.innerHTML = items.map(i => `<span style="color:${i.color}"><i class="${i.dash ? "dash" : ""}${i.dot ? " dot" : ""}" style="background:${i.color}"></i><span style="color:var(--ink-2)">${i.label}</span></span>`).join("");
  host.appendChild(d); return d;
}

const cache = {};
const data = name => cache[name] || (cache[name] = fetch("data/" + name).then(r => r.json()));
window.Story = { C, line, legend, data, el };

// ---- load sections ----
async function boot() {
  const main = document.querySelector("main"), nav = document.querySelector("nav.toc .list");
  const list = await fetch("story.json").then(r => r.json());
  let n = 0;
  for (const item of list.sections) {
    if (item.hidden) continue;
    n++;
    const id = item.file.replace(/\.html$/, "").replace(/^\d+[a-z]?-/, "");
    const only = new URLSearchParams(location.search).get("only");
    if (only && !only.split(",").includes(id)) continue;
    const sec = document.createElement("section"); sec.className = "s"; sec.id = id;
    try {
      sec.innerHTML = await fetch("sections/" + item.file).then(r => { if (!r.ok) throw new Error(r.status); return r.text(); });
    } catch (e) { sec.innerHTML = `<p class="err">Couldn't load sections/${item.file} (${e.message}).</p>`; }
    main.appendChild(sec);
    sec.dataset.title = item.nav;
    const a = document.createElement("a"); a.href = "#" + id; a.innerHTML = `<span class="n">${String(n).padStart(2, "0")}</span><span>${item.nav}</span>`; nav.appendChild(a);
    for (const old of sec.querySelectorAll("script")) {
      const s = document.createElement("script"); s.textContent = `(()=>{const root=document.getElementById(${JSON.stringify(id)});\n${old.textContent}\n})();`;
      old.replaceWith(s);
    }
  }
  const links = [...nav.querySelectorAll("a")];
  const io = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) links.forEach(l => l.classList.toggle("on", l.getAttribute("href") === "#" + e.target.id)); }), { rootMargin: "-30% 0px -60% 0px" });
  main.querySelectorAll("section.s").forEach(s => io.observe(s));
  setupPins(main);
  if (location.hash && !location.hash.startsWith("#note-")) document.getElementById(location.hash.slice(1))?.scrollIntoView();
  // keep the scroll position across live reloads
  try { const y = sessionStorage.getItem("story-y"); if (y && !location.hash) window.scrollTo(0, +y); } catch (e) {}
}
addEventListener("beforeunload", () => { try { sessionStorage.setItem("story-y", scrollY); } catch (e) {} });

// ---- theme override (?theme=dark|light), mainly for checking both themes ----
const th = new URLSearchParams(location.search).get("theme");
if (th) document.documentElement.setAttribute("data-theme", th);

// ---- collapsible section list ----
const store = {
  get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch (e) {} },
  sget(k) { try { return sessionStorage.getItem(k); } catch (e) { return null; } },
  sset(k, v) { try { sessionStorage.setItem(k, v); } catch (e) {} },
};
function setNav(hidden) { document.body.classList.toggle("nav-hidden", hidden); store.set("story-nav-hidden", hidden ? "1" : "0"); }
document.getElementById("nav-hide").addEventListener("click", () => setNav(true));
document.getElementById("nav-show").addEventListener("click", () => setNav(false));
setNav(store.get("story-nav-hidden") === "1");

// ---- pin a figure to a floating, resizable, movable dock ----
const dock = document.getElementById("dock"), dockBody = dock.querySelector(".dock-body"), dockTitle = dock.querySelector(".dock-title");
let pinned = null; // { panel, placeholder }
function unpin() {
  if (!pinned) return;
  Object.assign(pinned.panel.style, { width: "", transform: "", transformOrigin: "", marginLeft: "" });
  pinned.placeholder.replaceWith(pinned.panel);
  pinned.panel.querySelector(".pin-btn").textContent = "Pin";
  pinned = null; dock.hidden = true; store.sset("story-pin", "");
}
function pin(panel) {
  if (pinned && pinned.panel === panel) { unpin(); return; }
  unpin();
  const natW = panel.offsetWidth;
  const ph = document.createElement("div"); ph.className = "pin-placeholder";
  ph.style.height = panel.offsetHeight + "px";
  ph.innerHTML = `<span>Pinned to the floating window.</span> <button class="btn ghost">Unpin</button>`;
  ph.querySelector("button").addEventListener("click", unpin);
  panel.replaceWith(ph); dockBody.appendChild(panel);
  panel.querySelector(".pin-btn").textContent = "Unpin";
  const t = panel.querySelector(".ttl, h3"); dockTitle.textContent = t ? t.textContent : "Pinned figure";
  pinned = { panel, placeholder: ph, natW }; dock.hidden = false; store.sset("story-pin", panel.id);
  fitDock();
}
// Zoom the pinned figure to fit the window: it keeps its page layout, so resizing the
// window scales the whole figure instead of adding scroll bars.
function fitDock() {
  if (!pinned || dock.hidden) return;
  const p = pinned.panel, W0 = pinned.natW;
  Object.assign(p.style, { width: W0 + "px", transform: "", transformOrigin: "0 0", marginLeft: "0" });
  const cs = getComputedStyle(dockBody);
  const H0 = p.offsetHeight, aw = dockBody.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight), ah = dockBody.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
  const s = Math.min(aw / W0, ah / H0, 1.6);
  p.style.transform = `scale(${s})`;
  p.style.marginLeft = Math.max(0, (aw - W0 * s) / 2) + "px";
}
function setupPins(main) {
  const counts = {};
  main.querySelectorAll("section.s .panel").forEach(p => {
    if (!p.querySelector(".chart, svg.scene, canvas")) return;
    const sid = p.closest("section").id; counts[sid] = (counts[sid] || 0) + 1;
    p.id = p.id || `fig-${sid}-${counts[sid]}`;
    p.classList.add("pinnable"); p.dataset.section = sid;
    const b = document.createElement("button"); b.className = "pin-btn"; b.type = "button"; b.textContent = "Pin";
    b.title = "Keep this figure in a floating window while you scroll"; b.addEventListener("click", () => pin(p));
    p.prepend(b);
  });
  const saved = store.sget("story-pin"); if (saved && document.getElementById(saved)) pin(document.getElementById(saved));
}
dock.querySelector(".dock-close").addEventListener("click", unpin);
// geometry: drag by the header, resize from the corner (CSS resize), remembered for the session
function saveGeo() { store.sset("story-dock", JSON.stringify({ l: dock.offsetLeft, t: dock.offsetTop, w: dock.offsetWidth, h: dock.offsetHeight })); }
try { const g = JSON.parse(store.sget("story-dock") || "null"); if (g) Object.assign(dock.style, { left: g.l + "px", top: g.t + "px", width: g.w + "px", height: g.h + "px", right: "auto" }); } catch (e) {}
new ResizeObserver(() => { if (!dock.hidden) { saveGeo(); fitDock(); } }).observe(dock);
addEventListener("resize", fitDock);
dock.querySelector(".dock-head").addEventListener("pointerdown", e => {
  if (e.target.closest("button")) return;
  const sx = e.clientX, sy = e.clientY, l0 = dock.offsetLeft, t0 = dock.offsetTop;
  const move = ev => {
    const l = Math.min(Math.max(0, l0 + ev.clientX - sx), innerWidth - 80), t = Math.min(Math.max(0, t0 + ev.clientY - sy), innerHeight - 40);
    Object.assign(dock.style, { left: l + "px", top: t + "px", right: "auto" });
  };
  const up = () => { removeEventListener("pointermove", move); removeEventListener("pointerup", up); saveGeo(); };
  addEventListener("pointermove", move); addEventListener("pointerup", up);
});
addEventListener("keydown", e => { if (e.key === "Escape" && pinned) unpin(); });
if (window.EventSource && !/[?&]static/.test(location.search)) { const es = new EventSource("/__reload"); es.onmessage = () => location.reload(); }
Story.ready = boot();
})();
