#!/usr/bin/env python3
"""tools/web_mock.py stays in step with the firmware: /api/state fields and POST body
buffers read from c6_web.c / c3_web.c, then the signed protocol over real HTTP."""
import hashlib
import hmac
import http.client
import importlib.util
import inspect
import json
import re
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("web_mock", ROOT / "tools/web_mock.py")
web_mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web_mock)

def page_text(rel):
    """A page with its script: the code lives in app.js beside the HTML (the firmware's CSP allows no inline script)."""
    p = ROOT / rel
    return p.read_text() + p.with_name("app.js").read_text()


SOURCES = {web_mock.Master: "firmware/master/main/c6_web.c", web_mock.Node: "firmware/node/main/c3_web.c"}


def c_function(src, name):
    m = re.search(rf"^static \w+ (?:\w+ )?{name}\(.*?^\}}", src, re.S | re.M)
    return m.group(0) if m else ""


def state_source(src):
    """get_state and the put_* helpers it calls (the health, the levels...)."""
    fn = c_function(src, "get_state")
    return fn + "".join(c_function(src, h) for h in sorted(set(re.findall(r"\b(put_\w+)\(", fn))))


def json_keys(value):
    if isinstance(value, dict):
        return set(value) | {k for v in value.values() for k in json_keys(v)}
    if isinstance(value, list):
        return {k for v in value for k in json_keys(v)}
    return set()


def top_level_keys(c):
    """The keys of the outer object a C handler writes: its format strings in source order (one
    pass of each loop), depth from their braces and brackets. json_keys() flattens, so a field
    moved into, out of or copied into a nested object (put_health's rearm_db_x10) still passes it."""
    c = re.sub(r"/\*.*?\*/|//[^\n]*", "", c, flags=re.S)
    text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', c))
    keys, depth = set(), 0
    for m in re.finditer(r'\\"([a-z0-9_]+)\\":|[{}\[\]]', text):
        if m.group(1):
            if depth == 1:
                keys.add(m.group(1))
        else:
            depth += 1 if m.group(0) in "{[" else -1
    return keys


def page_posts(page):
    """Each post("/api/x", ...) of a page: its path and the keys of the object literals it sends."""
    out = {}
    for m in re.finditer(r'\bpost\("(/api/\w+)",', page):
        i, depth = m.end(), 1
        while depth:                              # to the call's closing parenthesis
            depth += {"(": 1, ")": -1}.get(page[i], 0)
            i += 1
        keys = set()
        for obj in re.findall(r"\{([^{}]*)\}", page[m.end():i - 1]):
            keys |= set(re.findall(r"(?:^|,)\s*(\w+)\s*(?=[:,]|$)", obj.strip()))
        out.setdefault(m.group(1), set()).update(keys)
    return out


def c_reads(fn, src=""):
    """The body keys a C handler parses (json_i/json_s/json_ints), with the helpers it hands buf to (body_gate(buf))."""
    reads = r'json_(?:[is]|ints)\(buf, "(\w+)"'
    keys = set(re.findall(reads, fn))
    for helper in set(re.findall(r"\b(\w+)\(buf\)", fn)):
        keys |= set(re.findall(reads, c_function(src, helper)))
    return keys


def mock_reads(cls, method):
    """The body keys a mock handler reads (b.get, b[...]), with the helpers it hands the body to."""
    src = inspect.getsource(getattr(cls, method))
    keys = set(re.findall(r'\bb(?:\.get\(|\[)"(\w+)"', src))
    for helper in set(re.findall(r"self\.(\w+)\(b\b", src)):
        keys |= mock_reads(cls, helper)
    return keys


class Firmware(unittest.TestCase):
    def test_state_fields_match_the_c_code(self):
        for cls, path in SOURCES.items():
            want = set(re.findall(r'\\"([a-z0-9_]+)\\":', state_source((ROOT / path).read_text())))
            sim = cls()
            sim.state(60.0)                       # past a few passes: every list is filled
            self.assertTrue(want)
            self.assertEqual(json_keys(sim.state(61.0)), want, path)

    def test_top_level_state_fields_match_the_c_code(self):
        for cls, path in SOURCES.items():
            want = top_level_keys(c_function((ROOT / path).read_text(), "get_state"))
            self.assertIn("rearm_db_x10" if cls is web_mock.Master else "config", want)
            self.assertEqual(set(cls().state(61.0)), want, path)

    def test_results_fields_match_the_c_code(self):
        src = (ROOT / SOURCES[web_mock.Master]).read_text()
        want = set(re.findall(r'\\"([a-z0-9_]+)\\":', c_function(src, "get_results")))
        sim = web_mock.Master()
        self.assertTrue(want)
        self.assertEqual(json_keys(sim.results(60.0)), want)
        header = (ROOT / "components/race_timing/include/race_timing.h").read_text()
        self.assertEqual(web_mock.RESULTS_KEPT, int(re.search(r"RT_HISTORY\s+(\d+)u", header).group(1)))

    def test_node_page_posts_what_the_firmware_reads(self):
        """The C3 page's detector body: every key parsed by post_config (json_i) and echoed in
        /api/state's config, and its widest form within the firmware's buffer."""
        page = page_text("firmware/node/main/www/index.html")
        body = re.search(r'\$\("save"\).*?const body = \{(.*?)\};', page, re.S).group(1)
        keys = re.findall(r"^\s*(\w+):", body, re.M)
        src = (ROOT / SOURCES[web_mock.Node]).read_text()
        parsed = set(re.findall(r'json_i\(buf, "(\w+)"', c_function(src, "post_config")))
        self.assertIn("rearm_db_x10", keys)
        self.assertLessEqual(set(keys), parsed, "a field the firmware would not read")
        self.assertLessEqual(set(keys), set(web_mock.Node().config), "a field /api/state does not echo")
        widest = "-1.7976931348623157e+308"          # the longest number JSON.stringify writes
        longest = "{" + ",".join(f'"{k}":{widest}' for k in keys) + "}"
        self.assertLess(len(longest), web_mock.Node.ADMIN["/api/config"][1], "web_auth_request needs len < buf")

    def test_master_page_posts_what_the_firmware_reads(self):
        """Every admin POST of the C6 page: a route of c6_web.c and of the mock, each key it sends
        parsed by the C handler (json_i/json_s) and read by the mock; the re-arm body within its
        buffer."""
        page = page_text("firmware/master/main/www/index.html")
        src = (ROOT / SOURCES[web_mock.Master]).read_text()
        routes = dict(re.findall(r'\.uri = "(/api/\w+)",\s*\.method = HTTP_POST,\s*\.handler = (\w+)', src))
        posts = page_posts(page)
        self.assertEqual(posts.get("/api/rearm"), {"rearm_db_x10", "gate"})
        for uri, keys in posts.items():
            self.assertIn(uri, routes, "a POST the firmware does not route")
            self.assertIn(uri, web_mock.Master.ADMIN, "a POST the mock does not serve")
            self.assertLessEqual(keys, c_reads(c_function(src, routes[uri])), f"{uri}: a field the firmware would not read")
            self.assertLessEqual(keys, mock_reads(web_mock.Master, web_mock.Master.ADMIN[uri][0]),
                                 f"{uri}: a field the mock would not read")
        widest = '{"rearm_db_x10":-1.7976931348623157e+308}'
        self.assertLess(len(widest), web_mock.Master.ADMIN["/api/rearm"][1], "web_auth_request needs len < buf")

    def test_rearm_default_matches_link_proto(self):
        header = (ROOT / "components/link_proto/include/link_proto.h").read_text()
        self.assertEqual(web_mock.REARM_X10, int(re.search(r"LP_REARM_DEFAULT_X10\s+(\d+)", header).group(1)))

    def test_admin_routes_and_buffers_match_the_c_code(self):
        for cls, path in SOURCES.items():
            src = (ROOT / path).read_text()
            routes = re.findall(r'\.uri = "(/api/\w+)",\s*\.method = HTTP_POST,\s*\.handler = (\w+)', src)
            buffers = {uri: int(re.search(r"char buf\[(\d+)\]", c_function(src, h)).group(1)) for uri, h in routes}
            self.assertEqual({uri: cap for uri, (_, cap) in cls.ADMIN.items()}, buffers, path)


class Simulation(unittest.TestCase):
    PD = range(4)                                 # pd_state_t: IDLE, RISING, PEAK_PASSED, COOLDOWN

    def runs(self, seq, value):
        """The lengths of the runs of `value` in seq."""
        out, n = [], 0
        for v in seq + [None]:
            if v == value:
                n += 1
            elif n:
                out, n = out + [n], 0
        return out

    def test_channel_states(self):
        """The 8 states of every gate: the pages draw a cooldown frame from them, so the gate's
        own channel rises on an approach and cools for seconds after each pass, longer when the
        drone lingers; a far pilot rises and never cools; an offline gate reports none."""
        web_mock.random.seed(5)
        sim, step = web_mock.Master(), 0.1
        own, pilot = [], []
        for i in range(1200):                         # 2 minutes of race
            st = sim.state(60 + i * step)
            for g in [st["local"]] + st["nodes"]:
                self.assertEqual(len(g["states"]), 8)
                self.assertTrue(set(g["states"]) <= set(self.PD))
            self.assertEqual(st["nodes"][2]["states"], [0] * 8, "gate 3 is offline")
            own.append(st["local"]["states"][0])
            pilot.append(st["local"]["states"][3])
        idle = [i for i, v in enumerate(own) if v == 0]
        cool = self.runs(own[idle[0]:idle[-1]], 3)      # whole cooldowns only
        self.assertGreaterEqual(len(cool), 10, "a cooldown after each pass")
        self.assertGreaterEqual(min(cool) * step, web_mock.REFRACTORY_S - 0.2)
        self.assertGreaterEqual(max(cool) * step, 3.0, "a drone that lingers")
        self.assertTrue(self.runs(own, 1), "rising on an approach")
        self.assertTrue(self.runs(pilot, 1) and not self.runs(pilot, 3), "the far pilot never passes")

    def test_states_replace_the_strongest_channel(self):
        """v4's "states" took the place of the strongest channel and its state: in the C3's
        stats and the C6's local; a C6 node keeps its assigned channel."""
        for gate in (web_mock.Node().state(61.0)["stats"], web_mock.Master().state(61.0)["local"]):
            self.assertEqual(len(gate["states"]), 8)
            self.assertFalse({"state", "channel"} & set(gate))
        node = web_mock.Master().state(61.0)["nodes"][0]
        self.assertIn("channel", node)
        self.assertNotIn("state", node)


class Protocol(unittest.TestCase):
    def setUp(self):
        self.sim = web_mock.Master()
        self.auth = web_mock.Auth("c6", "pw")
        handler = web_mock.make_handler("master", self.sim, self.auth, 0)
        handler.log_message = lambda *a: None
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.seq = 0

    def call(self, method, path, body=b"", headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1])
        c.request(method, path, body, headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data

    def signed(self, sid, seq, method, path, body=b"", key=None):
        msg = f"{sid}\n{seq}\n{method}\n{path}\n".encode() + body
        sig = hmac.new(key or self.key, msg, hashlib.sha256).hexdigest()
        return self.call(method, path, body, {"X-Auth-Session": sid, "X-Auth-Seq": str(seq), "X-Auth-Sig": sig})

    def open(self):
        s = json.loads(self.call("POST", "/api/session")[1])
        self.key = hashlib.pbkdf2_hmac("sha256", b"pw", s["salt"].encode(), s["iter"], 32)
        return s["sid"]

    def test_pages_are_read_from_disk(self):
        status, page = self.call("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b'<script src="/auth.js">', page)
        self.assertIn(b"const AdminAuth", self.call("GET", "/auth.js")[1])

    def test_signed_requests(self):
        sid = self.open()
        self.assertEqual(self.signed(sid, 1, "GET", "/api/auth")[0], 200)
        self.assertEqual(self.signed(sid, 2, "POST", "/api/volume", b'{"volume":35}')[0], 200)
        self.assertEqual(self.sim.volume, 35)
        self.assertEqual(self.signed(sid, 2, "POST", "/api/volume", b'{"volume":99}')[0], 401, "replay")
        self.assertEqual(self.sim.volume, 35)
        self.assertEqual(self.signed(sid, 3, "POST", "/api/volume", b'{"volume":99}', b"x" * 32)[0], 401, "wrong key")
        status, data = self.signed("0" * 32, 1, "GET", "/api/auth")
        self.assertEqual((status, json.loads(data)["session"]), (401, False))
        self.assertEqual(self.call("POST", "/api/reset", b"{}")[0], 401, "unsigned")

    def test_debug_routes_over_http(self):
        """Signed /api/debug, /api/key and /api/log reach the mock's handlers (not only the methods)."""
        sid = self.open()
        self.assertEqual(self.call("POST", "/api/debug", b'{"on":1}')[0], 401, "unsigned")
        self.assertEqual(json.loads(self.signed(sid, 1, "POST", "/api/debug", b"{}")[1])["debug"], 0, "off at boot")
        self.assertEqual(json.loads(self.signed(sid, 2, "POST", "/api/debug", b'{"on":1}')[1])["debug"], 1)
        self.sim.state(60.0)
        self.assertEqual(json.loads(self.signed(sid, 3, "POST", "/api/key", b'{"keys":"T","force":0}')[1]), {"ok": True})
        out = json.loads(self.signed(sid, 4, "POST", "/api/log", b'{"since":0}')[1])
        self.assertEqual((out["debug"], out["c5_alive"]), (1, 1))
        self.assertIn("STATUS", out["text"])
        self.assertEqual(self.signed(sid, 5, "POST", "/api/key", b'{"keys":"`"}')[0], 400)

    def test_pairing_is_closed_at_boot_and_closes_by_itself(self):
        """c6_hub.c: closed at every boot, opened for PAIRING_OPEN_MS (5 min), again to close it now."""
        hub = (ROOT / "firmware/master/main/c6_hub.c").read_text()
        self.assertRegex(hub, r"static bool\s+s_pairing;")
        minutes = re.search(r"PAIRING_OPEN_MS \((\d+)u \* 60u \* 1000u\)", hub)
        self.assertEqual(float(minutes.group(1)) * 60, web_mock.Master.PAIRING_OPEN_S)
        sim = web_mock.Master()
        self.assertEqual((sim.state(10.0)["pairing"], sim.state(10.0)["pairing_left_s"]), (False, 0))
        sim.set_pairing({"open": 1}, 100.0)
        st = sim.state(101.0)
        self.assertEqual((st["pairing"], st["pairing_left_s"]), (True, 299))
        self.assertTrue(sim.state(399.0)["pairing"])
        st = sim.state(400.0)
        self.assertEqual((st["pairing"], st["pairing_left_s"]), (False, 0), "closed by itself after 5 minutes")
        sim.set_pairing({"open": 1}, 500.0)
        sim.set_pairing({"open": 0}, 510.0)
        self.assertFalse(sim.state(511.0)["pairing"], "the toggle closes it at once")

    def test_reset_of_one_channel(self):
        status, data = self.call("GET", "/api/results")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["channels"])
        self.sim.state(60.0)
        sid = self.open()
        self.assertEqual(self.signed(sid, 1, "POST", "/api/reset", b'{"channel":0}')[0], 200)
        by = {c["channel"]: c["laps"] for c in self.sim.results(60.0)["channels"]}
        self.assertEqual(by[0], [])
        self.assertTrue(by[7], "the other channels keep their laps")
        self.assertEqual(self.signed(sid, 2, "POST", "/api/reset", b'{"channel":5}')[1], b'{"ok": false}')
        self.assertEqual(self.signed(sid, 3, "POST", "/api/reset", b"{}")[0], 200)
        self.assertEqual(self.sim.results(60.0)["channels"], [])

    def test_fleet_rearm(self):
        sid = self.open()
        st = self.sim.state(60.0)
        self.assertEqual(st["rearm_db_x10"], web_mock.REARM_X10)
        self.assertEqual([n["rearm_db_x10"] for n in st["nodes"]], [160, 120, 160], "gate 2 missed a CONFIG")
        self.assertEqual(self.signed(sid, 1, "POST", "/api/rearm", b'{"rearm_db_x10":130}')[1], b'{"ok": true}')
        st = self.sim.state(60.0)
        self.assertEqual((st["rearm_db_x10"], st["local"]["rearm_db_x10"]), (130, 130))
        self.assertEqual([n["rearm_db_x10"] for n in st["nodes"]], [130, 130, 160], "an offline gate takes it when back")
        for seq, bad in enumerate((b'{"rearm_db_x10":401}', b'{"rearm_db_x10":-1}', b"{}"), 2):
            self.assertEqual(self.signed(sid, seq, "POST", "/api/rearm", bad)[1], b'{"ok": false}', bad)
        self.assertEqual(self.sim.rearm, 130)
        self.assertEqual(self.call("POST", "/api/rearm", b'{"rearm_db_x10":0}')[0], 401, "unsigned")

    def test_body_over_the_firmware_buffer_is_refused(self):
        sid = self.open()
        self.assertEqual(self.signed(sid, 1, "POST", "/api/reset", b" " * 15)[0], 200)
        self.assertEqual(self.signed(sid, 2, "POST", "/api/reset", b" " * 16)[0], 400)


class DebugCases:
    """Debug mode of a page with a local C5: /api/log, /api/debug and /api/key against c3_web.c's and
    c6_web.c's rules (the same on both: link_proto's lp_logring, lp_keys_valid, lp_keys_blind)."""

    def setUp(self):
        self.sim = self.cls()

    def test_off_by_default(self):
        self.sim.state(60.0)
        self.assertEqual(self.sim.log({"since": 0}, 60.0)["debug"], 0)
        self.assertEqual(self.sim.log({"since": 0}, 60.0)["text"], "", "nothing is kept while off")
        self.assertEqual(self.sim.key({"keys": "T"}, 60.0), {"ok": False, "msg": "debug mode is off"})
        self.assertEqual(self.sim.set_debug({}, 60.0)["debug"], 0, "no \"on\" only asks")
        self.assertEqual(self.sim.set_debug({"on": 1}, 60.0)["debug"], 1)
        self.assertEqual(self.sim.key({"keys": "T"}, 60.0), {"ok": True})
        self.assertEqual(self.sim.set_debug({"on": 0}, 60.0)["debug"], 0)

    def test_log_pages_through_the_ring(self):
        self.sim.set_debug({"on": 1}, 1.0)
        self.sim.say('PASS "x" \\ \r\x01\n')
        out = self.sim.log({"since": 0}, 1.0)
        self.assertEqual(out["text"], "PASS 'x' / ?\n")
        self.assertEqual(self.sim.log({"since": out["next"]}, 1.0)["text"], "")
        self.sim.say("a" * 5000)                       # more than the ring keeps
        out = self.sim.log({"since": 0}, 1.0)
        self.assertEqual((out["lost"], len(out["text"])), (1, self.cls.LOG_CHUNK))
        self.assertEqual(self.sim.log({"since": 10 ** 9}, 1.0)["lost"], 0, "a reboot restarts at the oldest")

    def quiet(self, t):
        """No drone near the gate: its C/N stays under the VTX guard, whenever the test runs."""
        g = self.sim.local_c5()
        g.next_pass, g.last_pass = t + 100.0, t - 100.0

    def test_keys(self):
        t = 60.0
        self.sim.state(t)
        self.quiet(t)
        self.sim.set_debug({"on": 1}, t)
        self.assertIsNone(self.sim.key({"keys": "`"}, t), "the download key never goes out")
        self.assertIsNone(self.sim.key({"keys": ""}, t))
        self.assertIsNone(self.sim.key({"keys": "K" * 17}, t))
        self.assertEqual(self.sim.key({"keys": "Ka"}, t), {"ok": True})
        self.assertEqual(self.sim.key({"keys": "T"}, t), {"ok": True})
        self.assertTrue(self.sim.key({"keys": "F"}, t).get("blind"), "F blinds the gate")
        self.assertEqual(self.sim.key({"keys": "F", "force": 1}, t), {"ok": True})
        self.assertTrue(self.sim.key({"keys": "Ks"}, t).get("blind"), "a bench blinds the gate")
        self.assertEqual(self.sim.key({"keys": "Kaq", "force": 0}, t), {"ok": True})
        self.assertTrue(self.sim.blinds("Kqi") is False and self.sim.blinds("Kq" "F"))

    def test_a_carrier_asks_before_blinding(self):
        sim = self.cls(vtx=True)
        t = 60.0
        sim.state(t)
        sim.set_debug({"on": 1}, t)
        out = sim.key({"keys": "B"}, t)
        self.assertTrue(out.get("vtx") and out["cn_x10"] > web_mock.ACTIVE_X10, out)
        self.assertEqual(sim.key({"keys": "T"}, t), {"ok": True}, "a status blinds nothing")

    def test_page_sends_what_the_firmware_reads(self):
        """The debug panel's bodies: each key the page sends is parsed by the C handler and read by the mock,
        the C handler's buffer holds the widest one, and every route is the mock's."""
        page = page_text(self.page)
        src = (ROOT / SOURCES[self.cls]).read_text()
        routes = dict(re.findall(r'\.uri = "(/api/\w+)",\s*\.method = HTTP_POST,\s*\.handler = (\w+)', src))
        sent = {}
        for uri, args in re.findall(r'AdminAuth\.request\("POST", "(/api/(?:log|key|debug))", (.*?)\);?\n', page):
            for obj in re.findall(r"\{([^{}]*)\}", args):
                sent.setdefault(uri, set()).update(re.findall(r"(?:^|,)\s*(\w+)\s*(?=[:,]|$)", obj.strip()))
        self.assertEqual(sent, {"/api/log": {"since"} | self.gate_key, "/api/debug": {"on"} | self.gate_key,
                                "/api/key": {"keys", "force"} | self.gate_key})
        for uri, keys in sent.items():
            self.assertIn(uri, routes, "a POST the firmware does not route")
            self.assertLessEqual(keys, c_reads(c_function(src, routes[uri]), src), f"{uri}: a field the firmware would not read")
            self.assertLessEqual(keys, mock_reads(self.cls, self.cls.ADMIN[uri][0]), f"{uri}: a field the mock would not read")
        widest = '{"keys":"' + "\\\\" * 16 + '","force":1' + (',"gate":15' if self.gate_key else "") + "}"   # 16 keys, each escaped by JSON.stringify
        self.assertLess(len(widest), self.cls.ADMIN["/api/key"][1])

    def test_every_route_calls_a_method(self):
        for uri, (name, cap) in self.cls.ADMIN.items():
            self.assertTrue(callable(getattr(self.cls, name, None)), f"{uri}: {name} is not a handler")


class NodeDebug(DebugCases, unittest.TestCase):
    cls, page, gate_key = web_mock.Node, "firmware/node/main/www/index.html", set()


class MasterDebug(DebugCases, unittest.TestCase):
    cls, page, gate_key = web_mock.Master, "firmware/master/main/www/index.html", {"gate"}

    def test_a_remote_gate_is_debugged_through_its_node(self):
        t = 60.0
        self.sim.state(t)
        node = self.sim.nodes[0]                                  # gate 1, online
        self.assertEqual(self.sim.set_debug({"on": 1, "gate": 9}, t), {"ok": False, "debug": 0}, "no node on gate 9")
        self.assertEqual(self.sim.set_debug({"on": 1, "gate": self.sim.nodes[2].gate}, t)["ok"], False, "its node is offline")
        self.assertIsNone(self.sim.set_debug({"on": 1, "gate": 16}, t), "gate out of range")
        self.assertEqual(self.sim.key({"keys": "T", "gate": node.gate}, t), {"ok": False, "msg": "debug mode is off"})
        self.assertEqual(self.sim.set_debug({"on": 1, "gate": node.gate}, t), {"ok": True, "debug": 1})
        out = self.sim.log({"since": 0, "gate": node.gate}, t)
        self.assertEqual((out["gate"], out["debug"], out["c5_alive"]), (node.gate, 1, 1))
        self.assertEqual(self.sim.key({"keys": "T", "gate": node.gate}, t), {"ok": True})
        self.assertIn("STATUS", self.sim.log({"since": 0, "gate": node.gate}, t)["text"])
        self.assertEqual(self.sim.log({"since": 0}, t)["debug"], 0, "gate 0 is not the one in debug mode")
        self.assertEqual(self.sim.log({"since": 0}, t)["text"], "")
        self.assertEqual(self.sim.set_debug({"gate": node.gate}, t)["debug"], 1, "no \"on\" only asks")
        # One gate at a time: switching gate 0 on switches the remote one off, and its log restarts.
        self.assertEqual(self.sim.set_debug({"on": 1, "gate": 0}, t), {"ok": True, "debug": 1})
        self.assertEqual(self.sim.set_debug({"gate": node.gate}, t)["debug"], 0)
        self.assertEqual(self.sim.log({"since": 0, "gate": 0}, t)["text"], "", "a fresh log")
        self.assertEqual(self.sim.set_debug({"on": 0, "gate": node.gate}, t)["debug"], 0, "off for a gate not in debug is a no-op")
        self.assertEqual(self.sim.set_debug({"gate": 0}, t)["debug"], 1)

    def test_a_remote_gates_blind_keys_ask_first(self):
        t = 60.0
        sim = web_mock.Master(vtx=True)
        sim.state(t)
        g = sim.nodes[0].gate
        sim.set_debug({"on": 1, "gate": g}, t)
        out = sim.key({"keys": "B", "gate": g}, t)
        self.assertTrue(out.get("vtx") and out["cn_x10"] > web_mock.ACTIVE_X10, out)
        self.assertEqual(sim.key({"keys": "B", "gate": g, "force": 1}, t), {"ok": True})
        sim.nodes[0].online = False
        self.assertEqual(sim.key({"keys": "T", "gate": g}, t), {"ok": False, "msg": "no C5 linked"}, "its node left")

    def test_the_remote_gates_passes_reach_its_log_only(self):
        sim = web_mock.Master()
        sim.set_debug({"on": 1, "gate": 1}, 0.0)
        sim.state(120.0)
        text = sim.log({"since": 0, "gate": 1}, 120.0)["text"]
        self.assertIn("PASS channel=R2", text)
        self.assertNotIn("PASS channel=R1", text, "gate 0's prints are not in a remote gate's log")

    def test_the_local_gates_passes_reach_the_log(self):
        self.sim.set_debug({"on": 1}, 0.0)
        self.sim.state(60.0)
        self.assertIn("PASS channel=R1", self.sim.log({"since": 0}, 60.0)["text"])


class GainCases:
    """/api/gain against c3_web.c's and c6_web.c's rules: range, the VTX guard, a C5 that relearns."""

    def setUp(self):
        self.sim = self.cls()

    def quiet(self, t):
        g = self.sim.local_c5()
        g.next_pass, g.last_pass = t + 100.0, t - 100.0

    def test_gain_page_posts_what_the_firmware_reads(self):
        page = page_text(self.page)
        src = (ROOT / SOURCES[self.cls]).read_text()
        sent = set()
        for obj in re.findall(r'/api/gain", JSON\.stringify\(\{([^{}]*)\}\)|post\("/api/gain", \{([^{}]*)\}', page):
            for part in obj:
                sent |= set(re.findall(r"(?:^|,)\s*(\w+)\s*(?=[:,]|$)", part.strip()))
        self.assertEqual(sent, self.gain_keys)
        routes = dict(re.findall(r'\.uri = "(/api/\w+)",\s*\.method = HTTP_POST,\s*\.handler = (\w+)', src))
        if "/api/gain" not in routes:       # c3_web.c registers its handlers one by one
            routes["/api/gain"] = "post_gain"
        self.assertLessEqual(sent, c_reads(c_function(src, routes["/api/gain"]), src))
        self.assertLessEqual(sent, mock_reads(self.cls, "gain"))
        widest = "{" + ",".join(f'"{k}":{v}' for k, v in (("gate", 15), ("gain", 83), ("force", 1)) if k in sent) + "}"
        self.assertLess(len(widest), self.cls.ADMIN["/api/gain"][1])

    def test_gain_range_and_effect(self):
        t = 60.0
        self.sim.state(t)
        self.quiet(t)
        for bad in (9, 84, -1):
            self.assertIsNone(self.sim.gain({"gain": bad, "gate": 0}, t), bad)
        out = self.sim.gain({"gain": 50, "gate": 0}, t)
        self.assertTrue(out["ok"])
        g = self.sim.local_c5()
        self.assertEqual(g.gain, 50)
        self.assertTrue(g.calibrating(t + 1), "the gate relearns its rests: blind a moment")
        self.assertFalse(g.calibrating(t + 10))

    def test_gain_asks_before_blinding_a_gate_that_sees_a_carrier(self):
        sim = self.cls(vtx=True)
        t = 60.0
        sim.state(t)
        out = sim.gain({"gain": 30, "gate": 0}, t)
        self.assertFalse(out["ok"])
        self.assertTrue(out.get("vtx"), out)
        self.assertNotEqual(sim.local_c5().gain, 30, "nothing was sent")
        self.assertTrue(sim.gain({"gain": 30, "gate": 0, "force": 1}, t)["ok"])
        self.assertEqual(sim.local_c5().gain, 30)


class NodeGain(GainCases, unittest.TestCase):
    cls, page, gain_keys = web_mock.Node, "firmware/node/main/www/index.html", {"gain", "force"}


class MasterGain(GainCases, unittest.TestCase):
    cls, page, gain_keys = web_mock.Master, "firmware/master/main/www/index.html", {"gate", "gain", "force"}

    def test_a_remote_gate_takes_its_gain_through_its_node(self):
        t = 60.0
        self.sim.state(t)
        node = self.sim.nodes[0]
        node.next_pass, node.last_pass = t + 100.0, t - 100.0
        self.assertEqual(self.sim.gain({"gain": 44, "gate": node.gate}, t),
                         {"ok": True, "sent": [node.gate], "failed": [], "busy": []})
        self.assertEqual(node.gain, 44)
        self.assertEqual(self.sim.gain({"gain": 46, "gate": node.gate}, t + 1)["busy"], [node.gate], "still relearning")
        self.assertEqual(node.gain, 44)
        off = self.sim.nodes[2]                                   # offline
        self.assertEqual(self.sim.gain({"gain": 44, "gate": off.gate}, t)["failed"], [off.gate])
        self.assertIsNone(self.sim.gain({"gain": 44, "gate": 9}, t), "no such gate")


class MasterOrder(unittest.TestCase):
    """The gate order (c6_web.c post_order, c6_hub_set_order): every node once, first gate first, gates 1..N."""

    def setUp(self):
        self.sim = web_mock.Master()
        self.sim.state(60.0)
        self.ids = [n.node_id for n in self.sim.nodes]
        self.gates = lambda: {n.node_id: n.gate for n in self.sim.nodes}

    def test_ids_start_at_zero_and_stay_unique_after_a_removal(self):
        self.assertEqual(sorted(self.ids), list(range(len(self.ids))))
        self.sim.remove_node({"node_id": 1}, 60.0)
        self.sim.add_node(9, 9, 9.0)
        ids = [n.node_id for n in self.sim.nodes]
        self.assertEqual(len(set(ids)), len(ids), "a node paired after a removal never reuses a held id")
        self.assertEqual(self.sim.nodes[-1].node_id, 1, "it takes the lowest free one")

    def test_the_order_renumbers_the_gates_from_one(self):
        want = list(reversed(self.ids))
        self.assertEqual(self.sim.order({"order": want}, 60.0), {"ok": True})
        self.assertEqual(self.gates(), {nid: i + 1 for i, nid in enumerate(want)})

    def test_a_list_that_is_not_every_node_once_changes_nothing(self):
        before = self.gates()
        for bad in (self.ids[:-1], self.ids + [99], [self.ids[0]] * len(self.ids), [], "x", None):
            self.assertEqual(self.sim.order({"order": bad}, 60.0), {"ok": False}, bad)
        self.assertEqual(self.sim.order({}, 60.0), {"ok": False})
        self.assertEqual(self.gates(), before)

    def test_the_same_order_leaves_the_race_alone(self):
        self.sim.order({"order": self.ids}, 60.0)           # makes the gates 1..N
        self.sim.state(80.0)
        laps = {c: s["laps"] for c, s in self.sim.laps.items()}
        self.assertTrue(any(laps.values()))
        self.sim.order({"order": self.ids}, 81.0)
        self.assertEqual({c: s["laps"] for c, s in self.sim.laps.items()}, laps, "no change, no restart")
        self.sim.order({"order": list(reversed(self.ids))}, 82.0)
        self.assertEqual(self.sim.laps, {}, "a change restarts the race timing")

    def test_a_remote_gate_in_debug_mode_follows_its_node(self):
        node = self.sim.nodes[0]
        self.sim.set_debug({"on": 1, "gate": node.gate}, 60.0)
        self.sim.order({"order": list(reversed(self.ids))}, 61.0)
        self.assertEqual(self.sim.debug_gate, node.gate, "debug mode stays on the same node")
        self.assertEqual(self.sim.log({"since": 0, "gate": node.gate}, 61.0)["debug"], 1)

    def test_the_old_per_node_save_is_gone(self):
        self.assertNotIn("/api/node", web_mock.Master.ADMIN)
        page = page_text("firmware/master/main/www/index.html")
        self.assertNotIn('class="save"', page)
        self.assertNotIn('"/api/node",', page)


class MasterPerGate(unittest.TestCase):
    """D (the re-arm drop) and G (the RX gain) for every gate or one: c6_hub_set_rearm(_gate), post_gain's run_on_gates."""

    def setUp(self):
        self.sim = web_mock.Master()
        self.sim.state(60.0)
        for g in [self.sim.local] + self.sim.nodes:           # nothing near a gate: no VTX guard in these tests
            g.next_pass, g.last_pass = 160.0, -40.0

    def want(self, gate):
        s = self.sim.state(60.0)
        d = s["local"] if gate == 0 else next(n for n in s["nodes"] if n["gate"] == gate)
        return d["rearm_want_x10"], d["rearm_own"]

    def test_d_for_one_gate_changes_only_that_gate(self):
        fleet = self.sim.rearm
        self.assertEqual(self.sim.set_rearm({"rearm_db_x10": 90, "gate": 2}, 60.0), {"ok": True})
        self.assertEqual(self.want(2), (90, True))
        self.assertEqual((self.want(0), self.want(1)), ((fleet, False), (fleet, False)))
        self.assertEqual(self.sim.rearm, fleet, "the fleet's D is untouched")
        self.assertEqual(self.sim.nodes[1].rearm, 90, "its C5 now runs it")
        self.assertEqual(self.sim.set_rearm({"rearm_db_x10": 70, "gate": 0}, 60.0), {"ok": True})
        self.assertEqual(self.want(0), (70, True), "gate 0 keeps its own in its own store")

    def test_all_gates_drops_every_own_d(self):
        self.sim.set_rearm({"rearm_db_x10": 90, "gate": 2}, 60.0)
        self.sim.set_rearm({"rearm_db_x10": 70, "gate": 0}, 60.0)
        self.assertEqual(self.sim.set_rearm({"rearm_db_x10": 200}, 60.0), {"ok": True})
        self.assertEqual([self.want(g) for g in (0, 1, 2)], [(200, False)] * 3)
        self.assertEqual(self.sim.nodes[1].rearm, 200)

    def test_minus_one_gives_a_gate_the_fleet_d_again(self):
        self.sim.set_rearm({"rearm_db_x10": 90, "gate": 1}, 60.0)
        self.assertEqual(self.sim.set_rearm({"rearm_db_x10": -1, "gate": 1}, 60.0), {"ok": True})
        self.assertEqual(self.want(1), (self.sim.rearm, False))
        self.assertEqual(self.sim.nodes[0].rearm, self.sim.rearm)

    def test_d_refuses_what_it_cannot_do(self):
        for body in ({"rearm_db_x10": 401, "gate": 1}, {"rearm_db_x10": -2, "gate": 1}, {"rearm_db_x10": 90, "gate": 9},
                     {"rearm_db_x10": -1}, {"gate": 1}, {"rearm_db_x10": 90, "gate": 16}):
            self.assertEqual(self.sim.set_rearm(body, 60.0), {"ok": False}, body)

    def test_an_own_d_follows_its_node_when_the_order_changes(self):
        self.sim.set_rearm({"rearm_db_x10": 90, "gate": 1}, 60.0)
        node = self.sim.nodes[0]
        self.sim.order({"order": list(reversed([n.node_id for n in self.sim.nodes]))}, 61.0)
        self.assertNotEqual(node.gate, 1)
        self.assertEqual(self.want(node.gate), (90, True), "the D stays with the node, not with the number")

    def test_an_own_d_survives_a_passage_calibration(self):
        self.sim.set_rearm({"rearm_db_x10": 90, "gate": 1}, 60.0)
        for action, dt in (("start1", 0), ("state", 6), ("start2", 0), ("state", 6), ("apply", 0), ("reset", 0)):
            self.sim.calib({"gate": 1, "action": action}, 60.0 + dt * (action == "state") + 12 * (action in ("start2", "apply", "reset")))
        self.assertEqual(self.want(1), (90, True))

    def test_g_for_every_gate_reaches_each_reachable_one(self):
        out = self.sim.gain({"gate": -1, "gain": 50}, 60.0)
        self.assertEqual(out["ok"], True)
        self.assertEqual((out["sent"], out["failed"], out["busy"]), ([0, 1, 2], [], []), "every reachable gate; gate 3's node is offline")
        self.assertEqual([self.sim.local.gain, self.sim.nodes[0].gain, self.sim.nodes[1].gain], [50, 50, 50])
        self.assertNotEqual(self.sim.nodes[2].gain, 50, "an offline node is not reached")
        self.assertEqual(self.sim.gain({"gate": 3, "gain": 50}, 60.0)["failed"], [3], "asked by name, it is listed as failed")

    def test_g_for_every_gate_asks_when_one_sees_a_carrier_and_skips_a_calibrating_one(self):
        self.sim.nodes[0].next_pass, self.sim.nodes[0].last_pass = 60.0, 60.0     # a drone at gate 1 right now
        out = self.sim.gain({"gate": -1, "gain": 40}, 60.0)
        self.assertFalse(out["ok"])
        self.assertEqual([v["gate"] for v in out["vtx"]], [1], "only the gate that sees it is listed")
        self.assertNotEqual(self.sim.local.gain, 40, "nothing was sent before the admin forces it")
        self.sim.nodes[1].run("cal_quick", 60.0)
        out = self.sim.gain({"gate": -1, "gain": 40, "force": 1}, 60.0)
        self.assertEqual((out["sent"], out["busy"]), ([0, 1], [2]))

    def test_the_widest_body_fits_the_firmware_buffers(self):
        self.assertLess(len('{"rearm_db_x10":400,"gate":15}'), web_mock.Master.ADMIN["/api/rearm"][1])
        self.assertLess(len('{"gate":-1,"gain":83,"force":1}'), web_mock.Master.ADMIN["/api/gain"][1])


class MasterCalib(unittest.TestCase):
    """/api/calib against c6_web.c's post_calib and the C5's session (lt_calib.c): two steps, then a proposal."""

    def setUp(self):
        self.sim = web_mock.Master()
        self.sim.state(60.0)
        self.t = 60.0

    def call(self, action, dt=0.0, gate=0):
        self.t += dt
        return self.sim.calib({"gate": gate, "action": action}, self.t)

    def test_two_steps_then_a_proposal(self):
        self.assertEqual(self.call("state")["phase"], 0)
        out = self.call("start1")
        self.assertEqual((out["phase"], out["secs_left"]), (1, 5))
        self.assertEqual(self.call("state", 2.0)["secs_left"], 3, "the countdown follows the measurement")
        self.assertEqual(self.call("start2", 0.5)["flags"] & 32, 32, "step 2 while measuring: refused")
        out = self.call("state", 3.0)
        self.assertEqual((out["phase"], out["channel"], out["passage"]), (2, 0, web_mock.CAL_PASSAGE))
        self.assertEqual(out["rec"]["min_peak"], 0, "no proposal before step 2")
        self.assertEqual(self.call("start2")["phase"], 3)
        out = self.call("state", 5.5)
        self.assertEqual((out["phase"], out["away"], out["rec"]), (4, web_mock.CAL_AWAY, web_mock.CAL_REC))
        self.assertTrue(self.call("apply")["ok"])
        self.assertEqual(self.sim.applied[0], web_mock.CAL_REC)
        self.assertEqual(self.call("state")["phase"], 0, "applying ends the session")
        self.assertEqual(self.call("apply"), {**self.call("state"), "ok": False, "msg": "nothing to apply"})

    def test_the_channel_is_any_or_the_one_asked(self):
        self.sim.calib({"gate": 0, "action": "start1"}, self.t)
        self.assertEqual(self.sim.calib({"gate": 0, "action": "state"}, self.t + 6)["channel"], self.sim.local.channel,
                         "no channel given: any, the drone's own")
        self.sim.calib({"gate": 0, "action": "cancel"}, self.t + 6)
        self.sim.calib({"gate": 0, "action": "start1", "channel": 5}, self.t + 7)
        self.assertEqual(self.sim.calib({"gate": 0, "action": "state"}, self.t + 13)["channel"], 5, "R6 asked, R6 measured")
        self.sim.calib({"gate": 0, "action": "cancel"}, self.t + 13)
        self.sim.calib({"gate": 0, "action": "start1", "channel": 9}, self.t + 14)
        self.assertEqual(self.sim.calib({"gate": 0, "action": "state"}, self.t + 20)["channel"], self.sim.local.channel,
                         "a channel past R8 means any")

    def test_step_2_needs_step_1_and_cancel_ends_it(self):
        self.assertEqual(self.call("start2")["flags"] & 32, 32, "step 2 first: refused")
        self.call("start1")
        self.call("state", 6.0)
        self.call("start2")
        self.assertEqual(self.call("cancel")["phase"], 0)

    def test_a_gate_without_a_reachable_c5(self):
        offline = self.sim.nodes[2].gate                              # its node is offline
        for gate in (9, offline):
            out = self.call("start1", gate=gate)
            self.assertEqual((out["ok"], out["msg"]), (False, "no C5 linked"), gate)
        self.assertIsNone(self.sim.calib({"gate": 16, "action": "state"}, self.t))
        self.assertIsNone(self.sim.calib({"gate": 0, "action": "nope"}, self.t))

    def test_a_remote_gate_calibrates_through_its_node_and_keeps_its_thresholds(self):
        g = self.sim.nodes[0].gate
        self.assertFalse(self.sim.state(self.t)["nodes"][0]["custom"], "the fleet's thresholds to begin with")
        self.assertEqual(self.call("start1", gate=g)["phase"], 1)
        self.call("state", 6.0, gate=g)
        self.call("start2", gate=g)
        self.assertEqual(self.call("state", 6.0, gate=g)["phase"], 4)
        self.assertEqual(self.call("state")["phase"], 0, "gate 0 has no session of its own")
        self.assertTrue(self.call("apply", gate=g)["ok"])
        self.assertEqual(self.sim.applied[g], web_mock.CAL_REC)
        self.assertTrue(self.sim.state(self.t)["nodes"][0]["custom"], "the node runs calibrated thresholds")
        out = self.call("reset", gate=g)
        self.assertTrue(out["ok"])
        self.assertFalse(self.sim.state(self.t)["nodes"][0]["custom"], "back on the fleet's")
        out = self.call("reset")
        self.assertEqual((out["ok"], out["msg"]), (False, "gate 0 keeps its own settings"))

    def test_a_calibrating_gate_refuses(self):
        self.sim.local.run("cal_quick", self.t)
        self.assertEqual(self.call("start1")["flags"] & 32, 32, "the rests are being learnt: busy")


class NodeCalib(unittest.TestCase):
    """The degraded C3's /api/calib (c3_web.c post_calib): the same session as the C6's, one gate, applied into its config."""

    def setUp(self):
        self.sim = web_mock.Node()
        self.sim.state(60.0)
        self.t = 60.0

    def call(self, action, dt=0.0):
        self.t += dt
        return self.sim.calib({"action": action}, self.t)

    def test_two_steps_apply_and_the_config_follows(self):
        before = dict(self.sim.config)
        self.assertEqual(self.call("start1")["phase"], 1)
        self.assertEqual(self.call("state", 6.0)["phase"], 2)
        self.call("start2")
        out = self.call("state", 6.0)
        self.assertEqual((out["phase"], out["rec"]), (4, web_mock.CAL_REC))
        self.assertEqual(self.sim.config, before, "nothing changes before Apply")
        self.assertTrue(self.call("apply")["ok"])
        self.assertEqual((self.sim.config["active_db_x10"], self.sim.config["min_peak_db_x10"],
                          self.sim.config["hysteresis_db_x10"], self.sim.config["drop_db_x10"]), (105, 125, 40, 30))
        self.assertEqual(self.sim.config["rearm_db_x10"], before["rearm_db_x10"], "D stays")
        self.assertEqual(self.call("state")["phase"], 0)

    def test_no_reset_and_a_missing_c5(self):
        self.assertIsNone(self.call("reset"), "the C3 has no fleet thresholds to go back to")
        self.sim.gate.run("reboot", self.t)
        out = self.call("start1")
        self.assertEqual((out["ok"], out["msg"]), (False, "no C5 linked"))

    def test_page_sends_what_the_firmware_reads(self):
        page = page_text("firmware/node/main/www/index.html")
        src = (ROOT / SOURCES[web_mock.Node]).read_text()
        self.assertIn('"/api/calib", JSON.stringify({ action, channel })', page)
        self.assertEqual(c_reads(c_function(src, "post_calib"), src), {"action", "channel"})
        self.assertLessEqual({"action", "channel"}, mock_reads(web_mock.Node, "calib"))
        self.assertLess(len('{"action":"start1","channel":255}'), web_mock.Node.ADMIN["/api/calib"][1])


class PagesAccessibility(unittest.TestCase):
    """What the modern-web-guidance audit asked of both pages: announced statuses, one h1, tables a keyboard and a screen
    reader can use, bigger touch targets, and no refresh while the tab is hidden."""
    PAGES = {"node": "firmware/node/main/www/index.html", "master": "firmware/master/main/www/index.html"}

    def pages(self):
        return {k: page_text(v) for k, v in self.PAGES.items()}

    def test_every_status_message_is_a_live_region(self):
        for name, page in self.pages().items():
            for el in re.findall(r'<span id="(\w*msg)"[^>]*>', page):
                tag = re.search(rf'<span id="{el}"[^>]*>', page).group(0)
                self.assertIn('role="status"', tag, f"{name}: #{el} is not announced")
            self.assertRegex(page, r'<pre id="dbg_log" role="log" aria-live="off"[^>]*tabindex="0"')
        self.assertIn('id="conn" class="pill bad" role="alert"', self.pages()["master"])

    def test_one_h1_the_page_title(self):
        for name, page in self.pages().items():
            self.assertEqual(len(re.findall(r"<h1[ >]", page)), 1, name)
            self.assertRegex(page, r"<header[^>]*>\s*(?:<div>)?<h1>Lëtz Timer C[36]")

    def test_tables_are_named_scrollable_and_have_column_scopes(self):
        page = self.pages()["master"]
        tables = re.findall(r"<table[^>]*>(?:<caption[^>]*>.*?</caption>)?", page)
        self.assertGreaterEqual(len(tables), 6)
        for t in tables:
            self.assertIn("<caption", t, "a table without a caption")
        for head in re.findall(r"<thead>(.*?)</thead>", page, re.S):
            self.assertNotRegex(head, r"<th(?![^>]*scope=)", "a column header without scope")
        for card in re.findall(r'<div class="card scroll[^"]*"[^>]*>', page):
            self.assertIn('tabindex="0" role="region"', card, card)
            ref = re.search(r'aria-labelledby="(\w+)"', card).group(1)
            self.assertIn(f'id="{ref}"', page, f"{ref} names nothing")

    def test_a_narrow_gates_table_becomes_cards_with_a_label_on_each_cell(self):
        page = self.pages()["master"]
        self.assertIn('<table class="stack">', page)
        self.assertRegex(page, r"@container \(width < 40rem\)")
        for label in ("Gate", "Node", "Link", "Health", "RSSI", "Loss", "Seen"):
            self.assertIn(f'data-label="{label}"', page)

    def test_touch_targets_grow_on_coarse_pointers(self):
        for name, page in self.pages().items():
            block = re.search(r"@media \(pointer: coarse\) \{(.*?)\n  \}", page, re.S)
            self.assertIsNotNone(block, name)
            self.assertIn("min-block-size: 2.75rem", block.group(1))
            self.assertLess(page.index("button.info {"), page.index("@media (pointer: coarse)"), "the override must come after the size rules")

    def test_a_hidden_tab_asks_the_esp_for_nothing(self):
        for name, page in self.pages().items():
            self.assertRegex(page, r"async function poll\(\) \{\n  if \(document\.hidden\) return;")
            self.assertRegex(page, r"async function dbgPoll\(\) \{\n  if \(document\.hidden \|\|")
            self.assertRegex(page, r"async function calPoll\(\) \{\n  if \(document\.hidden\) return;")
            self.assertIn('document.addEventListener("visibilitychange"', page)


class PagesPlatform(unittest.TestCase):
    """The platform features the pages use as the web guidance asks: native switches, popover notes, a Content-Security-Policy
    the pages obey (no inline script), accent-color and reduced motion."""
    PAGES = {"node": "firmware/node/main/www/index.html", "master": "firmware/master/main/www/index.html"}

    def html(self):
        return {k: (ROOT / v).read_text() for k, v in self.PAGES.items()}

    def test_the_pages_run_no_inline_script(self):
        for name, html in self.html().items():
            self.assertEqual(re.findall(r"<script(?![^>]*\bsrc=)[^>]*>", html), [], f"{name}: an inline script")
            self.assertEqual(re.findall(r"\son[a-z]+=\"", html), [], f"{name}: an inline event handler")
            self.assertEqual(re.findall(r"javascript:", html), [])
            self.assertIn('<script src="/auth.js"></script>\n<script src="/app.js"></script>', html)
            self.assertTrue((ROOT / self.PAGES[name]).with_name("app.js").exists())

    def test_the_policy_lets_only_the_pages_own_scripts_run(self):
        csp = web_mock.page_headers()["Content-Security-Policy"]
        directives = dict(d.strip().split(" ", 1) for d in csp.split(";") if d.strip())
        self.assertEqual(directives["script-src"], "'self'", "no inline script, no eval, no other origin")
        self.assertEqual(directives["default-src"], "'none'")
        self.assertEqual(directives["connect-src"], "'self'")
        for d in ("base-uri", "form-action", "frame-ancestors"):
            self.assertEqual(directives[d], "'none'", d)
        self.assertNotIn("unsafe-eval", csp)
        self.assertNotIn("'unsafe-inline'", directives["script-src"])
        self.assertEqual(web_mock.page_headers()["X-Content-Type-Options"], "nosniff")

    def test_every_page_and_script_the_firmware_serves_carries_the_policy(self):
        for path in ("firmware/node/main/c3_web.c", "firmware/master/main/c6_web.c"):
            src = (ROOT / path).read_text()
            for fn in ("get_root", "get_app_js"):
                self.assertIn("web_auth_page_headers(req)", c_function(src, fn), f"{path}: {fn}")
            routes = re.findall(r'\.uri = "(/[^"]*)",\s*\.method = HTTP_GET', src)
            self.assertIn("/app.js", routes, path)
            wired = len(re.findall(r'\.uri = "/', src)) + 2          # + web_auth's /api/session and /auth.js
            self.assertLessEqual(wired, int(re.search(r"max_uri_handlers = (\d+)", src).group(1)), f"{path}: handlers")
        self.assertIn("web_auth_page_headers(req)", c_function((ROOT / "components/web_auth/web_auth_esp.c").read_text(), "get_js"))

    def test_the_mock_serves_the_script_with_the_policy(self):
        sim = web_mock.Master()
        handler = web_mock.make_handler("master", sim, web_mock.Auth("c6", "pw"), 0)
        handler.log_message = lambda *a: None
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for path, ctype in (("/", "text/html"), ("/app.js", "application/javascript"), ("/auth.js", "application/javascript")):
                c = http.client.HTTPConnection("127.0.0.1", server.server_address[1])
                c.request("GET", path)
                r = c.getresponse()
                r.read()
                self.assertEqual((r.status, r.getheader("Content-Type")), (200, ctype), path)
                self.assertEqual(r.getheader("Content-Security-Policy"), web_mock.page_headers()["Content-Security-Policy"], path)
                c.close()
        finally:
            server.shutdown()
            server.server_close()

    def test_the_switches_are_buttons_that_say_switch_and_their_state(self):
        """Kept as buttons with role=switch (their look and feel are the ones the operator knows): each says its state in aria-checked."""
        for name, html in self.html().items():
            self.assertNotRegex(html, r'<input[^>]*\bswitch\b', f"{name}: a native switch is not used")
            ids = re.findall(r'<button id="(\w+)" class="switch" role="switch" aria-checked="false"><i></i><span>', html)
            self.assertEqual(len(ids), len(re.findall(r'class="switch"', html)), f"{name}: a switch that is not one")
            js = (ROOT / self.PAGES[name]).with_name("app.js").read_text()
            for i in ids:
                self.assertIn(f'$("{i}").setAttribute("aria-checked"', js, f"{name}: #{i} never says its state")
        self.assertEqual(sorted(re.findall(r'<button id="(\w+)" class="switch" role="switch"', self.html()["master"])),
                         ["dbg_on", "mode", "pause", "role"])

    def test_the_accent_colour_and_reduced_motion_are_set(self):
        for name, html in self.html().items():
            self.assertIn("accent-color: #2f6feb", html, name)
            self.assertRegex(html, r"@media \(prefers-reduced-motion: reduce\) \{[^}]*transition: none", name)
            self.assertEqual(len(re.findall(r"transition:", html)), 2, f"{name}: a transition without its reduced-motion override")


if __name__ == "__main__":
    unittest.main()
