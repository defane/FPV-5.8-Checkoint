const $ = id => document.getElementById(id);
const fmt = ms => ms ? (ms/1000).toFixed(3) + " s" : "—";
const fmtUp = ms => { let s = ms/1000|0; return s<60 ? s+"s" : s<3600 ? (s/60|0)+"m"+(s%60)+"s" : (s/3600|0)+"h"+(s%3600/60|0)+"m"; };
const cnTxt = x => x > -990 ? (x/10).toFixed(1) : "—";
const rssiTxt = r => r ? r + " dBm" : "—";               // 0 = no frame yet
const lossTxt = n => { const t = (n.status_rx||0) + (n.status_lost||0);
  return t ? Math.round(100*(n.status_lost||0)/t) + "%" : "—"; };
const lossTitle = n => `${n.status_rx||0} status rx, ${n.status_lost||0} lost · ${n.rx_frames||0} frames total`;
// C5 health (local or a node's): calibration badge, capture faults, then its re-arm pill (`more`), details on hover
const calBadge = h => h.calibrating ? '<span class="pill busy">calibrating</span>'
  : h.cal_needed ? '<span class="pill bad" title="Run Cal B with every VTX off">cal needed</span>'
  : '<span class="pill ok">OK</span>';
const faultsTxt = h => h.faults ? ` <span class="bad">${h.faults} faults</span>` : "";
const healthInfo = h => `fw ${h.fw || "?"} · C5 up ${fmtUp((h.c5_uptime_s||0)*1000)} · heap min ${((h.heap_min||0)/1024).toFixed(1)} KB · gain ${h.gain} · ${h.passes} passes · ${h.faults} capture faults`;
const healthTd = (n, more = "") => n.fw ? `<td data-label="Health" title="${healthInfo(n)}">${calBadge(n)}${faultsTxt(n)}${more}</td>`
  : `<td class="muted" data-label="Health" title="no C5 status yet">—${more}</td>`;
const CMD = { cal_full: "Cal B", cal_quick: "Cal F", reboot: "Reboot" };
// Cal B / Cal F are off while that C5 calibrates: it would refuse (the master skips it as busy).
const cmdBtns = busy => { const d = busy ? " disabled" : "";
  return `<button class="ghost sm cmd" data-cmd="cal_full"${d} title="Full calibration (console B), VTXs off">Cal B</button>
  <button class="ghost sm cmd" data-cmd="cal_quick"${d} title="Quick calibration of the current filter (console F)">Cal F</button>
  <button class="ghost sm cmd" data-cmd="reboot">Reboot</button>`; };
let admin = false, editing = false, rearmDirty = false;
// A gate whose C5 echoes another re-arm drop than the one it should run (the fleet's, or its own): it missed the last
// CONFIG, or a console K e/E changed it; nothing while its C5 has not told it yet (-1). A gate with a D of its own says so.
const rearmPill = (x10, want) => x10 >= 0 && want != null && x10 !== want
  ? ` <span class="pill busy" title="Its C5 runs a re-arm drop of ${x10/10} dB, not the ${want/10} dB it should. The master sends it again when the gate comes back online or reboots; an admin's Apply sends it now.">D ${x10/10}</span>` : "";
const ownDPill = (own, x10) => own ? ` <span class="pill" title="This gate has a re-arm drop of its own (${x10/10} dB), not the fleet's. All gates in the D row, or Use the fleet's D, puts it back.">own D ${x10/10}</span>` : "";

// Info bubbles (the same snippet on the C3 and C6 pages): a button.info opens the .note its popovertarget names, a popover:
// light dismiss, Escape and the focus going back to the button are the browser's. A popover sits in the top layer, so the
// note is placed by script: under its button, above when there is no room below, inside the window, its arrow under the button
// (CSS anchor positioning is not in every browser yet). Without the Popover API a click toggles it, and Escape or a click
// elsewhere closes it.
const hasPopover = typeof HTMLElement !== "undefined" && "popover" in HTMLElement.prototype;
const NOTE_GAP = 8, NOTE_EDGE = 8, ARROW_EDGE = 14;
// Where a note of w x h goes for the button box r in a window of vw x vh: left and top in px, the arrow's offset in the note,
// and whether it sits above the button (when it does not fit below and does above, or fits nowhere: below).
function notePlace(r, w, h, vw, vh) {
  const left = Math.min(Math.max(r.left + r.width / 2 - w / 2, NOTE_EDGE), Math.max(vw - w - NOTE_EDGE, NOTE_EDGE));
  const fitsBelow = r.bottom + NOTE_GAP + h <= vh - NOTE_EDGE, fitsAbove = r.top - NOTE_GAP - h >= NOTE_EDGE;
  const above = !fitsBelow && fitsAbove;
  const top = above ? r.top - NOTE_GAP - h : r.bottom + NOTE_GAP;
  const arrow = Math.min(Math.max(r.left + r.width / 2 - left, ARROW_EDGE), Math.max(w - ARROW_EDGE, ARROW_EDGE));
  return { left, top, arrow, above };
}
const noteButton = n => document.querySelector(`button.info[popovertarget="${n.id}"]`);
function noteShow(n, b) {
  const r = b.getBoundingClientRect(), nr = n.getBoundingClientRect();
  const p = notePlace(r, nr.width, nr.height, document.documentElement.clientWidth, document.documentElement.clientHeight);
  n.style.left = p.left + "px"; n.style.top = p.top + "px";
  n.style.setProperty("--arrow", Math.round(p.arrow) + "px");
  n.classList.toggle("above", p.above);
  n.dataset.placed = "";                               // until then it is invisible: no flash at the wrong place
  b.classList.add("on");
}
function noteHide(n) {
  delete n.dataset.placed;
  const b = noteButton(n);
  if (b) b.classList.remove("on");
}
function infoClose() {
  document.querySelectorAll(".note").forEach(n => {
    if (hasPopover ? n.matches(":popover-open") : n.classList.contains("open")) {
      if (hasPopover) n.hidePopover(); else { n.classList.remove("open"); noteHide(n); }
    }
  });
}
if (hasPopover) {
  document.addEventListener("toggle", e => {          // toggle does not bubble: caught on its way down
    const n = e.target;
    if (!n.classList || !n.classList.contains("note")) return;
    const b = noteButton(n);
    if (e.newState === "open") { if (b) noteShow(n, b); } else noteHide(n);
  }, true);
} else {
  document.documentElement.classList.add("no-popover");
  document.addEventListener("click", e => {
    const b = e.target.closest("button.info"), n = b && document.getElementById(b.getAttribute("popovertarget"));
    const was = n && n.classList.contains("open");
    infoClose();
    if (n && !was) { n.classList.add("open"); noteShow(n, b); }
  });
  document.addEventListener("keydown", e => { if (e.key === "Escape") infoClose(); });
}
// Keyboard: the note would cover the controls under it, and a popover does not close when the focus leaves it.
document.addEventListener("focusin", e => {
  if (!e.target.closest || e.target.closest(".note") || e.target.closest("button.info")) return;
  infoClose();
});
// A scroll or a resize moves the button away from the note: it closes.
window.addEventListener("scroll", e => { if (!e.target.closest || !e.target.closest(".note")) infoClose(); }, true);
window.addEventListener("resize", infoClose);

async function post(url, obj) {     // signed (auth.js): the password never goes out
  const r = await AdminAuth.request("POST", url, JSON.stringify(obj));
  if (r.status === 401) setAdmin(false);
  return r;
}

// The maintenance view is the same document (#maintenance), so the admin session in memory carries over.
function route() {
  const m = location.hash === "#maintenance" && admin;
  $("home").hidden = m; $("maint").hidden = !m;
  if (m) window.scrollTo(0, 0);
}
window.addEventListener("hashchange", route);
function setAdmin(on) {             // back to spectator forgets the key
  admin = on; editing = false;
  if (!on) { AdminAuth.logout(); rearmDirty = false; rearmGate = -1; gainDirty = false; gainGate = 0; calGate = 0; calPhase = 0; infoClose(); dbgText = ""; dbgNext = 0; dbgGate = 0; $("dbg").open = false; dbgShow(false); dbgRender(); }
  document.body.classList.toggle("is-admin", on);
  $("role").setAttribute("aria-checked", on);
  if (!on && location.hash === "#maintenance") location.hash = "";
  route();
}

$("role").addEventListener("click", () => {
  if (admin) { setAdmin(false); poll(); return; }
  $("p").value = ""; $("login_msg").textContent = "";
  $("login").showModal();
});
// One login at a time (the key derivation takes a moment); closing the dialog
// (Cancel, Esc) abandons a login still running, and its result is ignored.
let attempt = 0, pending = false;
$("login_cancel").addEventListener("click", () => $("login").close());
$("login").addEventListener("close", () => {
  $("p").value = "";
  if (pending) { pending = false; attempt++; AdminAuth.logout(); $("unlock").disabled = false; }
});
$("login_form").addEventListener("submit", async e => {
  e.preventDefault();
  if (pending) return;
  const my = ++attempt;
  pending = true; $("unlock").disabled = true; $("login_msg").textContent = "checking…";
  let ok = false, msg = "wrong password";
  try { ok = await AdminAuth.login($("p").value); } catch (_) { msg = "offline"; }
  if (my !== attempt || !$("login").open) return;   // abandoned (its close event may still be queued)
  pending = false; $("unlock").disabled = false;
  if (ok) { setAdmin(true); $("login").close(); poll(); }
  else { $("login_msg").textContent = msg; $("p").select(); }
});

// Simple shows what spectators came for, the standings and the results; the choice stays on this device.
const MODE_KEY = "letztimer.mode";
function setSimple(simple, save) {
  document.body.classList.toggle("simple", simple);
  $("mode").setAttribute("aria-checked", !simple);
  if (save) try { localStorage.setItem(MODE_KEY, simple ? "simple" : "advanced"); } catch (_) {}
}
try { setSimple(localStorage.getItem(MODE_KEY) !== "advanced", false); } catch (_) { setSimple(true, false); }
$("mode").addEventListener("click", () => setSimple(!document.body.classList.contains("simple"), true));

// A channel is online while a pass is recent. Its badge tells how long ago in a bubble,
// on hover or on tap: the rows are only rebuilt when they change, so the bubble holds
// under the mouse, and the channel tapped open is kept here.
const ONLINE_MS = 30000;
const chName = c => c < 8 ? "R" + (c + 1) : c;
const statusTd = (x, up) => { const on = up - x.last_seen_ms < ONLINE_MS;
  return `<td><span class="tip" data-ch="${x.channel}" data-seen="${x.last_seen_ms}">` +
    `<span class="pill${on ? " ok" : ""}">${on ? "online" : "offline"}</span><span class="bubble"></span></span></td>`; };
let shownRows = "", tipCh = null, tipUp = 0;
function fillTips(up = tipUp) {
  tipUp = up;
  document.querySelectorAll("#standings .tip").forEach(t => {
    t.classList.toggle("open", t.dataset.ch === tipCh);
    t.lastChild.textContent = "last seen " + fmtUp(Math.max(0, up - t.dataset.seen)) + " ago";
  });
}
document.addEventListener("click", e => {
  const t = e.target.closest(".tip");
  tipCh = t && t.dataset.ch !== tipCh ? t.dataset.ch : null;
  fillTips();
});

// Last results: a column per channel, its own newest lap on top, so a row holds laps of
// the same age. Sector k runs from gate k-1 to gate k, the last one closing on gate 0;
// a missed gate leaves the two around it empty. Fetched only when a lap count changes.
let results = { num_gates: 1, channels: [] }, resultsSig = null, openLap = null, paused = false, live = [];
const secTxt = ms => ms ? (ms/1000).toFixed(2) : "—";
function sectorMs(lap, k, n) {
  const from = k === 1 ? 0 : lap.splits[k-2], to = k === n ? lap.lap_ms : lap.splits[k-1];
  return (k === 1 || from) && to > from ? to - from : 0;
}
// While paused, how far a column is behind the race; fewer laps than shown means a reset.
function lateBadge(c) {
  if (!paused) return "";
  const late = (live.find(x => x.channel === c.channel)?.laps ?? 0) - (c.laps[0]?.number ?? 0);
  return late ? `<span class="pill busy late">${late > 0 ? "+" + late : "reset"}</span>` : "";
}
function fillResults() {
  const n = results.num_gates, known = results.channels.map(c => c.channel);
  const added = paused ? live.filter(x => !known.includes(x.channel)).map(x => ({ channel: x.channel, laps: [] })) : [];
  const ch = [...results.channels, ...added].sort((a, b) => a.channel - b.channel);
  if (!ch.length) { $("results").innerHTML = '<tr><td class="muted">no laps yet</td></tr>'; return; }
  const cells = f => ch.map(c => `<td>${f(c)}</td>`).join("");
  let h = `<tr><th>Lap</th>${ch.map(c => `<th>${chName(c.channel)}${lateBadge(c)}</th>`).join("")}</tr>`;
  for (let r = 0; r < Math.max(...ch.map(c => c.laps.length)); r++) {
    const open = n > 1 && r === openLap, lapOf = (c, f) => c.laps[r] ? f(c.laps[r]) : "";
    h += `<tr class="lap" data-row="${r}"><td>${n > 1 ? (open ? "▾ " : "▸ ") : ""}${r ? "-" + r : "last"}</td>` +
         `${cells(c => lapOf(c, l => secTxt(l.lap_ms)))}</tr>`;
    if (!open) continue;
    h += `<tr class="sub"><td>lap no.</td>${cells(c => lapOf(c, l => l.number))}</tr>`;
    for (let k = 1; k <= n; k++)
      h += `<tr class="sub"><td>G${k-1}→${k % n}</td>${cells(c => lapOf(c, l => secTxt(sectorMs(l, k, n))))}</tr>`;
  }
  $("results").innerHTML = h + `<tr class="admin-only"><td><button class="warn sm reset_ch">Reset all</button></td>` +
    `${cells(c => `<button class="warn sm reset_ch" data-ch="${c.channel}">Reset</button>`)}</tr>`;
}
async function loadResults(st) {
  live = st;
  const sig = st.map(x => x.channel + ":" + x.laps).join();
  if (sig === resultsSig) return;
  if (paused) { resultsSig = sig; fillResults(); return; }   // only the badges move
  try {
    const r = await fetch("/api/results");
    if (r.ok) { results = await r.json(); resultsSig = sig; fillResults(); }
  } catch (_) {}                    // the next poll tries again
}
$("results").addEventListener("click", async e => {
  const b = e.target.closest(".reset_ch");
  if (b) {
    const one = b.dataset.ch !== undefined;
    if (confirm(one ? `Reset the times of ${chName(+b.dataset.ch)}?` : "Reset the times of every channel?")) {
      await post("/api/reset", one ? { channel: +b.dataset.ch } : {}); setPaused(false); poll();
    }
    return;
  }
  const tr = e.target.closest("tr.lap");
  if (tr && results.num_gates > 1) { openLap = +tr.dataset.row === openLap ? null : +tr.dataset.row; fillResults(); }
});

function setPaused(on) {
  paused = on; resultsSig = null;
  $("pause").setAttribute("aria-checked", on);
  loadResults(live);
}
$("pause").addEventListener("click", () => setPaused(!paused));


// Channels: one row per gate, R1..R8; a pass lights its cell for HIT_MS, its age
// taken here, since a pass and the master do not share a clock.
const HIT_MS = 3000;
const hits = new Map();
let seenPasses = null;
// Each cell shows a channel's level, its dB above the noise: a VTX reads the same
// whenever the page was opened and however long it stays on.
const QUIET_DB = 3, RISE_HIGH = 20;
const riseAlpha = rise => Math.min(1, Math.max(0, (rise - QUIET_DB)/(RISE_HIGH - QUIET_DB)));
const riseTxt = rise => rise < QUIET_DB ? "0" : "+" + rise.toFixed(0);
// Channel states (the same snippet on the C3 and C6 pages): each channel's pd_state_t, sent by
// its C5 once a second. RISING and PEAK_PASSED (one reading) pass too fast for that; COOLDOWN
// holds until the channel re-arms, and explains a lap not counted (a VTX left near the gate):
// a dashed blue frame, under the yellow of a pass while that lasts.
const STATE_TXT = ["idle", "rising", "cooldown", "cooldown"];
const frame = (hit, st) => hit ? " hit" : st === 2 || st === 3 ? " cool" : "";
const chTitle = (name, x10, st) => [name, x10 <= -990 && "not measured", STATE_TXT[st]].filter(Boolean).join(" · ");
function notePasses(s) {
  const fresh = seenPasses === null ? 0 : Math.max(0, s.total_passes - seenPasses);
  seenPasses = s.total_passes;
  (s.passes || []).slice(0, fresh).forEach(p => hits.set(`${p.gate}:${p.channel}`, Date.now() + HIT_MS));
}
const lvCell = (x10, gate, ch, st) => {
  const cls = frame((hits.get(`${gate}:${ch}`) || 0) > Date.now(), st);
  const title = `title="${chTitle(`Gate ${gate} · R${ch + 1}`, x10, st)}"`;
  if (x10 <= -990) return `<td class="lv none${cls}" ${title}>—</td>`;
  const rise = x10/10;
  return `<td class="lv${cls}" ${title} style="background:rgba(46,204,113,${riseAlpha(rise).toFixed(2)})">${riseTxt(rise)}</td>`;
};
// A gate's states only while it reports (a node offline, gate 0 not linked: its levels are unknown too).
const chanRow = (name, gate, levels, states) =>
  `<tr><td>${name}</td>${(levels || []).map((v, ch) => lvCell(v, gate, ch, (states || [])[ch])).join("")}</tr>`;
function fillChannels(s) {
  notePasses(s);
  const rows = [...(s.nodes || [])].sort((a, b) => a.gate - b.gate)
    .map(n => chanRow(`${n.gate} ${n.mac.slice(12)}`, n.gate, n.levels, n.online && n.states));
  const L = s.local || {};
  $("chans").innerHTML = chanRow("0 master", 0, L.levels, s.has_local && L.states) + rows.join("");
}

async function poll() {
  if (document.hidden) return;                  // a hidden tab asks the ESP for nothing
  try {
    const s = await (await fetch("/api/state")).json();
    fillChannels(s);
    dbgGates(s);
    gainGates(s);
    calGates(s);
    $("conn").hidden = true;
    $("pair_state").textContent = s.pairing ? "OPEN" : "closed";
    $("pair_state").className = s.pairing ? "on" : "off";
    $("pair_left").textContent = s.pairing ? `closes in ${Math.floor(s.pairing_left_s / 60)}:${String(s.pairing_left_s % 60).padStart(2, "0")}` : "";
    const L = s.local || {};
    $("l_health").innerHTML = s.has_local ? calBadge(L) + faultsTxt(L) + rearmPill(L.rearm_db_x10, L.rearm_want_x10) + ownDPill(L.rearm_own, L.rearm_want_x10) : "—";
    $("l_info").textContent = s.has_local ? healthInfo(L) : "local C5 not linked (no report for 3 s)";
    renderMaintGates(s);
    $("l_passes").textContent = s.total_passes;
    if (s.volume != null) { $("vol_v").textContent = s.volume; if (document.activeElement !== $("vol")) $("vol").value = s.volume; }
    lastState = s;
    rearmGates(s);
    if (s.uptime_ms != null) $("m_uptime").textContent = fmtUp(s.uptime_ms);
    if (s.free_heap != null) {
      const kb = s.free_heap/1024|0;
      if (s.heap_total > 0) {
        $("m_heap").textContent = Math.round(100*s.free_heap/s.heap_total) + "%";
        $("m_heap").title = kb + " KB free of " + (s.heap_total/1024|0) + " KB";
      } else { $("m_heap").textContent = kb + " KB"; }
    }

    const st = s.standings || [];
    const rows = st.length ? st.map(x => `
      <tr><td>${chName(x.channel)}</td>${statusTd(x, s.uptime_ms)}<td>${x.laps}</td><td>${fmt(x.last_lap_ms)}</td>
      <td>${fmt(x.best_lap_ms)}</td><td>${x.last_gate<0?'—':x.last_gate}</td></tr>`).join("")
      : '<tr><td colspan="6" class="muted">no passes yet</td></tr>';
    if (rows !== shownRows) $("standings").innerHTML = shownRows = rows;
    fillTips(s.uptime_ms);
    loadResults(st);

    if (!editing) {
      const nodes = s.nodes || [];
      const up = s.uptime_ms || 0;
      const seen = n => n.online ? Math.max(0, (up - n.last_seen_ms)/1000 | 0) + "s" : "—";
      const rearm = n => n.online ? rearmPill(n.rearm_db_x10, n.rearm_want_x10) + ownDPill(n.rearm_own, n.rearm_want_x10) + (n.custom ? CAL_PILL : "") : "";   // after its health
      const byGate = [...nodes].sort((a, b) => a.gate - b.gate || a.node_id - b.node_id);
      const ae = document.activeElement;                                   // a grip in focus keeps it over the redraw
      const gripOf = ae && ae.classList && ae.classList.contains("grip") && $("nodes").contains(ae) ? ae.closest("tr").dataset.id : null;
      $("nodes").innerHTML = nodes.length ? byGate.map(n => admin ? `
        <tr data-id="${n.node_id}" data-gate="${n.gate}">
          <td class="acts" data-label="Gate"><button type="button" class="grip" aria-label="Reorder gate ${n.gate}, node ${n.mac.slice(9)}: drag, or press up and down" aria-keyshortcuts="ArrowUp ArrowDown" title="Drag to change the order">⠿</button><b class="gnum">${n.gate}</b></td>
          <td class="muted" data-label="Node">${n.mac.slice(9)}</td>
          <td class="${n.online?'on':'off'}" data-label="Link">${n.online?'online':'offline'}</td>
          ${healthTd(n, rearm(n))}
          <td data-label="RSSI">${rssiTxt(n.rssi)}</td>
          <td title="${lossTitle(n)}" data-label="Loss">${lossTxt(n)}</td>
          <td data-label="Seen">${seen(n)}</td>
          <td class="admin-only acts"><button type="button" class="warn sm rm" title="Forget this gate: it must pair again">Remove</button></td>
        </tr>` : `
        <tr><td data-label="Gate">${n.gate}</td><td class="muted" data-label="Node">${n.mac.slice(9)}</td>
          <td class="${n.online?'on':'off'}" data-label="Link">${n.online?'online':'offline'}</td>
          ${healthTd(n, rearm(n))}
          <td data-label="RSSI">${rssiTxt(n.rssi)}</td>
          <td title="${lossTitle(n)}" data-label="Loss">${lossTxt(n)}</td>
          <td data-label="Seen">${seen(n)}</td></tr>`).join("")
        : '<tr><td colspan="8" class="muted">no nodes paired</td></tr>';
      if (gripOf !== null) { const g = $("nodes").querySelector(`tr[data-id="${gripOf}"] .grip`); if (g) g.focus({ preventScroll: true }); }
    }
  } catch (e) { $("conn").hidden = false; }
}

$("pair_toggle").addEventListener("click", async () => {
  await post("/api/pairing", { open: $("pair_state").className === "on" ? 0 : 1 }); poll();
});

// Calibrate/reboot a gate's C5 (gate -1 = every reachable gate). The master holds a
// calibration when a target sees a strong signal (a VTX probably on) until forced.
async function sendCmd(gate, cmd, force) {
  try {
    const r = await post("/api/command", { gate, cmd, force });
    return r.ok ? await r.json() : { error: r.status === 401 ? "admin password needed" : "refused (" + r.status + ")" };
  } catch (e) {   // back to spectator meanwhile: nothing to report
    return { error: /logged out|not logged in/.test(e.message) ? "logged out" : "master not answering" };
  }
}
async function command(gate, cmd) {
  if (!confirm(cmd === "reboot" ? `Reboot the C5 of gate ${gate}? It stops detecting until it is back up.`
      : "All VTXs must be OFF. The gate(s) stop detecting while calibrating (a few seconds). Continue?")) return;
  let r = await sendCmd(gate, cmd, 0);
  if (r.vtx && r.vtx.length) {
    const g = r.vtx.map(v => `${v.gate} (${cnTxt(v.cn_x10)} dB over its rest)`).join(", ");
    if (!confirm(`Strong signal on gate(s) ${g}: a VTX may be on. Force?`)) return;
    r = await sendCmd(gate, cmd, 1);
  }
  const sent = r.sent || [], failed = r.failed || [], busy = r.busy || [];
  const msg = CMD[cmd] + ": " + (r.error || (sent.length ? "sent to gate(s) " + sent.join(", ") : "no gate reached")
    + (busy.length ? " · skipped, already calibrating: " + busy.join(", ") : "")
    + (failed.length ? " · not reached (offline or no C5): " + failed.join(", ") : ""));
  if (r.error === "logged out") { $("cmd_msg").textContent = ""; return; }
  const bad = r.error || failed.length || busy.length || !sent.length;
  $("cmd_msg").textContent = msg;
  $("cmd_msg").className = bad ? "bad" : "muted";
  if (bad) alert(msg);
}
$("cal_all").addEventListener("click", async () => { await command(-1, "cal_full"); poll(); });
// Maintenance: a row per gate that has a C5 (gate 0, then each node), its calibrate/reboot buttons.
let maintSig = "";
function renderMaintGates(s) {
  const L = s.local || {};
  const rows = [...(s.has_local ? [{ gate: 0, name: "0 · master", h: L, busy: L.calibrating }] : []),
    ...[...(s.nodes || [])].sort((a, b) => a.gate - b.gate)
      .map(n => ({ gate: n.gate, name: `${n.gate} · ${n.mac.slice(9)}`, h: n, busy: n.online && n.calibrating, off: !n.online }))];
  const html = rows.length ? rows.map(r => `<tr data-gate="${r.gate}"><td>${r.name}</td>` +
    `<td>${r.off ? '<span class="muted">offline</span>' : r.h.fw ? calBadge(r.h) + faultsTxt(r.h) : '<span class="muted">—</span>'}</td>` +
    `<td class="acts">${r.off ? "" : cmdBtns(r.busy)}</td></tr>`).join("")
    : '<tr><td colspan="3" class="muted">no gate yet</td></tr>';
  if (html !== maintSig) { maintSig = html; $("maint_gates").innerHTML = html; }
}
$("maint_gates").addEventListener("click", async e => {
  const b = e.target.closest("button.cmd"), tr = e.target.closest("tr");
  if (!b || !tr || !admin) return;
  await command(+tr.dataset.gate, b.dataset.cmd); poll();
});
$("vol").addEventListener("input", () => { $("vol_v").textContent = $("vol").value; });
// The fleet's re-arm drop: saved by the master and sent to every gate (refused outside 0..40 dB).
// D for every gate (the fleet's) or one gate alone (its own); the field shows what the chosen one should run.
let rearmGate = -1, lastState = null;
const gateD = (s, g) => g < 0 ? s.rearm_db_x10 : g === 0 ? s.local : (s.nodes || []).find(n => n.gate === g);
function rearmGates(s) {
  const gates = [-1, ...(s.has_local ? [0] : []), ...[...new Set((s.nodes || []).map(n => n.gate))].sort((a, b) => a - b)];
  if (!gates.includes(rearmGate)) gates.push(rearmGate);          // a gate that dropped keeps its entry while selected
  const sel = $("rearm_gate");
  const want = gates.map(g => `<option value="${g}">${g < 0 ? "All gates" : g === 0 ? "0 · master" : g}</option>`).join("");
  if (sel.innerHTML !== want) sel.innerHTML = want;
  if (sel.value !== String(rearmGate)) sel.value = rearmGate;
  const d = gateD(s, rearmGate);
  const x10 = rearmGate < 0 ? d : d && d.rearm_want_x10;
  if (x10 != null && !rearmDirty && document.activeElement !== $("rearm")) $("rearm").value = x10 / 10;
  $("rearm_reset").hidden = !(rearmGate >= 0 && d && d.rearm_own);
  $("rearm_save").textContent = rearmGate < 0 ? "Apply to every gate" : `Apply to gate ${rearmGate}`;
}
$("rearm_gate").addEventListener("change", () => { rearmGate = +$("rearm_gate").value; rearmDirty = false; $("rearm_msg").textContent = ""; if (lastState) rearmGates(lastState); });
$("rearm").addEventListener("input", () => { rearmDirty = true; });
async function sendRearm(x10, gate, okMsg) {
  $("rearm_msg").textContent = "sending…"; $("rearm_msg").className = "muted";
  let msg = "refused", ok = false;
  try {
    const r = await post("/api/rearm", { rearm_db_x10: x10, gate });
    ok = r.ok && (await r.json()).ok;
    msg = ok ? okMsg : r.status === 401 ? "admin password needed" : "refused";
  } catch (e) { msg = /logged out|not logged in/.test(e.message) ? "" : "master not answering"; }
  if (ok) rearmDirty = false;
  $("rearm_msg").textContent = msg; $("rearm_msg").className = ok ? "muted" : "bad";
  poll();
}
$("rearm_save").addEventListener("click", () => {
  const v = parseFloat($("rearm").value);
  if (!(v >= 0 && v <= 40)) { $("rearm_msg").textContent = "0 to 40 dB"; $("rearm_msg").className = "bad"; return; }
  const own = lastState ? [...(lastState.has_local && lastState.local.rearm_own ? [0] : []), ...(lastState.nodes || []).filter(n => n.rearm_own).map(n => n.gate)] : [];
  if (rearmGate < 0 && own.length && !confirm(`Gate${own.length > 1 ? "s" : ""} ${own.join(", ")} ${own.length > 1 ? "have" : "has"} a D of ${own.length > 1 ? "their" : "its"} own: ${own.length > 1 ? "they take" : "it takes"} this one too. Continue?`)) return;
  sendRearm(Math.round(v * 10), rearmGate, rearmGate < 0 ? "sent to every gate" : `gate ${rearmGate} has its own D`);
});
$("rearm_reset").addEventListener("click", () => {
  if (confirm(`Gate ${rearmGate} runs the fleet's D again (its own is dropped)?`)) sendRearm(-1, rearmGate, `gate ${rearmGate} is on the fleet's D`);
});
$("vol").addEventListener("change", async () => { await post("/api/volume", { volume: +$("vol").value }); });
// Gate order: drag a row by its grip, or focus it and press the arrows; the rows are renumbered as they move, and the
// order goes to the master once the row is dropped (/api/order, every node id, first gate first). The table is not
// redrawn by the poll meanwhile (editing). One pointer drives a drag, Escape puts it back.
let dragRow = null, dragPointer = null, orderBefore = "", orderTimer = null, liveTimer = null;
const rowIds = () => [...$("nodes").children].map(tr => +tr.dataset.id);
const rowMac = tr => tr.children[1].textContent;
function say(text) {                                   // polite, once, after a beat (a move that refocuses is swallowed otherwise)
  clearTimeout(liveTimer); $("order_live").textContent = "";
  liveTimer = setTimeout(() => { $("order_live").textContent = text; }, 300);
}
function renumber() {
  [...$("nodes").children].forEach((tr, i) => {
    const g = tr.querySelector(".gnum"), grip = tr.querySelector(".grip");
    if (g) g.textContent = i + 1;
    if (grip) grip.setAttribute("aria-label", `Reorder gate ${i + 1}, node ${rowMac(tr)}: drag, or press up and down`);
  });
}
// moveBefore keeps what lives in a row (the focused grip); append/insertBefore drops it, so the grip is focused again by the callers.
const canMoveBefore = typeof Element !== "undefined" && "moveBefore" in Element.prototype;
function moveRow(tr, before) {                       // before: the row it goes in front of, null = last
  if (before === tr || tr.nextElementSibling === before) return false;
  if (canMoveBefore) $("nodes").moveBefore(tr, before); else $("nodes").insertBefore(tr, before);
  renumber();
  return true;
}
async function commitOrder() {
  clearTimeout(orderTimer); orderTimer = null;
  const order = rowIds();
  if (order.join() === orderBefore) { editing = false; renumber(); return; }
  $("order_msg").textContent = "saving…"; $("order_msg").className = "muted";
  let msg = "", bad = false;
  try {
    const r = await post("/api/order", { order });
    const ok = r.ok && (await r.json()).ok;
    msg = ok ? "order saved" : r.status === 401 ? "admin password needed" : "the list changed: try again";
    bad = !ok;
    if (ok) gatesRenumbered();
  } catch (e) { msg = /logged out|not logged in/.test(e.message) ? "" : "master not answering"; bad = !!msg; }
  $("order_msg").textContent = msg; $("order_msg").className = bad ? "bad" : "muted";
  if (msg) say(msg);
  if (!bad) setTimeout(() => { if ($("order_msg").textContent === msg) $("order_msg").textContent = ""; }, 4000);
  editing = false;
  poll();
}
// Gate numbers now name other nodes: the gate the pages' controls had chosen would act on the wrong one, so they go back to gate 0.
function gatesRenumbered() {
  gainGate = 0; gainDirty = false; rearmGate = -1; rearmDirty = false; calGate = 0; calPhase = 0; dbgGate = 0;
  dbgShow(false); dbgReset();
}
function restoreOrder() {                             // Escape: the rows as they were when the drag began
  const byId = new Map([...$("nodes").children].map(tr => [String(tr.dataset.id), tr]));
  for (const id of orderBefore.split(",")) { const tr = byId.get(id); if (tr) moveRow(tr, null); }
}
$("nodes").addEventListener("pointerdown", e => {
  const grip = e.target.closest(".grip");
  if (!grip || !admin || e.button || dragRow) return;   // the primary button, one drag at a time
  dragRow = grip.closest("tr"); dragPointer = e.pointerId;
  editing = true; orderBefore = rowIds().join();
  dragRow.classList.add("dragging");
  document.documentElement.classList.add("gate-dragging");
  $("nodes").setPointerCapture(e.pointerId);        // on the tbody: it is not moved, the row is
  e.preventDefault();
});
$("nodes").addEventListener("pointermove", e => {
  if (!dragRow || e.pointerId !== dragPointer) return;   // a second finger changes nothing
  const rows = [...$("nodes").children].filter(r => r !== dragRow);
  moveRow(dragRow, rows.find(r => { const b = r.getBoundingClientRect(); return e.clientY < b.top + b.height / 2; }) || null);
});
function endDrag(e, cancel) {
  if (!dragRow || (e && e.pointerId !== dragPointer)) return;
  const row = dragRow;
  row.classList.remove("dragging"); document.documentElement.classList.remove("gate-dragging");
  dragRow = dragPointer = null;
  if (cancel) { restoreOrder(); editing = false; say("Reordering cancelled"); return; }
  const place = rowIds().indexOf(+row.dataset.id) + 1;
  if (rowIds().join() !== orderBefore) say(`Gate ${place} of ${rowIds().length}: ${rowMac(row)} moved to position ${place}`);
  commitOrder();
}
$("nodes").addEventListener("pointerup", e => endDrag(e, false));
$("nodes").addEventListener("pointercancel", e => endDrag(e, true));
$("nodes").addEventListener("lostpointercapture", e => endDrag(e, false));   // capture ended some other way: drop where it stands
document.addEventListener("keydown", e => { if (e.key === "Escape" && dragRow) { e.preventDefault(); endDrag(null, true); } });
$("nodes").addEventListener("keydown", e => {         // a grip in focus: ↑ ↓ move its row one place, saved a moment after the last
  const grip = e.target.closest(".grip");
  if (!grip || !admin || (e.key !== "ArrowUp" && e.key !== "ArrowDown")) return;
  e.preventDefault();
  const tr = grip.closest("tr");
  if (!editing) { editing = true; orderBefore = rowIds().join(); }
  const moved = e.key === "ArrowUp" ? tr.previousElementSibling && moveRow(tr, tr.previousElementSibling)
                                    : tr.nextElementSibling && moveRow(tr, tr.nextElementSibling.nextElementSibling);
  if (moved) {
    grip.focus({ preventScroll: true });
    tr.scrollIntoView({ block: "nearest", inline: "nearest" });
    const place = rowIds().indexOf(+tr.dataset.id) + 1;
    say(`Gate ${place} of ${rowIds().length}: ${rowMac(tr)}`);
  }
  clearTimeout(orderTimer); orderTimer = setTimeout(commitOrder, 800);
});
$("nodes").addEventListener("click", async e => {
  const tr = e.target.closest("tr"); if (!tr || !admin || !e.target.classList.contains("rm")) return;
  const id = +tr.dataset.id, gate = tr.querySelector(".gnum").textContent, mac = tr.children[1].textContent;
  if (!confirm(`Remove gate ${gate} (node ${mac})? It stops reporting and has to pair again. The other gates keep their numbers.`)) return;
  try { await post("/api/node_remove", { node_id: id }); gatesRenumbered(); }
  catch (_) { $("cmd_msg").textContent = "remove failed: master not answering"; }
  poll();
});
// RX gain of one gate (a C5 forces it, saved there): the field follows the selected gate's gain until
// the admin edits it. The VTX guard is the calibrations': a gate that sees a carrier asks first.
let gainGate = 0, gainDirty = false;
function gainGates(s) {
  const gates = [-1, ...(s.has_local ? [0] : []), ...[...new Set((s.nodes || []).filter(n => n.online).map(n => n.gate))].sort((a, b) => a - b)];
  if (!gates.includes(gainGate)) gates.push(gainGate);          // a gate that dropped keeps its entry while selected
  const sel = $("gain_gate");
  const want = gates.map(g => `<option value="${g}">${g < 0 ? "All gates" : g === 0 ? "0 · master" : g}</option>`).join("");
  if (sel.innerHTML !== want) sel.innerHTML = want;
  if (sel.value !== String(gainGate)) sel.value = gainGate;
  const c5 = gainGate < 0 ? null : gainGate === 0 ? (s.has_local ? s.local : null) : (s.nodes || []).find(n => n.gate === gainGate && n.online);
  const now = c5 && c5.fw ? c5.gain : null;
  if (!gainDirty && document.activeElement !== $("gain_in") && now !== null) $("gain_in").value = now;
  $("gain_set").disabled = !!(c5 && c5.calibrating);
}
$("gain_gate").addEventListener("change", () => { gainGate = +$("gain_gate").value; gainDirty = false; $("gain_msg").textContent = ""; poll(); });
$("gain_in").addEventListener("input", () => { gainDirty = true; });
async function sendGain(gate, gain, force) {
  try {
    const r = await post("/api/gain", { gate, gain, force });
    return r.ok ? await r.json() : { error: r.status === 401 ? "admin password needed" : r.status === 400 ? "10 to 83" : "refused (" + r.status + ")" };
  } catch (e) { return { error: /logged out|not logged in/.test(e.message) ? "logged out" : "master not answering" }; }
}
$("gain_set").addEventListener("click", async () => {
  const gate = gainGate, gain = Math.round(+$("gain_in").value);
  if (!(gain >= 10 && gain <= 83)) { $("gain_msg").textContent = "10 to 83"; $("gain_msg").className = "bad"; return; }
  const who = gate < 0 ? "every gate" : `gate ${gate}`;
  if (!confirm(`Set the RX gain of ${who} to ${gain}? ${gate < 0 ? "Every reachable gate stops" : "That gate stops"} detecting for a few seconds while it learns its rests again. All VTXs must be OFF.`)) return;
  $("gain_msg").textContent = "sending…"; $("gain_msg").className = "muted";
  let r = await sendGain(gate, gain, 0);
  if (r.vtx && r.vtx.length) {
    const seen = r.vtx.map(v => `gate ${v.gate} (${cnTxt(v.cn_x10)} dB over its rest)`).join(", ");
    if (!confirm(`${seen[0].toUpperCase() + seen.slice(1)} ${r.vtx.length > 1 ? "see" : "sees"} a strong signal: a VTX may be on. Force the change?`)) {
      $("gain_msg").textContent = "cancelled"; return;
    }
    r = await sendGain(gate, gain, 1);
  }
  if (r.error === "logged out") { $("gain_msg").textContent = ""; return; }
  const sent = (r.sent || []).length, busy = (r.busy || []).length, failed = (r.failed || []).length;
  const list = a => a.join(", ");
  const msg = r.error || (gate >= 0
    ? (sent ? "sent" : busy ? "not sent: the gate is calibrating" : "not reached (offline or no C5)")
    : (sent ? `sent to gate${sent > 1 ? "s" : ""} ${list(r.sent)}` : "no gate reached") +
      (busy ? ` · skipped, calibrating: ${list(r.busy)}` : "") + (failed ? ` · not reached: ${list(r.failed)}` : ""));
  $("gain_msg").textContent = msg; $("gain_msg").className = sent ? "muted" : "bad";
  if (sent) { gainDirty = false; setTimeout(() => $("gain_msg").textContent = "", 4000); poll(); }
});

// Passage calibration: a gate's C5 measures a drone in the passage, then 2 m away, and proposes thresholds.
let calGate = 0, calPhase = 0, calAt = 0, calBusy = false;
const CAL_WARN = [
  [1, "No drone seen in the passage (nothing stood 3 dB over the rest): check its VTX is on, its channel, and that it is in the passage. Measure again."],
  [2, "The ADC clips in the passage: lower the gain (the gain control above), then measure again."],
  [4, "In the passage and 2 m away the drone reads almost the same (under 6 dB apart): the thresholds are a compromise. Check the antenna sits near the line, or lower the gain."],
  [8, "Hardly seen at 2 m: raise the gain, or check the drone was on and 2 m from the gate."],
  [16, "Another channel stood much higher than the one measured: another drone, or the drone is not on the channel chosen (pick Any). Measure again."],
  [32, "The C5 refused it (learning its rests, a calibration, a bench): try again in a few seconds."],
];
const dbh = x => (x / 2).toFixed(1);
const CAL_PILL = ' <span class="pill" title="Thresholds from a passage calibration, not the fleet\'s">calibrated</span>';
async function calCall(action) {
  try {
    const channel = action === "start1" ? +$("cal_ch_sel").value : 255;
    const r = await post("/api/calib", { gate: calGate, action, channel });
    return r.ok ? await r.json() : { error: r.status === 401 ? "admin password needed" : "refused (" + r.status + ")" };
  } catch (e) { return { error: /logged out|not logged in/.test(e.message) ? "logged out" : "master not answering" }; }
}
function calRender(k) {
  calPhase = k.phase;
  const names = ["no session", "measuring in the passage…", "passage measured", "measuring 2 m away…", "both measured"];
  const measuring = k.phase === 1 || k.phase === 3;
  $("cal_pill").textContent = names[k.phase] + (measuring ? ` ${k.secs_left} s` : "");
  $("cal_pill").className = "pill" + (measuring ? " busy" : k.phase === 4 ? " ok" : "");
  $("cal_s1").disabled = measuring;
  $("cal_ch_sel").disabled = measuring || k.phase >= 2;   // the channel is settled once step 1 is measured
  $("cal_s2").disabled = measuring || k.phase < 2 || (k.flags & 1) !== 0;
  $("cal_cancel").disabled = k.phase === 0;
  const show = k.phase >= 2;
  $("cal_result").hidden = !show;
  if (!show) return;
  const have2 = k.phase === 4;
  $("cal_rows").innerHTML = `<tr><td>${k.channel < 8 ? "R" + (k.channel + 1) : "—"}</td>` +
    `<td>median ${dbh(k.passage[1])} · p95 ${dbh(k.passage[3])}${k.passage_sat ? ` · ${k.passage_sat} % clipped` : ""}</td>` +
    `<td>${have2 ? `median ${dbh(k.away[1])} · p95 ${dbh(k.away[3])}` : "—"}</td></tr>`;
  $("cal_warn").innerHTML = CAL_WARN.filter(w => k.flags & w[0]).map(w => `<li>${w[1]}</li>`).join("") +
    (k.gain_steps ? `<li>Suggested: ${k.gain_steps < 0 ? "lower" : "raise"} the gain by one step (2).</li>` : "");
  const rec = have2 && !(k.flags & 1);
  $("cal_rec").hidden = !rec;
  if (rec) {
    $("cr_min").textContent = (k.rec.min_peak / 10).toFixed(1); $("cr_act").textContent = (k.rec.active / 10).toFixed(1);
    $("cr_hys").textContent = (k.rec.hysteresis / 10).toFixed(1); $("cr_drop").textContent = (k.rec.drop / 10).toFixed(1);
  }
}
async function calDo(action, msg) {
  if (calBusy) return;
  calBusy = true; $("cal_msg").textContent = msg || ""; $("cal_msg").className = "muted";
  const k = await calCall(action);
  calBusy = false;
  if (k.error === "logged out") { $("cal_msg").textContent = ""; return; }
  if (k.error || !k.ok) { $("cal_msg").textContent = k.error || k.msg; $("cal_msg").className = "bad"; }
  else $("cal_msg").textContent = action === "apply" ? "applied to the gate" : action === "reset" ? "back on the fleet's thresholds" : "";
  if (!k.error) calRender(k);
  calAt = Date.now();
}
async function calPoll() {
  if (document.hidden) return;   // a running or finished session every second, an idle one every 5 s (it may have been started elsewhere)
  if (!admin || $("home").hidden || document.body.classList.contains("simple") || calBusy) return;
  if (calPhase === 0 && Date.now() - calAt < 5000) return;
  const g = calGate, k = await calCall("state");
  if (k.error || g !== calGate) return;
  calRender(k);
  calAt = Date.now();
}
function calGates(s) {
  const gates = [...(s.has_local ? [0] : []), ...new Set((s.nodes || []).filter(n => n.online).map(n => n.gate))].sort((a, b) => a - b);
  if (!gates.includes(calGate)) gates.push(calGate);
  const sel = $("cal_gate");
  const want = gates.map(g => `<option value="${g}">${g === 0 ? "0 · master" : g}</option>`).join("");
  if (sel.innerHTML !== want) sel.innerHTML = want;
  if (sel.value !== String(calGate)) sel.value = calGate;
  const nd = (s.nodes || []).find(n => n.gate === calGate && n.online);
  $("cal_reset").hidden = !(nd && nd.custom);       // a remote gate running thresholds of its own
}
$("cal_gate").addEventListener("change", () => { calGate = +$("cal_gate").value; calPhase = 0; calAt = 0; $("cal_msg").textContent = ""; calRender({ phase: 0, flags: 0, passage: [], away: [], rec: {}, secs_left: 0 }); calPoll(); });
$("cal_s1").addEventListener("click", () => calDo("start1", "starting…"));
$("cal_s2").addEventListener("click", () => calDo("start2", "starting…"));
$("cal_cancel").addEventListener("click", () => calDo("cancel"));
$("cal_reset").addEventListener("click", () => {
  if (confirm(`Gate ${calGate} goes back to the fleet's thresholds (its calibration is dropped)?`)) calDo("reset", "sending…");
});
$("cal_apply").addEventListener("click", () => {
  if (confirm(`Apply these thresholds to gate ${calGate}? They replace its min peak, active, hysteresis and drop (saved on its C5).`)) calDo("apply", "sending…");
});
setInterval(calPoll, 1000);

// Debug: the C5's console text (POST /api/log from the position we have) and its keys.
const DBG_MAX = 60000;
let dbgNext = 0, dbgText = "", dbgPaused = false, dbgBusy = false, dbgOn = false, dbgGate = 0;
function dbgShow(on) {
  dbgOn = on; $("dbg_on").setAttribute("aria-checked", on); $("dbg_body").hidden = !on;
}
// The gate the panel talks to; its log, position and switch are that gate's (one is in debug mode at a time).
function dbgReset() { dbgText = ""; dbgNext = 0; dbgRender(); }
async function dbgSwitch(on) {   // on: true/false to set it, undefined to ask
  const gate = dbgGate;
  const r = await AdminAuth.request("POST", "/api/debug", JSON.stringify(on === undefined ? { gate } : { on: on ? 1 : 0, gate }));
  if (r.status === 401) { setAdmin(false); return; }
  if (!r.ok || gate !== dbgGate) return;
  const j = await r.json();
  if (!!j.debug !== dbgOn) dbgReset();
  dbgShow(!!j.debug);
  if (on && !j.ok) $("dbg_msg").textContent = "gate offline";
}
// The selector lists gate 0 and each online remote gate (state's nodes); the choice stays while it can.
function dbgGates(s) {
  const gates = [0, ...[...new Set((s.nodes || []).filter(n => n.online).map(n => n.gate))].sort((a, b) => a - b)];
  const sel = $("dbg_gate");
  if (!gates.includes(dbgGate)) gates.push(dbgGate);   // a gate that dropped keeps its entry while selected
  const want = gates.map(g => `<option value="${g}">${g === 0 ? "0 · master" : g}</option>`).join("");
  if (sel.innerHTML !== want) sel.innerHTML = want;
  if (sel.value !== String(dbgGate)) sel.value = dbgGate;
}
$("dbg_gate").addEventListener("change", () => {
  const was = dbgOn;
  dbgGate = +$("dbg_gate").value;
  dbgShow(false); dbgReset();
  $("dbg_msg").textContent = "";
  // A debug mode that was on follows to the new gate (the old one goes off); otherwise it is only asked,
  // so a mere choice never starts one.
  dbgSwitch(was ? true : undefined).catch(() => { $("dbg_msg").textContent = "failed"; });
});
$("dbg_on").addEventListener("click", () => { dbgSwitch(!dbgOn).catch(() => { $("dbg_msg").textContent = "failed"; }); });
function dbgRender() {
  const f = $("dbg_filter").value.toLowerCase();
  const lines = dbgText.split("\n");
  const shown = f ? lines.filter(l => l.toLowerCase().includes(f)) : lines;
  const box = $("dbg_log"), stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 8;
  box.textContent = shown.join("\n");
  if (stick) box.scrollTop = box.scrollHeight;
}
async function dbgPoll() {
  if (document.hidden || !admin || $("maint").hidden || !$("dbg").open || !dbgOn || dbgPaused || dbgBusy) return;
  dbgBusy = true;
  try {
    const gate = dbgGate;
    const r = await AdminAuth.request("POST", "/api/log", JSON.stringify({ since: dbgNext, gate }));
    if (r.status === 401) { setAdmin(false); return; }
    if (!r.ok) return;
    const j = await r.json();
    if (j.gate !== dbgGate) return;   // the selector moved while this was in flight
    if (!j.debug) { dbgShow(false); return; }   // the C6 rebooted, or the gate's node left debug mode: off again
    if (j.next < dbgNext) dbgText += "\n[C6 restarted]\n";
    if (j.lost) dbgText += "\n[log: older lines were lost]\n";
    dbgNext = j.next;
    if (j.text) {
      dbgText += j.text;
      if (dbgText.length > DBG_MAX) dbgText = dbgText.slice(dbgText.length - DBG_MAX);
      dbgRender();
    }
  } catch (_) { /* offline: the next tick retries */ }
  finally { dbgBusy = false; }
}
async function dbgKeys(keys, force) {
  const r = await AdminAuth.request("POST", "/api/key", JSON.stringify({ keys, force, gate: dbgGate }));
  if (r.status === 401) { setAdmin(false); return { ok: false, msg: "password required" }; }
  return r.ok ? r.json() : { ok: false, msg: "failed (" + r.status + ")" };
}
async function dbgSend(keys) {
  if (!keys) return;
  $("dbg_msg").textContent = "sending…";
  try {
    let r = await dbgKeys(keys, 0);
    if (r.vtx || r.blind) {
      const why = r.vtx ? `The gate sees a strong signal (${(r.cn_x10 / 10).toFixed(1)} dB over its rest): a VTX may be on. ` : "";
      if (!confirm(why + `"${keys}" blinds the gate (no detection) for a few seconds or the length of a bench; all VTXs should be OFF. Send?`)) {
        $("dbg_msg").textContent = "cancelled"; return;
      }
      r = await dbgKeys(keys, 1);
    }
    $("dbg_msg").textContent = r.ok ? "sent" : (r.msg || "failed");
    if (r.ok) { setTimeout(() => $("dbg_msg").textContent = "", 2000); setTimeout(dbgPoll, 300); }
  } catch (e) { $("dbg_msg").textContent = "failed"; }
}
$("dbg_send").addEventListener("click", () => { dbgSend($("dbg_keys").value); });
$("dbg_keys").addEventListener("keydown", e => { if (e.key === "Enter") dbgSend($("dbg_keys").value); });
document.querySelectorAll("button[data-keys]").forEach(b => b.addEventListener("click", () => dbgSend(b.dataset.keys)));
$("dbg_pause").addEventListener("click", () => {
  dbgPaused = !dbgPaused; $("dbg_pause").textContent = dbgPaused ? "Resume" : "Pause";
});
$("dbg_clear").addEventListener("click", () => { dbgText = ""; dbgRender(); });
$("dbg_filter").addEventListener("input", dbgRender);
$("dbg_copy").addEventListener("click", async () => {
  try { await navigator.clipboard.writeText(dbgText); $("dbg_msg").textContent = "copied"; }
  catch (_) { $("dbg_msg").textContent = "copy: select the log by hand"; }
  setTimeout(() => $("dbg_msg").textContent = "", 2000);
});
$("dbg").addEventListener("toggle", () => { if ($("dbg").open) dbgSwitch().catch(() => {}); });

AdminAuth.restore().then(ok => { if (ok) { setAdmin(true); poll(); } });   // the key this tab kept (sessionStorage)
poll(); setInterval(poll, 1000);
setInterval(dbgPoll, 1000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) { poll(); calPoll(); dbgPoll(); } });   // back in view: catch up now
