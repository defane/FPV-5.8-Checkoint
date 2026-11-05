#!/usr/bin/env python3
"""Mock of the C6 master / C3 node web server: work on the pages without a board.

Serves the real pages from disk, read at every request: edit
firmware/<role>/main/www/index.html or components/web_auth/www/auth.js and reload
the browser, no build, no flash. /api/* answers like the firmware on a simulated
race (a healthy gate, one needing a calibration, one offline), and admin actions
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
CAL_S = {"cal_full": 8.0, "cal_quick": 3.0}
REBOOT_S = 4.0
PASSES_KEPT = {"master": 24, "node": 16}   # C6_EVENTS, C3_EVENTS


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
        self.passes = self.faults = 0
        self.cal_needed, self.gain, self.heap_min = False, 42, 61000
        self.cal_until = self.down_until = self.booted = 0.0
        self.__dict__.update(health)

    def up(self, t):
        return t >= self.down_until

    def calibrating(self, t):
        return self.up(t) and t < self.cal_until

    def cn_x10(self, t):
        if not self.up(t) or self.calibrating(t):
            return CN_UNKNOWN
        noise = 60 + int(15 * math.sin(t / 3 + self.gate)) + random.randint(-5, 5)
        near = min(abs(t - self.next_pass), abs(t - self.last_pass))
        return noise + (220 if self.vtx else int(max(0.0, 220 * (1 - near / 1.2))))

    def state(self, t):                              # pd_state_t
        if self.next_pass - t < 1.2:
            return 1
        since = t - self.last_pass
        return 2 if since < 0.4 else 3 if since < 1.2 else 0

    def advance(self, t):
        """Passes due by t, as (time, peak) pairs."""
        due = []
        while t >= self.next_pass:
            at = self.next_pass
            self.next_pass += self.period + random.uniform(-0.5, 0.5)
            if self.up(at) and not self.calibrating(at):
                self.last_pass = at
                self.passes += 1
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

    def health(self, t):
        return {"passes": self.passes, "cal_needed": self.cal_needed, "calibrating": self.calibrating(t),
                "gain": self.gain, "c5_uptime_s": max(0, int(t - self.booted)),
                "heap_min": self.heap_min, "faults": self.faults, "fw": "5.0.0-mock"}


class Master:
    """firmware/master/main/c6_web.c."""
    TAG = "c6"
    # POST path -> (method, char buf[N] of its C handler)
    ADMIN = {"/api/pairing": ("set_pairing", 128), "/api/node": ("set_node", 128),
             "/api/node_remove": ("remove_node", 128), "/api/reset": ("reset_race", 16),
             "/api/volume": ("set_volume", 128), "/api/command": ("command", 128)}

    def __init__(self, vtx=False):
        self.vtx, self.pairing, self.volume, self.join_at = vtx, False, 60, None
        self.local = Gate(0, 0, 7.0, vtx)
        self.nodes = []
        self.add_node(1, 1, 8.5, rssi=-52)
        self.add_node(2, 2, 11.0, rssi=-78, lost=9, cal_needed=True, faults=3, gain=61)
        self.add_node(3, 3, 13.0, rssi=-88, lost=31, online=False)
        self.reset_race()

    def add_node(self, gate, channel, period, rssi=-60, lost=0, online=True, **health):
        n = Gate(gate, channel, period, self.vtx, **health)
        n.node_id, n.rssi, n.status_lost, n.online = len(self.nodes) + 1, rssi, lost, online
        n.mac = f"7c:df:a1:10:20:{0x30 + n.node_id:02x}"
        self.nodes.append(n)

    def reset_race(self, b=None, t=None):
        self.laps, self.events, self.total = {}, [], 0
        return {"ok": True}

    def pass_at(self, g, at, peak):
        self.total += 1
        self.events = [{"gate": g.gate, "channel": g.channel, "peak_db_x10": peak,
                        "at_ms": int(at * 1000)}] + self.events[:PASSES_KEPT["master"] - 1]
        s = self.laps.setdefault(g.channel, {"laps": 0, "last_lap_ms": 0, "best_lap_ms": 0, "start": None})
        if s["start"] is not None:
            lap = int((at - s["start"]) * 1000)
            s["laps"] += 1
            s["last_lap_ms"] = lap
            s["best_lap_ms"] = min(lap, s["best_lap_ms"] or lap)
        s["start"], s["last_gate"] = at, g.gate

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
                  "cn_x10": n.cn_x10(t) if n.online else CN_UNKNOWN, "state": n.state(t) if n.online else 0,
                  "last_seen_ms": int(t * 1000) - 300 if n.online else 12000, "rssi": n.rssi,
                  "rx_frames": int(t * 6), "status_rx": int(t * 2), "status_lost": n.status_lost,
                  "mac": n.mac, **n.health(t)} for n in self.nodes]
        return {"pairing": self.pairing, "num_gates": 1 + len(self.nodes), "has_local": lo.up(t),
                "local": {"channel": lo.channel, "cn_x10": lo.cn_x10(t), "state": lo.state(t), **lo.health(t)},
                "total_passes": self.total, "volume": self.volume, "uptime_ms": int(t * 1000),
                "free_heap": 182000 + random.randint(-2000, 2000), "heap_total": 330000, "nodes": nodes,
                "standings": [{"channel": c, "laps": s["laps"], "last_lap_ms": s["last_lap_ms"],
                               "best_lap_ms": s["best_lap_ms"], "last_gate": s["last_gate"]}
                              for c, s in sorted(self.laps.items())],
                "passes": self.events}

    def node_by_id(self, b):
        return next((n for n in self.nodes if n.node_id == b.get("node_id")), None)

    def set_pairing(self, b, t):
        self.pairing = bool(b.get("open"))
        self.join_at = t + 4.0 if self.pairing else None
        return {"ok": True}

    def set_node(self, b, t):
        n = self.node_by_id(b)
        if n:
            n.gate, n.channel = b.get("gate", 0), b.get("channel", 0)
        return {"ok": bool(n)}

    def remove_node(self, b, t):
        n = self.node_by_id(b)
        if n:
            self.nodes.remove(n)
        return {"ok": bool(n)}

    def set_volume(self, b, t):
        self.volume = b.get("volume", 60)
        return {"ok": True}

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


class Node:
    """firmware/node/main/c3_web.c, the degraded local web."""
    TAG = "c3"
    ADMIN = {"/api/config": ("set_config", 512), "/api/command": ("command", 96)}

    def __init__(self, vtx=False):
        self.gate = Gate(0, 0, 7.0, vtx, cal_needed=True)
        self.events, self.total = [], 0
        self.config = {"alpha_milli": 300, "active_db_x10": 150, "hysteresis_db_x10": 30, "drop_db_x10": 30,
                       "min_peak_db_x10": 250, "refractory_ms": 1500, "stale_ms": 1000,
                       "volume_pct": 60, "muted": 0}

    def state(self, t):
        g = self.gate
        for at, peak in g.advance(t):
            self.total += 1
            self.events = [{"channel": g.channel, "peak_db_x10": peak,
                            "at_ms": int(at * 1000)}] + self.events[:PASSES_KEPT["node"] - 1]
        h = g.health(t)
        return {"has_config": True, "config": self.config, "c5_alive": g.up(t), "has_stats": True,
                "stats": {"cn_x10": g.cn_x10(t), "channel": g.channel, "state": g.state(t),
                          "flags": h["cal_needed"] | h["calibrating"] << 1, "cal_needed": h["cal_needed"],
                          "calibrating": h["calibrating"], "gain": h["gain"], "passes": h["passes"],
                          "uptime_s": h["c5_uptime_s"], "heap_min": h["heap_min"], "faults": h["faults"],
                          "fw": h["fw"]},
                "total_events": self.total, "events": self.events}

    def set_config(self, b, t):
        self.config.update({k: v for k, v in b.items() if k in self.config})
        if b.get("channel", -1) >= 0:
            self.gate.channel = b["channel"]
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
    p.add_argument("--vtx", action="store_true", help="a strong carrier on every gate: calibrations ask to force")
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
