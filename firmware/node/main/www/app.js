const $ = id => document.getElementById(id);
const CH = n => "R" + (n + 1);
let dirty = false, admin = false;
["alpha","active","hyst","drop","minpeak","rearm","refr","stale"].forEach(
  id => $(id).addEventListener("input", () => { dirty = true; }));

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

// The maintenance view is the same document (#maintenance), so the admin session in memory carries over.
function route() {
  const m = location.hash === "#maintenance" && admin;
  $("home").hidden = m; $("maint").hidden = !m;
  if (m) window.scrollTo(0, 0);
}
window.addEventListener("hashchange", route);
function setAdmin(on) {             // back to spectator forgets the key and unsaved detector edits
  admin = on;
  if (!on) { AdminAuth.logout(); dirty = false; gainDirty = false; calPhase = 0; infoClose(); dbgText = ""; dbgNext = 0; $("dbg").open = false; dbgShow(false); dbgRender(); }
  document.body.classList.toggle("is-admin", on);
  $("role").setAttribute("aria-checked", on);
  if (!on && location.hash === "#maintenance") location.hash = "";
  route();
}

$("role").addEventListener("click", () => {
  if (admin) return setAdmin(false);
  $("p").value = ""; $("login_msg").textContent = "";
  $("login").showModal();
});
// One login at a time (the key derivation takes a moment); closing the dialog
// (Cancel, Esc) abandons a login still running, and its result is ignored.
// Signed requests (auth.js): the password never goes out.
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
  if (ok) { setAdmin(true); $("login").close(); }
  else { $("login_msg").textContent = msg; $("p").select(); }
});

function fillConfig(c) {
  if (dirty) return;                 // don't stomp on edits in progress
  $("alpha").value  = (c.alpha_milli / 1000).toFixed(2);
  $("active").value = (c.active_db_x10 / 10).toFixed(1);
  $("hyst").value   = (c.hysteresis_db_x10 / 10).toFixed(1);
  $("drop").value   = (c.drop_db_x10 / 10).toFixed(1);
  $("minpeak").value= (c.min_peak_db_x10 / 10).toFixed(1);
  $("rearm").value  = c.rearm_db_x10 / 10;
  $("refr").value   = c.refractory_ms;
  $("stale").value  = c.stale_ms;
}

const upTime = s => s >= 3600 ? `${Math.floor(s / 3600)}h ${String(Math.floor(s / 60) % 60).padStart(2, "0")}m`
                  : s >= 60 ? `${Math.floor(s / 60)}m ${String(s % 60).padStart(2, "0")}s` : `${s}s`;

function fillHealth(s) {
  const st = s.stats || {}, ok = s.has_stats;
  const cal = !ok ? ["—", "pill"] : st.calibrating ? ["calibrating", "pill busy"]
            : st.cal_needed ? ["calibration needed", "pill warn"] : ["OK", "pill ok"];
  $("h_cal").textContent = cal[0]; $("h_cal").className = cal[1];
  $("h_gain").textContent = ok ? st.gain : "—";
  $("h_up").textContent = ok ? upTime(st.uptime_s) : "—";
  $("h_heap").textContent = ok ? (st.heap_min / 1024).toFixed(1) + " KB" : "—";
  $("h_faults").textContent = ok ? st.faults : "—";
  $("h_fw").textContent = ok && st.fw ? st.fw : "—";
  document.querySelectorAll('button[data-cmd^="cal_"]').forEach(b => { b.disabled = ok && st.calibrating; });
  $("gain_set").disabled = ok && st.calibrating;
  if (ok && !gainDirty && document.activeElement !== $("gain_in")) $("gain_in").value = st.gain;
}
// The detector at a glance: its C5 linked, then its calibration (details in the C5 health card).
const detPill = s => { const st = s.stats || {};
  return !s.has_stats ? ["no C5 yet", "pill"] : !s.c5_alive ? ["C5 silent", "pill"]
    : st.calibrating ? ["calibrating", "pill busy"] : st.cal_needed ? ["calibration needed", "pill warn"] : ["OK", "pill ok"]; };

// Channels: R1..R8; a pass lights its tile for HIT_MS, its age taken here, since a
// pass and this page do not share a clock.
const HIT_MS = 3000;
const hits = new Map();
let seenEvents = null;
// Each tile shows the channel's level, its dB above the noise: a VTX reads the same
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
function fillChannels(s) {
  const fresh = seenEvents === null ? 0 : Math.max(0, s.total_events - seenEvents);
  seenEvents = s.total_events;
  (s.events || []).slice(0, fresh).forEach(e => hits.set(e.channel, Date.now() + HIT_MS));
  // its states only while the C5 reports (the C3 already sends a silent one's as idle)
  const st = s.stats || {}, levels = st.levels || [], states = s.c5_alive && st.states || [];
  $("chans").innerHTML = levels.map((x10, ch) => {
    const name = "R" + (ch + 1), cls = frame((hits.get(ch) || 0) > Date.now(), states[ch]);
    const title = `title="${chTitle(name, x10, states[ch])}"`;
    if (x10 <= -990) return `<div class="ch none${cls}" ${title}><span>${name}</span><b>—</b></div>`;
    const rise = x10/10;
    return `<div class="ch${cls}" ${title} style="background:rgba(46,204,113,${riseAlpha(rise).toFixed(2)})"><span>${name}</span><b>${riseTxt(rise)}</b></div>`;
  }).join("");
}

async function poll() {
  if (document.hidden) return;                  // a hidden tab asks the ESP for nothing
  try {
    const s = await (await fetch("/api/state")).json();
    fillChannels(s);
    // c5_alive: a C5 frame within 3 s; has_stats alone stays true after it goes silent
    const lk = s.c5_alive ? ["linked", "pill ok"] : s.has_stats ? ["C5 silent", "pill warn"] : ["no C5 yet", "pill"];
    $("link").textContent = lk[0]; $("link").className = lk[1];
    if (s.has_config) fillConfig(s.config);
    const det = detPill(s);
    $("s_det").textContent = det[0]; $("s_det").className = det[1];
    $("s_passes").textContent = s.total_events;
    fillHealth(s);
    $("events").innerHTML = (s.events || []).map(e =>
      `<li>${CH(e.channel)} &middot; peak ${(e.peak_db_x10/10).toFixed(1)} dB &middot; <span class="muted">${e.at_ms} ms</span></li>`
    ).join("") || '<li class="muted">none yet</li>';
  } catch (e) { $("link").textContent = "offline"; $("link").className = "pill"; }
}

$("save").addEventListener("click", async () => {
  const rearm = parseFloat($("rearm").value);
  if (!(rearm >= 0 && rearm <= 40)) { $("msg").textContent = "re-arm drop: 0 to 40 dB"; return; }   // the C5 refuses the rest too
  const body = {
    alpha_milli: Math.round(parseFloat($("alpha").value) * 1000),
    active_db_x10: Math.round(parseFloat($("active").value) * 10),
    hysteresis_db_x10: Math.round(parseFloat($("hyst").value) * 10),
    drop_db_x10: Math.round(parseFloat($("drop").value) * 10),
    min_peak_db_x10: Math.round(parseFloat($("minpeak").value) * 10),
    rearm_db_x10: Math.round(rearm * 10),
    refractory_ms: parseInt($("refr").value, 10),
    stale_ms: parseInt($("stale").value, 10),
  };
  $("msg").textContent = "saving…";
  try {
    const r = await AdminAuth.request("POST", "/api/config", JSON.stringify(body));
    if (r.status === 401) { setAdmin(false); $("msg").textContent = "password required"; return; }
    if (!r.ok) { $("msg").textContent = "failed (" + r.status + ")"; return; }
    dirty = false;
    $("msg").textContent = "saved";
    setTimeout(() => $("msg").textContent = "", 1500);
  } catch (e) { $("msg").textContent = "failed"; }
});

// Calibrate / reboot the C5 (nobody opens its USB console in production). A
// calibration blinds the gate and wants every VTX off; the C3 refuses it while
// the gate sees a strong carrier unless we force it, and refuses any command
// while no C5 is linked ({ok:false, msg}).
async function sendCommand(cmd, force) {
  const r = await AdminAuth.request("POST", "/api/command", JSON.stringify({ cmd, force }));
  if (r.status === 401) { setAdmin(false); return { ok: false, msg: "password required" }; }
  return r.ok ? r.json() : { ok: false, msg: "failed (" + r.status + ")" };
}

const CMD_DONE = { cal_full: "full calibration started", cal_quick: "quick calibration started",
                   reboot: "C5 rebooting" };
document.querySelectorAll("button[data-cmd]").forEach(b => b.addEventListener("click", async () => {
  const cmd = b.dataset.cmd;
  if (!confirm(cmd === "reboot" ? "Reboot the C5? The gate stops detecting until it is back up."
      : "All VTXs must be OFF. The gate stops detecting while it calibrates (a few seconds). Continue?"))
    return;
  $("cmd_msg").textContent = "sending…";
  try {
    let r = await sendCommand(cmd, 0);
    if (r.vtx) {
      if (!confirm(`The gate sees a strong signal (${(r.cn_x10 / 10).toFixed(1)} dB over its rest): a VTX may be on. Force the calibration?`)) {
        $("cmd_msg").textContent = "cancelled"; return;
      }
      r = await sendCommand(cmd, 1);
    }
    $("cmd_msg").textContent = r.ok ? CMD_DONE[cmd] : (r.msg || "failed");
    if (r.ok) setTimeout(() => $("cmd_msg").textContent = "", 4000);
  } catch (e) { $("cmd_msg").textContent = "failed"; }
}));

// RX gain: the C5 forces it (K g/G on its console); the same VTX guard as a calibration, since it
// learns its rests again. The field follows the C5's gain until the admin edits it.
let gainDirty = false;
$("gain_in").addEventListener("input", () => { gainDirty = true; });
async function sendGain(gain, force) {
  const r = await AdminAuth.request("POST", "/api/gain", JSON.stringify({ gain, force }));
  if (r.status === 401) { setAdmin(false); return { ok: false, msg: "password required" }; }
  return r.ok ? r.json() : { ok: false, msg: r.status === 400 ? "10 to 83" : "failed (" + r.status + ")" };
}
$("gain_set").addEventListener("click", async () => {
  const gain = Math.round(+$("gain_in").value);
  if (!(gain >= 10 && gain <= 83)) { $("gain_msg").textContent = "10 to 83"; return; }
  if (!confirm(`Set the RX gain to ${gain}? The gate stops detecting for a few seconds while it learns its rests again. All VTXs must be OFF.`)) return;
  $("gain_msg").textContent = "sending…";
  try {
    let r = await sendGain(gain, 0);
    if (r.vtx) {
      if (!confirm(`The gate sees a strong signal (${(r.cn_x10 / 10).toFixed(1)} dB over its rest): a VTX may be on. Force the change?`)) {
        $("gain_msg").textContent = "cancelled"; return;
      }
      r = await sendGain(gain, 1);
    }
    $("gain_msg").textContent = r.ok ? "sent" : (r.msg || "failed");
    if (r.ok) { gainDirty = false; setTimeout(() => $("gain_msg").textContent = "", 4000); }
  } catch (e) { $("gain_msg").textContent = "failed"; }
});

// Passage calibration: the C5 measures a drone in the passage, then 2 m away, and proposes thresholds.
let calPhase = 0, calAt = 0, calBusy = false;
const CAL_WARN = [
  [1, "No drone seen in the passage (nothing stood 3 dB over the rest): check its VTX is on, its channel, and that it is in the passage. Measure again."],
  [2, "The ADC clips in the passage: lower the gain (the gain control above), then measure again."],
  [4, "In the passage and 2 m away the drone reads almost the same (under 6 dB apart): the thresholds are a compromise. Check the antenna sits near the line, or lower the gain."],
  [8, "Hardly seen at 2 m: raise the gain, or check the drone was on and 2 m from the gate."],
  [16, "Another channel stood much higher than the one measured: another drone, or the drone is not on the channel chosen (pick Any). Measure again."],
  [32, "The C5 refused it (learning its rests, a calibration, a bench): try again in a few seconds."],
];
const dbh = x => (x / 2).toFixed(1);
async function calCall(action) {
  try {
    const channel = action === "start1" ? +$("cal_ch_sel").value : 255;
    const r = await AdminAuth.request("POST", "/api/calib", JSON.stringify({ action, channel }));
    if (r.status === 401) { setAdmin(false); return { error: "logged out" }; }
    return r.ok ? await r.json() : { error: "refused (" + r.status + ")" };
  } catch (e) { return { error: /logged out|not logged in/.test(e.message) ? "logged out" : "C3 not answering" }; }
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
  $("cal_ch").textContent = k.channel < 8 ? "R" + (k.channel + 1) : "—";
  $("cal_pass").textContent = `${dbh(k.passage[1])} · ${dbh(k.passage[3])}` + (k.passage_sat ? ` · ${k.passage_sat} % clipped` : "");
  $("cal_away").textContent = have2 ? `${dbh(k.away[1])} · ${dbh(k.away[3])}` : "—";
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
  else $("cal_msg").textContent = action === "apply" ? "applied and saved" : "";
  if (!k.error) calRender(k);
  calAt = Date.now();
}
async function calPoll() {
  if (document.hidden) return;   // a running or finished session every second, an idle one every 5 s
  if (!admin || $("home").hidden || calBusy) return;
  if (calPhase === 0 && Date.now() - calAt < 5000) return;
  const k = await calCall("state");
  if (k.error) return;
  calRender(k);
  calAt = Date.now();
}
$("cal_s1").addEventListener("click", () => calDo("start1", "starting…"));
$("cal_s2").addEventListener("click", () => calDo("start2", "starting…"));
$("cal_cancel").addEventListener("click", () => calDo("cancel"));
$("cal_apply").addEventListener("click", () => {
  if (confirm("Apply these thresholds? They replace the minimum peak, activity, hysteresis and drop (saved on the C5).")) calDo("apply", "sending…");
});
setInterval(calPoll, 1000);

// Debug: the C5's console text (POST /api/log from the position we have) and its keys.
const DBG_MAX = 60000;
let dbgNext = 0, dbgText = "", dbgPaused = false, dbgBusy = false, dbgOn = false;
function dbgShow(on) {
  dbgOn = on; $("dbg_on").setAttribute("aria-checked", on); $("dbg_body").hidden = !on;
}
async function dbgSwitch(on) {   // on: true/false to set it, undefined to ask
  const r = await AdminAuth.request("POST", "/api/debug", JSON.stringify(on === undefined ? {} : { on: on ? 1 : 0 }));
  if (r.status === 401) { setAdmin(false); return; }
  if (!r.ok) return;
  const j = await r.json();
  if (!!j.debug !== dbgOn) { dbgText = ""; dbgNext = 0; dbgRender(); }
  dbgShow(!!j.debug);
}
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
    const r = await AdminAuth.request("POST", "/api/log", JSON.stringify({ since: dbgNext }));
    if (r.status === 401) { setAdmin(false); return; }
    if (!r.ok) return;
    const j = await r.json();
    if (!j.debug) { dbgShow(false); return; }   // the C3 rebooted: off again
    if (j.next < dbgNext) dbgText += "\n[C3 restarted]\n";
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
  const r = await AdminAuth.request("POST", "/api/key", JSON.stringify({ keys, force }));
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
poll();
setInterval(poll, 1000);
setInterval(dbgPoll, 1000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) { poll(); calPoll(); dbgPoll(); } });   // back in view: catch up now
