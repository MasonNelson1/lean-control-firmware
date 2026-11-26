// Notes panel: a small floating notebook. Select text on the page and press
// "+ Note" to save the quote as a link back to that spot. Notes are saved to
// notes.json / notes.md through serve.py (so Claude can read them), with a
// localStorage copy as a fallback when the page is opened without the server.
(() => {
const LS = "story-notes", LS_OPEN = "story-notes-open";
const ls = { get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }, set(k, v) { try { localStorage.setItem(k, v); } catch (e) {} } };
const esc = s => String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const uid = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
let notes = [], server = true, saveTimer = null;

// ---------- UI ----------
const wrap = document.createElement("div");
wrap.className = "notes";
wrap.innerHTML = `
  <button class="notes-pill" type="button" aria-expanded="false" title="Notes (select text on the page to add a quote)">✎ Notes <span class="notes-count">0</span></button>
  <div class="notes-panel" hidden>
    <div class="notes-head">
      <span class="notes-title">Notes</span>
      <span class="notes-status" aria-live="polite"></span>
      <button class="icon-btn notes-new" type="button" title="New note">+</button>
      <button class="notes-copy" type="button" title="Copy all notes as Markdown">Copy</button>
      <button class="icon-btn notes-min" type="button" title="Minimize">–</button>
    </div>
    <div class="notes-list"></div>
    <div class="notes-empty">Select any text on the page, then press <b>+ Note</b> to save it here as a link. Or press <b>+</b> for a blank note.</div>
  </div>`;
document.body.appendChild(wrap);
const pill = wrap.querySelector(".notes-pill"), panel = wrap.querySelector(".notes-panel"), list = wrap.querySelector(".notes-list");
const status = wrap.querySelector(".notes-status"), empty = wrap.querySelector(".notes-empty");
const addBtn = document.createElement("button");
addBtn.className = "notes-add"; addBtn.type = "button"; addBtn.textContent = "+ Note"; addBtn.hidden = true;
document.body.appendChild(addBtn);

function setOpen(open) {
  panel.hidden = !open; pill.hidden = open; pill.setAttribute("aria-expanded", open);
  ls.set(LS_OPEN, open ? "1" : "0");
  clampPos();
}

// ---------- move: drag the header (or the pill); anchored to the bottom-right corner ----------
const LS_POS = "story-notes-pos";
let dragged = false;
function setPos(right, bottom) { wrap.style.right = right + "px"; wrap.style.bottom = bottom + "px"; }
function clampPos() {
  const r = wrap.getBoundingClientRect();
  let right = innerWidth - r.right, bottom = innerHeight - r.bottom;
  right = Math.min(Math.max(4, right), innerWidth - r.width - 4);
  bottom = Math.min(Math.max(4, bottom), innerHeight - r.height - 4);
  setPos(right, bottom);
}
function startDrag(e) {
  if (e.button !== 0 || e.target.closest(".notes-head button")) return;
  const r = wrap.getBoundingClientRect(), sx = e.clientX, sy = e.clientY;
  const r0 = innerWidth - r.right, b0 = innerHeight - r.bottom;
  dragged = false;
  const move = ev => {
    const dx = ev.clientX - sx, dy = ev.clientY - sy;
    if (!dragged && Math.hypot(dx, dy) < 4) return;
    dragged = true; setPos(r0 - dx, b0 - dy); clampPos();
  };
  const up = () => {
    removeEventListener("pointermove", move); removeEventListener("pointerup", up);
    if (dragged) { ls.set(LS_POS, JSON.stringify({ right: parseFloat(wrap.style.right), bottom: parseFloat(wrap.style.bottom) })); setTimeout(() => (dragged = false), 0); }
  };
  addEventListener("pointermove", move); addEventListener("pointerup", up);
}
wrap.querySelector(".notes-head").addEventListener("pointerdown", startDrag);
pill.addEventListener("pointerdown", startDrag);
try { const q = JSON.parse(ls.get(LS_POS) || "null"); if (q) setPos(q.right, q.bottom); } catch (e) {}
addEventListener("resize", clampPos);
pill.addEventListener("click", () => { if (dragged) return; setOpen(true); });
wrap.querySelector(".notes-min").addEventListener("click", () => setOpen(false));
wrap.querySelector(".notes-new").addEventListener("click", () => { const n = { id: uid(), created: new Date().toISOString(), text: "" }; notes.push(n); save(); render(); focusNote(n.id); });
wrap.querySelector(".notes-copy").addEventListener("click", async () => {
  const md = notes.map(n => (n.quote ? `> ${n.quote}\n> (${n.sectionTitle || n.section})\n\n` : "") + (n.text || "")).join("\n\n---\n\n");
  try { await navigator.clipboard.writeText(md); flash("Copied"); } catch (e) { flash("Copy failed"); }
});

function flash(msg) { status.textContent = msg; clearTimeout(flash.t); flash.t = setTimeout(() => (status.textContent = ""), 1500); }

function render() {
  wrap.querySelector(".notes-count").textContent = notes.length;
  empty.hidden = notes.length > 0;
  list.innerHTML = notes.map(n => `
    <div class="note" data-id="${n.id}">
      ${n.quote ? `<a class="note-quote" href="#note-${n.id}" title="Jump to this spot"><span class="q">“${esc(n.quote.length > 220 ? n.quote.slice(0, 220) + "…" : n.quote)}”</span><span class="where">↗ ${esc(n.sectionTitle || n.section || "page")}</span></a>` : ""}
      <textarea class="note-text" rows="${n.quote ? 2 : 3}" placeholder="${n.quote ? "Add a thought…" : "Note…"}">${esc(n.text || "")}</textarea>
      <div class="note-foot"><span>${new Date(n.created).toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" })}</span><button class="note-del" type="button" title="Delete note">Delete</button></div>
    </div>`).join("");
}
function focusNote(id) { setOpen(true); const t = list.querySelector(`.note[data-id="${id}"] .note-text`); if (t) { t.focus(); t.scrollIntoView({ block: "nearest" }); } }

list.addEventListener("input", e => {
  if (!e.target.classList.contains("note-text")) return;
  const n = notes.find(x => x.id === e.target.closest(".note").dataset.id); if (!n) return;
  n.text = e.target.value; save(true);
});
list.addEventListener("click", e => {
  const del = e.target.closest(".note-del");
  if (del) {
    const id = del.closest(".note").dataset.id;
    if (del.dataset.armed) { notes = notes.filter(x => x.id !== id); save(); render(); }
    else { del.dataset.armed = "1"; del.textContent = "Delete?"; setTimeout(() => { if (del.isConnected) { delete del.dataset.armed; del.textContent = "Delete"; } }, 2500); }
    return;
  }
  const q = e.target.closest(".note-quote");
  if (q) { e.preventDefault(); jump(q.closest(".note").dataset.id); }
});

// ---------- persistence ----------
async function load() {
  try { const r = await fetch("/__notes", { cache: "no-store" }); if (!r.ok) throw 0; notes = await r.json(); }
  catch (e) { server = false; try { notes = JSON.parse(ls.get(LS) || "[]"); } catch (e2) { notes = []; } }
  render();
}
function save(debounced) {
  ls.set(LS, JSON.stringify(notes));
  clearTimeout(saveTimer);
  saveTimer = setTimeout(async () => {
    if (!server) { flash("Saved in this browser"); return; }
    try { const r = await fetch("/__notes", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(notes) }); if (!r.ok) throw 0; flash("Saved"); }
    catch (e) { flash("Not saved to disk"); }
  }, debounced ? 600 : 0);
}

// ---------- text anchors ----------
// Map a container's visible text (whitespace collapsed) back to text nodes.
function textMap(root) {
  const chars = [], nodes = [];
  const walk = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, { acceptNode: n => n.parentElement.closest("script,style,.pin-btn,.tip") ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT });
  let prevSpace = true;
  for (let n = walk.nextNode(); n; n = walk.nextNode()) {
    const t = n.nodeValue;
    for (let i = 0; i < t.length; i++) {
      const sp = /\s/.test(t[i]);
      if (sp && prevSpace) continue;
      chars.push(sp ? " " : t[i]); nodes.push([n, i]); prevSpace = sp;
    }
  }
  return { str: chars.join(""), nodes };
}
const norm = s => s.replace(/\s+/g, " ").trim();
function locate(n) {
  const roots = [document.getElementById(n.section), document.querySelector(".dock-body"), document.querySelector("main")].filter(Boolean);
  const q = norm(n.quote);
  for (const root of roots) {
    const m = textMap(root);
    let i = n.prefix ? m.str.indexOf(norm(n.prefix) + " " + q) : -1;
    if (i >= 0) i += norm(n.prefix).length + 1;
    if (i < 0 && n.prefix) { const j = m.str.indexOf(norm(n.prefix) + q); if (j >= 0) i = j + norm(n.prefix).length; }
    if (i < 0) i = m.str.indexOf(q);
    if (i < 0) continue;
    const a = m.nodes[i], b = m.nodes[i + q.length - 1];
    const r = document.createRange(); r.setStart(a[0], a[1]); r.setEnd(b[0], b[1] + 1);
    return r;
  }
  return null;
}
async function jump(id) {
  await Story.ready;
  const n = notes.find(x => x.id === id); if (!n) return;
  const r = locate(n);
  history.replaceState(null, "", "#note-" + id);
  if (!r) {
    document.getElementById(n.section)?.scrollIntoView({ behavior: "smooth", block: "start" });
    flash("Text changed; jumped to the section"); return;
  }
  const el = r.startContainer.parentElement;
  el.scrollIntoView({ behavior: "smooth", block: "center" });
  if (window.CSS && CSS.highlights && window.Highlight) {
    CSS.highlights.set("note-jump", new Highlight(r));
    clearTimeout(jump.t); jump.t = setTimeout(() => CSS.highlights.delete("note-jump"), 3500);
  } else {
    el.classList.add("note-flash"); setTimeout(() => el.classList.remove("note-flash"), 3500);
  }
}

// ---------- select → "+ Note" ----------
function selectionInfo() {
  const sel = getSelection();
  if (!sel || sel.isCollapsed || !sel.rangeCount) return null;
  const r = sel.getRangeAt(0), quote = norm(sel.toString());
  if (quote.length < 2) return null;
  const host = r.commonAncestorContainer.nodeType === 1 ? r.commonAncestorContainer : r.commonAncestorContainer.parentElement;
  if (!host || host.closest(".notes, .notes-add, input, textarea, nav")) return null;
  const sec = host.closest("section.s") || host.closest(".dock-body")?.querySelector(".panel");
  const sectionId = host.closest("section.s")?.id || sec?.dataset.section;
  if (!sectionId) return null;
  // up to 40 visible characters before the selection, to tell repeated phrases apart
  const pre = document.createRange();
  const container = document.getElementById(sectionId) && document.getElementById(sectionId).contains(host) ? document.getElementById(sectionId) : host.closest(".panel") || host;
  pre.setStart(container, 0); pre.setEnd(r.startContainer, r.startOffset);
  const prefix = norm(pre.toString()).slice(-40);
  return { r, quote, sectionId, prefix };
}
function placeAdd() {
  const info = selectionInfo();
  if (!info) { addBtn.hidden = true; return; }
  const rect = info.r.getBoundingClientRect();
  if (rect.bottom < 0 || rect.top > innerHeight) { addBtn.hidden = true; return; }
  addBtn.hidden = false;
  const top = Math.max(8, rect.top - addBtn.offsetHeight - 8), left = Math.min(innerWidth - addBtn.offsetWidth - 8, Math.max(8, rect.left + rect.width / 2 - addBtn.offsetWidth / 2));
  addBtn.style.top = top + "px"; addBtn.style.left = left + "px";
}
document.addEventListener("selectionchange", () => { clearTimeout(placeAdd.t); placeAdd.t = setTimeout(placeAdd, 120); });
addBtn.addEventListener("pointerdown", e => e.preventDefault()); // keep the selection
addBtn.addEventListener("click", () => {
  const info = selectionInfo(); if (!info) return;
  const title = document.getElementById(info.sectionId)?.dataset.title || info.sectionId;
  const n = { id: uid(), created: new Date().toISOString(), section: info.sectionId, sectionTitle: title, quote: info.quote, prefix: info.prefix, text: "" };
  notes.push(n); save(); render(); addBtn.hidden = true; getSelection().removeAllRanges(); focusNote(n.id);
});
addEventListener("scroll", () => { if (!addBtn.hidden || getSelection().toString()) placeAdd(); }, { passive: true });

// ---------- start ----------
setOpen(ls.get(LS_OPEN) === "1");
load().then(() => { if (location.hash.startsWith("#note-")) jump(location.hash.slice(6)); });
addEventListener("hashchange", () => { if (location.hash.startsWith("#note-")) jump(location.hash.slice(6)); });
})();
