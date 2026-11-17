#!/usr/bin/env python3
"""tools/web_mock.py stays in step with the firmware: /api/state fields and POST body
buffers read from c6_web.c / c3_web.c, then the signed protocol over real HTTP."""
import hashlib
import hmac
import http.client
import importlib.util
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


def json_keys(value):
    if isinstance(value, dict):
        return set(value) | {k for v in value.values() for k in json_keys(v)}
    if isinstance(value, list):
        return {k for v in value for k in json_keys(v)}
    return set()


class Firmware(unittest.TestCase):
    def test_state_fields_match_the_c_code(self):
        for cls, path in SOURCES.items():
            src = (ROOT / path).read_text()
            c = c_function(src, "get_state") + c_function(src, "put_health") + c_function(src, "put_levels")
            want = set(re.findall(r'\\"([a-z0-9_]+)\\":', c))
            sim = cls()
            sim.state(60.0)                       # past a few passes: every list is filled
            self.assertTrue(want)
            self.assertEqual(json_keys(sim.state(61.0)), want, path)

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

    def test_rearm_default_matches_link_proto(self):
        header = (ROOT / "components/link_proto/include/link_proto.h").read_text()
        self.assertEqual(web_mock.REARM_X10, int(re.search(r"LP_REARM_DEFAULT_X10\s+(\d+)", header).group(1)))

    def test_admin_routes_and_buffers_match_the_c_code(self):
        for cls, path in SOURCES.items():
            src = (ROOT / path).read_text()
            routes = re.findall(r'\.uri = "(/api/\w+)",\s*\.method = HTTP_POST,\s*\.handler = (\w+)', src)
            buffers = {uri: int(re.search(r"char buf\[(\d+)\]", c_function(src, h)).group(1)) for uri, h in routes}
            self.assertEqual({uri: cap for uri, (_, cap) in cls.ADMIN.items()}, buffers, path)


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
