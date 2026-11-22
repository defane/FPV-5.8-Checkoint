#!/usr/bin/env python3
"""Mock of the C6 master / C3 node web server: work on the pages without a board.

Serves the real pages from disk, read at every request: edit
firmware/<role>/main/www/index.html or components/web_auth/www/auth.js and reload
the browser, no build, no flash. /api/* answers like the firmware on a simulated
race (a healthy gate, one needing a calibration, one offline), every Raceband
channel of every gate measured as the scanning C5 does, with its detector state (a drone
now and then lingers at its gate, keeping its channel in COOLDOWN), and admin actions
take effect. Signed requests are checked as on the ESP (components/web_auth), and
a body over the firmware's buffer is refused, so a page cannot outgrow it unnoticed.
Restarting the mock is an ESP reboot: sessions are lost, the page's key stays valid.

test/test_web_mock.py keeps /api/state and the buffer sizes in step with the C code.
"""
import argparse
import hashlib
import hmac
import json
import math
import os
import random
import re
import threading
import time
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ITERATIONS = 200000       # components/web_auth/project_include.cmake
CN_UNKNOWN = -990         # LP_CN_UNKNOWN
ACTIVE_X10 = 150          # pass detector active_db: the VTX guard's threshold
NOISE_X10 = 60            # a gate's rise with no drone, about
REFRACTORY_S = 1.5        # refractory_ms: no re-arm before it
LEAVE_S = 0.9             # a drone gone this long: its level is under the release (active - hysteresis)
CAL_S = {"cal_full": 3.7, "cal_quick": 3.2}   # blind: the recalibration + the 3 s relearn
# Passage calibration (components/pass_detector/pass_calib.c, cp_calib.c): a drone's rise in the passage and
# 2 m away, half dB (p10, p50, p90, p95, max), and the thresholds pc_solve() proposes from them, dB x10
# (test_pass_calib.c checks the same numbers, so the mock cannot drift from the C).
CAL_SECS = 5.0                                 # LP_CALIB_SECS
CAL_PASSAGE, CAL_AWAY = [34, 40, 46, 48, 56], [6, 12, 18, 20, 26]
CAL_REC = {"active": 105, "hysteresis": 40, "drop": 30, "min_peak": 125}
RELEARN_S = 3.0                                # a gain change: nothing rings while the rests are learnt again
GAIN_MIN, GAIN_MAX = 10, 83                    # LP_GAIN_MIN / LP_GAIN_MAX in link_proto.h
REBOOT_S = 4.0
PASSES_KEPT = {"master": 24, "node": 16}   # C6_EVENTS, C3_EVENTS
RESULTS_KEPT = 10   # RT_HISTORY in race_timing.h
REARM_X10 = 160     # LP_REARM_DEFAULT_X10: the relative re-arm's 16 dB


class Auth:
    """components/web_auth/web_auth.c: sessions (LRU) and signed requests."""
    SESSIONS = 4

    def __init__(self, tag, password):
        self.salt = f"fpv58-checkpoint/{tag}/mock"   # fixed: K survives a restart, like a reboot
        self.open = not password
        self.key = hashlib.pbkdf2_hmac("sha256", password.encode(), self.salt.encode(), ITERATIONS, 32)
        self.sessions = OrderedDict()                # sid -> last seq, oldest first

    def session(self):
        if len(self.sessions) >= self.SESSIONS:
            self.sessions.popitem(last=False)
        sid = os.urandom(16).hex()
        self.sessions[sid] = 0
        return {"sid": sid, "salt": self.salt, "iter": ITERATIONS, "open": self.open}

    def refuse(self, headers, method, uri, body):
        """None when the request is accepted, else the 401 body."""
        if self.open:
            return None
        sid, seq, sig = (headers.get("X-Auth-" + h, "") for h in ("Session", "Seq", "Sig"))
        refused = {"ok": False, "auth": True}
        if not (re.fullmatch("[0-9a-f]{32}", sid) and re.fullmatch("[1-9][0-9]{0,9}", seq)
                and int(seq) < 2**32 and re.fullmatch("[0-9a-f]{64}", sig)):
            return refused
        if sid not in self.sessions:
            return {**refused, "session": False}
        if int(seq) <= self.sessions[sid]:
            return refused
        msg = f"{sid}\n{seq}\n{method}\n{uri}\n".encode() + body
        if not hmac.compare_digest(hmac.new(self.key, msg, hashlib.sha256).hexdigest(), sig):
            return refused
        self.sessions[sid] = int(seq)
        self.sessions.move_to_end(sid)
        return None


class Gate:
    """A simulated C5: a drone every `period` seconds, calibrations, reboots."""

    def __init__(self, gate, channel, period, vtx=False, **health):
        self.gate, self.channel, self.period, self.vtx = gate, channel, period, vtx
        self.next_pass, self.last_pass = period, -100.0
        self.hold = 0.0                              # how long the last pass's drone lingers
        self.passes = self.faults = 0
        self.cal_needed, self.gain, self.heap_min = False, 42, 61000
        self.rearm = REARM_X10                       # the re-arm drop its C5 echoes
        self.own_rearm = None                        # a D of its own (the C6 keeps it by MAC), None = the fleet's
        self.cal_until = self.down_until = self.booted = 0.0
        self.__dict__.update(health)

    def up(self, t):
        return t >= self.down_until

    def calibrating(self, t):
        return self.up(t) and t < self.cal_until

    def rise_x10(self, t):
        """The gate's drone over the noise: up to its pass, held while it lingers, then down."""
        if self.vtx:
            return 220
        near = min(abs(t - self.next_pass), max(0.0, t - self.last_pass - self.hold))
        return int(max(0.0, 220 * (1 - near / 1.2)))

    def pilot_x10(self, t):
        """A far pilot every ~19 s: over the activity threshold, under min_peak_db (no pass)."""
        return int(190 * math.sin((t + self.gate) / 6.0) ** 40)

    def cn_x10(self, t):
        if not self.up(t) or self.calibrating(t):
            return CN_UNKNOWN
        return NOISE_X10 + int(15 * math.sin(t / 3 + self.gate)) + random.randint(-5, 5) + self.rise_x10(t)

    def levels(self, t):
        """One level per Raceband channel: the gate's own, a second pilot, the noise."""
        if not self.up(t) or self.calibrating(t):
            return [CN_UNKNOWN] * 8
        out = [random.randint(-50, -10) for _ in range(8)]
        if self.channel < 8:
            out[self.channel] = self.cn_x10(t)
        other = (self.channel + 3) % 8
        out[other] = max(out[other], self.pilot_x10(t))
        return out

    def cooling(self, t):
        """The gate's own channel waits to re-arm: the refractory period, the drone still there
        (seconds more when it lingers), for good once --vtx's carrier has rung a pass."""
        return t - self.last_pass < max(REFRACTORY_S, self.hold + LEAVE_S) or (self.vtx and self.passes > 0)

    def states(self, t):
        """Each channel's pd_state_t, as the C5 reports it. The gate's drone: RISING over the
        activity threshold, PEAK_PASSED, then COOLDOWN while cooling(); the far pilot: RISING,
        then IDLE again (no pass); IDLE otherwise, and while blind."""
        out = [0] * 8
        if not self.up(t) or self.calibrating(t):
            return out
        other = (self.channel + 3) % 8
        out[other] = 1 if self.pilot_x10(t) > ACTIVE_X10 else 0
        if self.channel < 8:
            if t - self.last_pass < 0.05:
                out[self.channel] = 2
            elif self.cooling(t):
                out[self.channel] = 3
            elif NOISE_X10 + self.rise_x10(t) > ACTIVE_X10:
                out[self.channel] = 1
        return out

    def advance(self, t):
        """Passes due by t, as (time, peak) pairs: none while blind or cooling (a C5 rings no
        pass before its channel re-arms)."""
        due = []
        while t >= self.next_pass:
            at = self.next_pass
            self.next_pass += self.period + random.uniform(-0.5, 0.5)
            if self.up(at) and not self.calibrating(at) and not self.cooling(at):
                self.last_pass = at
                self.passes += 1
                # now and then the drone lingers at the gate (a crash, a pilot waiting): its
                # level stays up and its channel in COOLDOWN, the lap after it not yet counted
                self.hold = random.uniform(2.5, 4.0) if random.random() < 0.3 else 0.0
                due.append((at, 250 + random.randint(0, 60)))
        if self.cal_until and t >= self.cal_until:
            self.cal_until, self.cal_needed = 0.0, False
        return due

    def run(self, cmd, t):
        if cmd == "reboot":
            self.booted = self.down_until = t + REBOOT_S
            self.passes, self.cal_until = 0, 0.0
        else:
            self.cal_until = t + CAL_S[cmd]

    def set_gain(self, gain, t):
        """The C5 forces that gain (saved) and learns its rests again: blind for a moment."""
        self.gain = gain
        self.cal_until = t + RELEARN_S

    def health(self, t):
        return {"passes": self.passes, "cal_needed": self.cal_needed, "calibrating": self.calibrating(t),
                "gain": self.gain, "c5_uptime_s": max(0, int(t - self.booted)),
                "heap_min": self.heap_min, "faults": self.faults, "fw": "5.0.0-mock", "rearm_db_x10": self.rearm}


class ConsoleDebug:
    """Debug mode: c3_web.c / c6_web.c post_log, post_debug, post_key over the bridge's ring (link_proto's
    lp_logring, lp_keys_valid, lp_keys_blind). Every body may carry "gate" (default 0): one gate at a
    time is in debug mode (switching one on switches the previous off); the C3 has only gate 0, the C6
    gate 0 over its UART and the remote gates through their nodes (c6_hub_debug_*)."""
    LOG_RING, LOG_CHUNK = 4096, 700       # LOG_RING in c3_link.c, c6_link.c, c6_hub.c; C3_LOG_CHUNK / C6_LOG_CHUNK
    BLIND = "szxnpSgGi"                   # lp_keys_blind(): after K

    def init_debug(self):
        self.text, self.log_total, self.debug_gate = "", 0, None   # the C5's console text as the bridge keeps it; off at boot

    @property
    def debug(self):
        return self.debug_gate is not None

    def local_c5(self):
        """The Gate the page's own C5 (gate 0) is."""
        raise NotImplementedError

    def c5_of(self, gate):
        """The Gate a debug request can reach, None for a gate with no C5 (online node) behind it."""
        return self.local_c5() if gate == 0 else None

    def active_x10(self):
        """The VTX guard's threshold, the C5's active_db."""
        return ACTIVE_X10

    @staticmethod
    def gate_of(b):
        """body_gate() in c6_web.c: None out of range (the firmware's 400, or a log with no debug)."""
        g = b.get("gate", 0)
        return g if isinstance(g, int) and 0 <= g < 16 else None

    def say(self, text, gate=0):
        """A C5 prints: the bridge stores it sanitized (lp_logring_put), when that gate is the one in debug mode."""
        text = "".join(c if c == "\n" else "" if c == "\r" else "'" if c == '"' else "/" if c == "\\"
                       else c if " " <= c <= "~" else "?" for c in text)
        if self.debug_gate != gate:
            return
        self.text = (self.text + text)[-self.LOG_RING:]
        self.log_total += len(text)

    def set_debug(self, b, t):
        """post_debug(): None for its 400."""
        gate, on = self.gate_of(b), b.get("on", -1)
        if gate is None:
            return None
        ok = True
        if on > 0:
            ok = self.c5_of(gate) is not None
            if ok and self.debug_gate != gate:
                self.text, self.log_total, self.debug_gate = "", 0, gate
        elif on == 0 and self.debug_gate == gate:
            self.debug_gate = None
        return {"ok": ok, "debug": int(self.debug_gate == gate)}

    def log(self, b, t):
        """post_log(): the text after b["since"], 700 bytes at most, for b["gate"]."""
        gate, since = self.gate_of(b), b.get("since", 0)
        g = self.c5_of(gate) if gate is not None else None
        out = {"gate": -1 if gate is None else gate, "debug": 0, "next": 0, "lost": 0,
               "c5_alive": int(bool(g and g.up(t))), "text": ""}
        if gate is None or self.debug_gate != gate:
            return out
        oldest = max(0, self.log_total - self.LOG_RING)
        if since > self.log_total:
            since = oldest
        elif since < oldest:
            since, out["lost"] = oldest, 1
        chunk = self.text[since - oldest:][:self.LOG_CHUNK]
        return {**out, "debug": 1, "next": since + len(chunk), "text": chunk}

    @classmethod
    def blinds(cls, keys):
        """lp_keys_blind()."""
        in_k = False
        for k in keys:
            if in_k:
                if k == "q":
                    in_k = False
                elif k in cls.BLIND:
                    return True
            elif k == "K":
                in_k = True
            elif k in "FB":
                return True
        return False

    def key(self, b, t):
        """post_key(): None for its 400. The C5 answers on the console."""
        keys, gate = b.get("keys", ""), self.gate_of(b)
        if gate is None or not keys or len(keys) > 16 or any(not " " <= c <= "~" or c == "`" for c in keys):
            return None
        if self.debug_gate != gate:
            return {"ok": False, "msg": "debug mode is off"}
        g = self.c5_of(gate)
        if g is None or not g.up(t):
            return {"ok": False, "msg": "no C5 linked"}
        if self.blinds(keys) and not b.get("force"):
            cn = g.cn_x10(t)
            if cn > self.active_x10():
                return {"ok": False, "vtx": True, "cn_x10": cn}
            return {"ok": False, "blind": True}
        self.say(f"> {keys}\n", gate)
        if keys[0] == "K":
            self.say("CHECKPOINT K + key (lower = down, upper = up; K stays open 5 s after each key, q closes):\n", gate)
        elif keys == "T":
            self.say(f"STATUS uptime_s={int(t)} heap_internal=118000 fw={g.health(t)['fw']}\n", gate)
        elif keys in ("F", "B"):
            g.run("cal_quick" if keys == "F" else "cal_full", t)
        return {"ok": True}


class PassageCalib:
    """The passage calibration's session (cp_calib.c) as the pages' /api/calib answers it: two measurements of
    a drone, then a proposal. CAL_DEFAULT_GATE: what a body without "gate" means (the C3's pages never send one)."""
    CAL_DEFAULT_GATE = -1
    CAL_ACTIONS = ("state", "start1", "start2", "cancel", "apply", "reset")

    def init_calib(self):
        self.cals, self.applied = {}, {}                  # sessions and applied proposals, by gate

    def apply_calib(self, gate):
        self.applied[gate] = dict(CAL_REC)

    def cal_state(self, gate, t):
        """The session of a gate as c6_web.c's post_calib() answers it."""
        s = self.cals.setdefault(gate, {"phase": 0, "end": 0.0, "flags": 0})
        if s["phase"] in (1, 3) and t >= s["end"]:
            s["phase"] = 2 if s["phase"] == 1 else 4              # finish(): measured, then solved
        left = max(0, int(s["end"] - t + 0.999)) if s["phase"] in (1, 3) else 0
        ph = s["phase"]
        return {"ok": True, "msg": "", "gate": gate, "phase": ph, "secs_left": left, "flags": s["flags"],
                "channel": (s.get("want", 255) if s.get("want", 255) < 8 else (self.c5_of(gate) or self.local).channel) if ph >= 2 else 255,
                "passage": CAL_PASSAGE if ph >= 2 else [0] * 5, "passage_sat": 0,
                "away": CAL_AWAY if ph == 4 else [0] * 5,
                "rec": CAL_REC if ph == 4 else {"active": 0, "hysteresis": 0, "drop": 0, "min_peak": 0}, "gain_steps": 0}

    def calib(self, b, t):
        """post_calib() in c6_web.c / c3_web.c: None for its 400s. A remote gate goes through its node (the C6)."""
        gate, action = b.get("gate", self.CAL_DEFAULT_GATE), b.get("action")
        if not 0 <= gate < 16 or action not in self.CAL_ACTIONS:
            return None
        ok, msg = True, ""
        s = self.cals.setdefault(gate, {"phase": 0, "end": 0.0, "flags": 0})
        reach = self.c5_of(gate)
        if reach is not None and not reach.up(t):
            reach = None
        self.cal_state(gate, t)                                     # a measurement that has ended finishes first
        if action in ("start1", "start2", "apply") and reach is None:
            ok, msg = False, "no C5 linked"
        elif action in ("start1", "start2"):
            step = 1 if action == "start1" else 2
            if s["phase"] in (1, 3) or reach.calibrating(t) or (step == 2 and s["phase"] not in (2, 4)):
                s["flags"] |= 32                                    # PC_BUSY: refused, the session goes on
            else:
                if step == 1:
                    ch = b.get("channel", 255)
                    s["want"] = ch if isinstance(ch, int) and 0 <= ch < 8 else 255      # LP_CALIB_ANY
                s.update(phase=step if step == 1 else 3, end=t + CAL_SECS, flags=0)
        elif action == "cancel":
            s.update(phase=0, flags=0)
        elif action == "apply":
            if s["phase"] != 4 or reach is None:
                ok, msg = False, "nothing to apply"
            else:
                self.apply_calib(gate)
                s.update(phase=0, flags=0)
        elif action == "reset":
            if gate == 0:
                ok, msg = False, "gate 0 keeps its own settings"
            elif gate not in self.applied and reach is None:
                ok, msg = False, "no such gate"
            else:
                self.applied.pop(gate, None)
        return {**self.cal_state(gate, t), "ok": ok, "msg": msg}


class Master(PassageCalib, ConsoleDebug):
    """firmware/master/main/c6_web.c."""
    TAG = "c6"
    # POST path -> (method, char buf[N] of its C handler)
    ADMIN = {"/api/pairing": ("set_pairing", 128), "/api/order": ("order", 128),
             "/api/node_remove": ("remove_node", 128), "/api/reset": ("reset_race", 16),
             "/api/volume": ("set_volume", 128), "/api/command": ("command", 128),
             "/api/rearm": ("set_rearm", 64), "/api/gain": ("gain", 64), "/api/calib": ("calib", 64),
             "/api/log": ("log", 64), "/api/key": ("key", 96), "/api/debug": ("set_debug", 32)}

    def __init__(self, vtx=False):
        self.init_debug()
        self.vtx, self.pairing, self.volume, self.join_at = vtx, False, 60, None
        self.pairing_until = 0.0
        self.init_calib()
        self.rearm = REARM_X10                            # the fleet's re-arm drop (c6_store)
        self.local = Gate(0, 0, 7.0, vtx)
        self.nodes = []
        self.add_node(1, 1, 8.5, rssi=-52)
        self.add_node(2, 2, 11.0, rssi=-78, lost=9, cal_needed=True, faults=3, gain=61, rearm=120)  # missed a CONFIG
        self.add_node(3, 3, 13.0, rssi=-88, lost=31, online=False)
        self.reset_race()
        self.laps[7] = {"laps": 4, "last_lap_ms": 9120, "best_lap_ms": 8840, "start": None,  # a pilot who left
                        "last_gate": 2, "seen_ms": 45000,
                        "history": [self.lap_entry(n, 8840 + 93 * n) for n in range(4, 0, -1)]}

    def free_node_id(self):
        """The lowest id no paired node holds (free_node_id() in c6_hub.c: ids start at 0)."""
        return next(i for i in range(256) if all(n.node_id != i for n in self.nodes))

    def add_node(self, gate, channel, period, rssi=-60, lost=0, online=True, **health):
        n = Gate(gate, channel, period, self.vtx, **health)
        n.node_id, n.rssi, n.status_lost, n.online = self.free_node_id(), rssi, lost, online
        n.mac = f"7c:df:a1:10:20:{0x30 + n.node_id:02x}"
        self.nodes.append(n)

    def reset_race(self, b=None, t=None):
        if b and "channel" in b:                          # one channel: it stays listed
            s = self.laps.get(b["channel"])
            if s:
                s.update(laps=0, last_lap_ms=0, best_lap_ms=0, start=None, last_gate=-1, history=[])
            return {"ok": bool(s)}
        self.laps, self.events, self.total = {}, [], 0
        return {"ok": True}

    def local_c5(self):
        return self.local

    def c5_of(self, gate):
        if gate == 0:
            return self.local
        return next((n for n in self.nodes if n.gate == gate and n.online), None)   # dbg_find() in c6_hub.c

    def pass_at(self, g, at, peak):
        self.say(f"PASS channel=R{g.channel + 1} peak_db={peak / 10:.1f} at_ms={int(at * 1000)}\n", g.gate)
        self.total += 1
        self.events = [{"gate": g.gate, "channel": g.channel, "peak_db_x10": peak,
                        "at_ms": int(at * 1000)}] + self.events[:PASSES_KEPT["master"] - 1]
        s = self.laps.setdefault(g.channel, {"laps": 0, "last_lap_ms": 0, "best_lap_ms": 0, "start": None,
                                             "history": []})
        if s["start"] is not None:
            lap = int((at - s["start"]) * 1000)
            s["laps"] += 1
            s["last_lap_ms"] = lap
            s["best_lap_ms"] = min(lap, s["best_lap_ms"] or lap)
            s["history"] = [self.lap_entry(s["laps"], lap)] + s["history"][:RESULTS_KEPT - 1]
        s["start"], s["last_gate"], s["seen_ms"] = at, g.gate, int(at * 1000)

    def lap_entry(self, number, lap):
        """Gates are not simulated in order: splits spread along the lap, a gate missed now and then."""
        n = 1 + len(self.nodes)
        splits = [lap * k // n + random.randint(-150, 150) for k in range(1, n)]
        return {"number": number, "lap_ms": lap, "splits": [0 if random.random() < .06 else v for v in splits]}

    def results(self, t):
        self.advance(t)
        return {"num_gates": 1 + len(self.nodes),
                "channels": [{"channel": c, "laps": s["history"]} for c, s in sorted(self.laps.items())]}

    def advance(self, t):
        if self.join_at is not None and t >= self.join_at:   # pairing: a node shows up
            self.join_at = None
            self.add_node(len(self.nodes) + 1, len(self.nodes) + 1, 9.0)
        for g in [self.local] + self.nodes:
            due = g.advance(t)
            if g is self.local or g.online:
                for at, peak in due:
                    self.pass_at(g, at, peak)

    def state(self, t):
        self.advance(t)
        lo = self.local
        nodes = [{"node_id": n.node_id, "gate": n.gate, "channel": n.channel, "online": n.online,
                  "cn_x10": n.cn_x10(t) if n.online else CN_UNKNOWN, "states": n.states(t) if n.online else [0] * 8,
                  "last_seen_ms": int(t * 1000) - 300 if n.online else 12000, "rssi": n.rssi,
                  "rx_frames": int(t * 6), "status_rx": int(t * 2), "status_lost": n.status_lost,
                  "custom": n.gate in self.applied, "rearm_want_x10": self.want_rearm(n), "rearm_own": n.own_rearm is not None,
                  "mac": n.mac, "levels": n.levels(t) if n.online else [CN_UNKNOWN] * 8,
                  **n.health(t)} for n in self.nodes]
        left = self.pairing_left(t)                       # closes it first when its 5 minutes are over
        return {"pairing": self.pairing, "pairing_left_s": left, "num_gates": 1 + len(self.nodes), "has_local": lo.up(t),
                "local": {"cn_x10": lo.cn_x10(t), "states": lo.states(t), "levels": lo.levels(t),
                          **lo.health(t), "rearm_want_x10": self.want_rearm(lo), "rearm_own": lo.own_rearm is not None},
                "total_passes": self.total, "volume": self.volume, "rearm_db_x10": self.rearm,
                "uptime_ms": int(t * 1000),
                "free_heap": 182000 + random.randint(-2000, 2000), "heap_total": 330000, "nodes": nodes,
                "standings": [{"channel": c, "laps": s["laps"], "last_lap_ms": s["last_lap_ms"],
                               "best_lap_ms": s["best_lap_ms"], "last_gate": s["last_gate"],
                               "last_seen_ms": s["seen_ms"]}
                              for c, s in sorted(self.laps.items())],
                "passes": self.events}

    def node_by_id(self, b):
        return next((n for n in self.nodes if n.node_id == b.get("node_id")), None)

    PAIRING_OPEN_S = 300.0                            # PAIRING_OPEN_MS in c6_hub.c

    def pairing_left(self, t):
        """Seconds until an open pairing closes by itself (pairing_open() in c6_hub.c), 0 when closed."""
        if self.pairing and t >= self.pairing_until:
            self.pairing = False
        return int(self.pairing_until - t + 0.999) if self.pairing else 0

    def set_pairing(self, b, t):
        self.pairing = bool(b.get("open"))
        self.pairing_until = t + self.PAIRING_OPEN_S
        self.join_at = t + 4.0 if self.pairing else None
        return {"ok": True}

    def order(self, b, t):
        """post_order() / c6_hub_set_order(): b["order"] lists every paired node's id, first gate first; gates 1..N follow
        it. Not a permutation of the paired ids: {"ok": false}, nothing changes. A change restarts the race timing."""
        ids = b.get("order")
        if not isinstance(ids, list) or sorted(ids) != sorted(n.node_id for n in self.nodes):
            return {"ok": False}
        by_id = {n.node_id: n for n in self.nodes}
        changed = False
        debug_node = self.c5_of(self.debug_gate) if self.debug_gate else None   # a remote gate in debug mode follows its node
        for i, nid in enumerate(ids):
            if by_id[nid].gate != i + 1:
                by_id[nid].gate, changed = i + 1, True
        if changed:
            self.reset_race()
            if debug_node is not None:
                self.debug_gate = debug_node.gate
        return {"ok": True}

    def remove_node(self, b, t):
        n = self.node_by_id(b)
        if n:
            self.nodes.remove(n)
        return {"ok": bool(n)}

    def set_volume(self, b, t):
        self.volume = b.get("volume", 60)
        return {"ok": True}

    def want_rearm(self, g):
        """The D a gate should run: its own, else the fleet's (rearm_want_x10 in c6_web.c)."""
        return g.own_rearm if g.own_rearm is not None else self.rearm

    def set_rearm(self, b, t):
        """post_rearm() / c6_hub_set_rearm(_gate)(): without "gate" the fleet's D, every gate's own dropped, sent to gate 0 and every
        online node; with "gate" that gate's own D alone (-1: the fleet's again), kept by its nodes whether online or not."""
        x10, gate = b.get("rearm_db_x10"), b.get("gate", -1)
        if not isinstance(x10, int) or not isinstance(gate, int) or gate >= 16:
            return {"ok": False}
        if gate < 0:
            if not 0 <= x10 <= 400:
                return {"ok": False}
            self.rearm = x10
            for g in [self.local] + self.nodes:
                g.own_rearm = None
            for g in [self.local] + [n for n in self.nodes if n.online]:
                g.rearm = x10
            return {"ok": True}
        gates = [self.local] if gate == 0 else [n for n in self.nodes if n.gate == gate]
        if not -1 <= x10 <= 400 or not gates:
            return {"ok": False}
        for g in gates:
            g.own_rearm = None if x10 < 0 else x10
            if g is self.local or g.online:
                g.rearm = self.want_rearm(g)
        return {"ok": True}

    def targets(self, gate, t):
        """run_on_gates() in c6_web.c: {gate: [Gates behind it]} for one gate or, -1, every reachable one; None for a gate nobody paired."""
        by = {}
        if gate in (0, -1) and self.local.up(t):
            by[0] = [self.local]
        for n in self.nodes:
            if gate in (-1, n.gate) and n.online:
                by.setdefault(n.gate, []).append(n)
        if gate >= 0 and gate not in by and gate != 0 and not any(n.gate == gate for n in self.nodes):
            return None
        if gate == 0 and 0 not in by:
            by[0] = []                                    # gate 0 asked by name, its C5 absent: it lands in "failed"
        if gate > 0 and gate not in by:
            by[gate] = []                                 # a node of that gate, offline
        return by

    def gain(self, b, t):
        """post_gain() in c6_web.c: None for its 400s. gate -1: every reachable gate, the guards on all of them."""
        gate, gain, force = b.get("gate", -2), b.get("gain", -1), b.get("force", 0)
        if not (-1 <= gate < 16 and GAIN_MIN <= gain <= GAIN_MAX):
            return None
        by = self.targets(gate, t)
        if by is None:
            return None
        vtx = [{"gate": g, "cn_x10": max(x.cn_x10(t) for x in gs)} for g, gs in sorted(by.items())
               if gs and max(x.cn_x10(t) for x in gs) > ACTIVE_X10]
        if vtx and not force:
            return {"ok": False, "vtx": vtx}
        sent, failed, busy = [], [], []
        for g, gs in sorted(by.items()):
            if any(x.calibrating(t) for x in gs):
                busy.append(g)
            elif gs:
                for x in gs:
                    x.set_gain(gain, t)
                sent.append(g)
            else:
                failed.append(g)
        return {"ok": True, "sent": sent, "failed": failed, "busy": busy}

    def command(self, b, t):
        """post_command() in c6_web.c: None for its 400s."""
        gate, name, force = b.get("gate", -2), b.get("cmd"), b.get("force", 0)
        if name not in ("cal_full", "cal_quick", "reboot") or not -1 <= gate < 16:
            return None
        cn = {}
        busy, targets = set(), set()
        if self.local.up(t):
            cn[0] = self.local.cn_x10(t)
            if self.local.calibrating(t):
                busy.add(0)
        if gate == 0 or (gate == -1 and self.local.up(t)):
            targets.add(0)
        known = gate <= 0
        for n in self.nodes:
            if n.gate == gate:
                known = True
                targets.add(gate)
            if not n.online:
                continue
            if gate == -1:
                targets.add(n.gate)
            cn[n.gate] = max(cn.get(n.gate, CN_UNKNOWN), n.cn_x10(t))
            if n.calibrating(t):
                busy.add(n.gate)
        if not known:
            return None
        busy = set() if name == "reboot" else busy & targets
        targets -= busy
        if name != "reboot" and not force:
            vtx = sorted(g for g in targets if cn.get(g, CN_UNKNOWN) > ACTIVE_X10)
            if vtx:
                return {"ok": False, "vtx": [{"gate": g, "cn_x10": cn[g]} for g in vtx]}
        sent, failed = [], []
        for g in sorted(targets):
            reach = [self.local] if g == 0 and self.local.up(t) else \
                    [n for n in self.nodes if n.gate == g and n.online]
            (sent if reach else failed).append(g)
            for r in reach:
                r.run(name, t)
        return {"ok": True, "sent": sent, "failed": failed, "busy": sorted(busy)}


class Node(PassageCalib, ConsoleDebug):
    """firmware/node/main/c3_web.c, the degraded local web."""
    TAG = "c3"
    CAL_DEFAULT_GATE, CAL_ACTIONS = 0, ("state", "start1", "start2", "cancel", "apply")   # one gate, no "reset"
    ADMIN = {"/api/config": ("set_config", 512), "/api/command": ("command", 96), "/api/gain": ("gain", 64),
             "/api/calib": ("calib", 64),
             "/api/log": ("log", 64), "/api/key": ("key", 96), "/api/debug": ("set_debug", 32)}

    def apply_calib(self, gate):
        """post_calib()'s apply in c3_web.c: the four thresholds into the stored config."""
        self.config.update(active_db_x10=CAL_REC["active"], hysteresis_db_x10=CAL_REC["hysteresis"],
                           drop_db_x10=CAL_REC["drop"], min_peak_db_x10=CAL_REC["min_peak"])
        self.applied[gate] = dict(CAL_REC)

    def __init__(self, vtx=False):
        self.init_debug()
        self.init_calib()
        self.gate = Gate(0, 0, 7.0, vtx, cal_needed=True)
        self.events, self.total = [], 0
        self.config = {"alpha_milli": 300, "active_db_x10": 150, "hysteresis_db_x10": 30, "drop_db_x10": 30,
                       "min_peak_db_x10": 250, "refractory_ms": 1500, "stale_ms": 1000,
                       "rearm_db_x10": REARM_X10, "volume_pct": 60, "muted": 0}

    def local_c5(self):
        return self.gate

    def active_x10(self):
        return self.config["active_db_x10"]

    def state(self, t):
        g = self.gate
        for at, peak in g.advance(t):
            self.total += 1
            self.say(f"PASS channel=R{g.channel + 1} peak_db={peak / 10:.1f} at_ms={int(at * 1000)}\n")
            self.events = [{"channel": g.channel, "peak_db_x10": peak,
                            "at_ms": int(at * 1000)}] + self.events[:PASSES_KEPT["node"] - 1]
        h = g.health(t)
        return {"has_config": True, "config": self.config, "c5_alive": g.up(t), "has_stats": True,
                "stats": {"cn_x10": g.cn_x10(t), "states": g.states(t),
                          "flags": h["cal_needed"] | h["calibrating"] << 1, "cal_needed": h["cal_needed"],
                          "calibrating": h["calibrating"], "gain": h["gain"], "passes": h["passes"],
                          "uptime_s": h["c5_uptime_s"], "heap_min": h["heap_min"], "faults": h["faults"],
                          "fw": h["fw"], "levels": g.levels(t)},
                "total_events": self.total, "events": self.events}

    def set_config(self, b, t):
        self.config.update({k: v for k, v in b.items() if k in self.config})
        return {"ok": True}

    def gain(self, b, t):
        """post_gain() in c3_web.c: None for its 400."""
        gain, g = b.get("gain", -1), self.gate
        if not GAIN_MIN <= gain <= GAIN_MAX:
            return None
        if not g.up(t):
            return {"ok": False, "msg": "no C5 linked"}
        cn = g.cn_x10(t)
        if not b.get("force") and cn > self.config["active_db_x10"]:
            return {"ok": False, "vtx": True, "cn_x10": cn}
        g.set_gain(gain, t)
        return {"ok": True}

    def command(self, b, t):
        """post_command() in c3_web.c: None for its 400."""
        name, g = b.get("cmd"), self.gate
        if name not in ("cal_full", "cal_quick", "reboot"):
            return None
        if not g.up(t):
            return {"ok": False, "msg": "no C5 linked"}
        cn = g.cn_x10(t)
        if name != "reboot" and not b.get("force") and cn > self.config["active_db_x10"]:
            return {"ok": False, "vtx": True, "cn_x10": cn}
        g.run(name, t)
        return {"ok": True}


def make_handler(role, sim, auth, latency_s):
    page = ROOT / "firmware" / role / "main/www/index.html"
    js = ROOT / "components/web_auth/www/auth.js"
    lock, t0 = threading.Lock(), time.monotonic() - 120.0     # a race already running

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def reply(self, status, data, ctype="application/json"):
            if isinstance(data, (dict, list)):
                data = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")     # a reload always shows the file on disk
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self.drain()
            if self.path == "/":
                self.reply(200, page.read_bytes(), "text/html")
            elif self.path == "/auth.js":
                self.reply(200, js.read_bytes(), "application/javascript")
            elif self.path == "/api/state":
                with lock:
                    self.reply(200, sim.state(time.monotonic() - t0))
            elif self.path == "/api/results" and hasattr(sim, "results"):
                with lock:
                    self.reply(200, sim.results(time.monotonic() - t0))
            elif self.path == "/api/auth":
                self.admin(8, lambda b, t: {"admin": True})
            else:
                self.reply(404, b"not found", "text/plain")

        def do_POST(self):
            body = self.drain()
            if self.path == "/api/session":
                with lock:
                    self.reply(200, auth.session())
            elif self.path in sim.ADMIN:
                name, cap = sim.ADMIN[self.path]
                self.admin(cap, getattr(sim, name), body)
            else:
                self.reply(404, b"not found", "text/plain")

        def drain(self):
            time.sleep(latency_s)
            return self.rfile.read(int(self.headers.get("Content-Length") or 0))

        def admin(self, cap, handler, body=b""):
            """web_auth_request(): the firmware's buffer, the signature, then the handler."""
            if len(body) >= cap:
                return self.reply(400, f"bad length: {len(body)} >= buf[{cap}]".encode(), "text/plain")
            with lock:
                refused = auth.refuse(self.headers, self.command, self.path, body)
                if refused:
                    return self.reply(401, refused)
                t = time.monotonic() - t0
                sim.state(t)
                try:
                    args = json.loads(body)
                except ValueError:
                    args = {}                                 # json_i()/json_s(): defaults, no error
                out = handler(args, t)
            self.reply(*(200, out) if out is not None else (400, b"bad request", "text/plain"))

        def log_message(self, fmt, *args):
            if "/api/state" not in str(args[0]):               # the page polls it every second
                print(time.strftime("%H:%M:%S"), fmt % args, flush=True)

    return Handler


def main():
    p = argparse.ArgumentParser(description="Mock of the C6/C3 web server (pages read from disk at every request).")
    p.add_argument("role", nargs="?", choices=("master", "node"), default="master")
    p.add_argument("--port", type=int, help="default: 8080 for the master, 8081 for the node")
    p.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to open it from a phone on the LAN")
    p.add_argument("--password", default="checkpoint", help='admin password; "" disables admin auth, as on the firmware')
    p.add_argument("--latency", type=int, default=0, metavar="MS", help="delay every request, as over Wi-Fi")
    p.add_argument("--vtx", action="store_true", help="a strong carrier on every gate: calibrations ask to force, "
                   "its channel stays in cooldown after one pass (no lap counted)")
    a = p.parse_args()

    sim = (Master if a.role == "master" else Node)(a.vtx)
    port = a.port or (8080 if a.role == "master" else 8081)
    handler = make_handler(a.role, sim, Auth(sim.TAG, a.password), a.latency / 1000)
    with ThreadingHTTPServer((a.host, port), handler) as server:
        print(f"{a.role} mock on http://{a.host}:{port}/ (admin password: {a.password!r}); edit the page, reload",
              flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
