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


def c_reads(fn):
    return set(re.findall(r'json_[is]\(buf, "(\w+)"', fn))


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
        page = (ROOT / "firmware/node/main/www/index.html").read_text()
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
        page = (ROOT / "firmware/master/main/www/index.html").read_text()
        src = (ROOT / SOURCES[web_mock.Master]).read_text()
        routes = dict(re.findall(r'\.uri = "(/api/\w+)",\s*\.method = HTTP_POST,\s*\.handler = (\w+)', src))
        posts = page_posts(page)
        self.assertEqual(posts.get("/api/rearm"), {"rearm_db_x10"})
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


if __name__ == "__main__":
    unittest.main()
