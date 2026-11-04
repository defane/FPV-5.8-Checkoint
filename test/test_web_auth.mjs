// Host test for the page side of web_auth (components/web_auth/www/auth.js): its
// SHA-256/HMAC/PBKDF2 against node:crypto and the build-side admin_key.py, the
// vectors shared with test_web_auth.c, and the session/seq/queue protocol against
// a mock ESP. Run: node test_web_auth.mjs (python3 on the PATH for admin_key.py).
import { createHash, createHmac, pbkdf2Sync, randomBytes } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

let fails = 0, oks = 0;
const check = (name, cond) => { if (cond) oks++; else { console.log("FAIL: " + name); fails++; } };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const ITER = 1000;   // the mock's iterations (firmware: 200000), keeps the test fast

// A mock ESP: /api/session, signed /api/auth and /api/node, like web_auth_esp.c.
// stalls.add(path): the next request to path hangs until the page aborts it.
// bodyStalls.add(path): its headers arrive, its body never does (until aborted).
// holds.add(path): the next request to path waits for release(status, json), unprocessed.
function mockEsp({ password = "checkpoint", salt = "fpv58-checkpoint/c6/0123456789abcdef", open = false } = {}) {
  const esp = { sessions: new Map(), log: [], salt, open, nextSid: 0, stalls: new Set(), bodyStalls: new Set(),
                holds: new Set(), held: [] };
  esp.setPassword = pw => { esp.key = pbkdf2Sync(pw, salt, ITER, 32, "sha256"); };
  esp.setPassword(password);
  esp.release = (status, obj) => esp.held.shift()(new Response(JSON.stringify(obj), { status }));
  esp.fetch = async (path, init = {}) => {
    const method = init.method || "GET", body = init.body ?? "", h = init.headers || {};
    if (esp.stalls.delete(path))
      return new Promise((_, reject) => init.signal.addEventListener("abort", () => reject(new Error("aborted"))));
    if (esp.bodyStalls.delete(path)) {
      const stream = new ReadableStream({ start(c) {
        init.signal.addEventListener("abort", () => c.error(new Error("aborted"))); } });
      return new Response(stream, { status: 200 });
    }
    if (esp.holds.delete(path)) return new Promise(res => esp.held.push(res));
    await sleep(Math.random() * 5);            // let requests race
    const json = (status, obj) => new Response(JSON.stringify(obj), { status });
    if (path === "/api/session" && method === "POST") {
      const sid = (esp.nextSid++).toString(16).padStart(32, "0");
      esp.sessions.set(sid, 0);
      return json(200, { sid, salt: esp.salt, iter: ITER, open: esp.open });
    }
    const sid = h["X-Auth-Session"], seq = h["X-Auth-Seq"], sig = h["X-Auth-Sig"];
    esp.log.push({ path, method, sid, seq: +seq, body });
    if (!esp.open) {
      if (!esp.sessions.has(sid)) return json(401, { ok: false, auth: true, session: false });
      const want = createHmac("sha256", esp.key).update(`${sid}\n${seq}\n${method}\n${path}\n${body}`).digest("hex");
      if (!(+seq > esp.sessions.get(sid)) || sig !== want) return json(401, { ok: false, auth: true });
      esp.sessions.set(sid, +seq);
    }
    return json(200, path === "/api/auth" ? { admin: true } : { ok: true, echo: body });
  };
  return esp;
}

function loadAuth(fetchImpl) {
  const ctx = vm.createContext({ fetch: (...a) => fetchImpl(...a), TextEncoder, Promise, Error, Uint8Array,
                                 Uint32Array, DataView, Array, Math, String, JSON, setTimeout, clearTimeout,
                                 AbortController, Response });
  const src = readFileSync(new URL("../components/web_auth/www/auth.js", import.meta.url), "utf8");
  vm.runInContext(src + "\nthis.AdminAuth = AdminAuth;", ctx);
  return ctx.AdminAuth;
}

const A = loadAuth(() => { throw new Error("no fetch"); });
const { sha256, hmac, pbkdf2, sign } = A._test;
const hex = b => Buffer.from(b).toString("hex");
const u8 = b => new Uint8Array(b);

// SHA-256, HMAC and PBKDF2 against node:crypto (every SHA padding boundary up to 3 blocks).
let shaOk = true, hmacOk = true, pbkdf2Ok = true;
for (let n = 0; n <= 200; n++) {
  const m = randomBytes(n);
  shaOk &&= hex(sha256(u8(m))) === createHash("sha256").update(m).digest("hex");
}
for (const n of [1000, 4096, 65537]) {
  const m = randomBytes(n);
  shaOk &&= hex(sha256(u8(m))) === createHash("sha256").update(m).digest("hex");
}
for (let n = 0; n <= 130; n += 7) {
  const k = randomBytes(n), m = randomBytes(n * 3);
  hmacOk &&= hex(hmac(u8(k), u8(m))) === createHmac("sha256", k).update(m).digest("hex");
}
for (const [pl, sl, it] of [[0, 0, 1], [1, 1, 1], [10, 20, 2], [64, 16, 3], [65, 16, 5], [100, 40, 1000], [13, 37, 4096]]) {
  const p = randomBytes(pl), s = randomBytes(sl);
  pbkdf2Ok &&= hex(pbkdf2(u8(p), u8(s), it)) === pbkdf2Sync(p, s, it, 32, "sha256").toString("hex");
}
check("sha256 matches node:crypto (0..200, 1000, 4096, 65537 bytes)", shaOk);
check("hmac matches node:crypto (keys 0..130 bytes)", hmacOk);
check("pbkdf2 matches node:crypto (passwords 0..100 bytes, 1..4096 iterations)", pbkdf2Ok);

// The build side (components/web_auth/admin_key.py) derives the same key as the page.
{
  const dir = mkdtempSync(join(tmpdir(), "web_auth-"));
  try {
    const pw = "é ünïcödé \"q\" ${x} \\ ;", salt = "fpv58-checkpoint/c6/0123456789abcdef";
    writeFileSync(join(dir, "sdkconfig.json"), JSON.stringify({ C6_ADMIN_PASSWORD: pw, C3_ADMIN_PASSWORD: "" }));
    const script = fileURLToPath(new URL("../components/web_auth/admin_key.py", import.meta.url));
    const run = name => execFileSync("python3", ["-I", script, join(dir, "sdkconfig.json"), name, salt, "200000"],
                                     { encoding: "utf8" }).trim();
    check("admin_key.py == page pbkdf2 (200000 iterations, UTF-8, quotes, ${...})",
          run("C6_ADMIN_PASSWORD") === hex(pbkdf2(new TextEncoder().encode(pw), new TextEncoder().encode(salt), 200000)));
    check("admin_key.py prints nothing for an empty password", run("C3_ADMIN_PASSWORD") === "");
  } finally { rmSync(dir, { recursive: true }); }
}

// Vectors shared with test_web_auth.c (Python hmac), for an arbitrary 32-byte key.
const K = sha256(new TextEncoder().encode("fpv58-checkpoint/c6:checkpoint"));
const SID = "00112233445566778899aabbccddeeff";
check("key vector", hex(K) === "13a66609c44b40550b8ea5ce655b116e9543439a7870301fc94edf5b217c530e");
check("vector 1", sign(K, SID, "1", "POST", "/api/reset", "{}") === "4ca74e9d9a99af7f5f9a48e8cec40d2e2892d8541214aeb11a5305087ea9c40e");
check("vector 2", sign(K, SID, "2", "GET", "/api/auth", "") === "391ec91e5996b3a933f648d53cc00156fa0ce40b43859d7b6e358d4d58b62a3d");
check("vector 3", sign(K, SID, "3", "POST", "/api/node", '{"node_id":1,"gate":2,"channel":3}') === "ef7f3361727f8339a720e97a887a73fab0b78cc74b778d36db241b504e79aacf");

// Protocol against the mock ESP.
{
  const esp = mockEsp();
  const auth = loadAuth(esp.fetch);
  check("not logged in: request rejects", await auth.request("POST", "/api/node", "{}").then(() => false, () => true));
  check("wrong password refused", (await auth.login("nope")) === false);
  check("refused login leaves no key", await auth.request("POST", "/api/node", "{}").then(() => false, () => true));
  check("right password accepted", (await auth.login("checkpoint")) === true);
  const r = await auth.request("POST", "/api/node", '{"gate":2}');
  check("signed POST accepted", r.ok && (await r.json()).echo === '{"gate":2}');
  check("login used seq 1, the POST seq 2", esp.log.at(-2).seq === 1 && esp.log.at(-1).seq === 2);

  // Requests fired together still reach the ESP in seq order (and all pass).
  const all = await Promise.all([1, 2, 3, 4, 5].map(i => auth.request("POST", "/api/node", `{"i":${i}}`)));
  check("concurrent requests all accepted", all.every(x => x.ok));
  check("concurrent requests sent in order", esp.log.slice(-5).map(e => e.seq).join() === "3,4,5,6,7");

  // A replayed capture is refused by the ESP itself.
  const last = esp.log.at(-1);
  const rep = await esp.fetch(last.path, { method: "POST", body: last.body, headers: {
    "X-Auth-Session": last.sid, "X-Auth-Seq": String(last.seq),
    "X-Auth-Sig": sign(esp.key, last.sid, String(last.seq), "POST", last.path, last.body) } });
  check("replay refused by the mock (same rule as wa_verify)", rep.status === 401);

  // ESP reboot: sessions gone, the page reopens one by itself.
  esp.sessions.clear();
  const after = await auth.request("POST", "/api/node", "{}");
  check("after a reboot the request goes through", after.ok);
  check("...in a new session restarting at seq 1", esp.log.at(-1).seq === 1 && esp.log.at(-1).sid !== last.sid);

  // Password changed on the ESP: refused once, no retry loop.
  const before = esp.log.length;
  esp.setPassword("other");
  const bad = await auth.request("POST", "/api/node", "{}");
  check("bad signature returned as 401", bad.status === 401);
  check("bad signature not retried", esp.log.length === before + 1);
  esp.setPassword("checkpoint");

  auth.logout();
  check("logout forgets the key", await auth.request("POST", "/api/node", "{}").then(() => false, () => true));
}
{
  // A request the ESP never answers: aborted after the timeout, the queue goes on.
  const esp = mockEsp();
  const auth = loadAuth(esp.fetch);
  auth._test.cfg.timeoutMs = 100;
  check("login before the stall", await auth.login("checkpoint"));
  esp.stalls.add("/api/node");
  const stalled = auth.request("POST", "/api/node", '{"stalled":1}');
  const nextOk = auth.request("POST", "/api/pairing", "{}");
  check("stalled request rejects after the timeout", await stalled.then(() => false, () => true));
  check("the next request still goes through", (await nextOk).ok);

  // Logout while a request is stuck: the click queued behind it never runs, the next login is not blocked.
  auth._test.cfg.timeoutMs = 2000;             // the stuck request would hold a shared queue 2 s
  esp.stalls.add("/api/node");
  const stuck = auth.request("POST", "/api/node", '{"stuck":1}');
  const ghost = auth.request("POST", "/api/reset", '{"ghost":1}');
  await sleep(10);
  auth.logout();
  const relog = await Promise.race([auth.login("checkpoint"), sleep(1000).then(() => "blocked")]);
  check("login right after a logout is not blocked by a stuck request", relog === true);
  check("a click made before the logout is dropped", await ghost.then(() => false, () => true));
  await stuck.catch(() => {});
  await sleep(50);
  check("...and never reaches the ESP", !esp.log.some(e => e.body === '{"ghost":1}'));
}
for (const late of [{ ok: false, auth: true, session: false }, { ok: false, auth: true }]) {
  // A request in flight at logout, answered with a late 401 after the next login:
  // it is neither re-signed nor retried, and does not log the new login out.
  const esp = mockEsp();
  const auth = loadAuth(esp.fetch);
  const what = late.session === false ? "session:false" : "bad-signature";
  check("login 1", await auth.login("checkpoint"));
  esp.holds.add("/api/reset");
  const stale = auth.request("POST", "/api/reset", '{"stale":1}');
  await sleep(10);
  auth.logout();
  check("login 2", await auth.login("checkpoint"));
  const sid2 = esp.log.at(-1).sid;
  esp.release(401, late);
  check(`late ${what} 401 rejects as logged out`, await stale.then(() => false, e => /logged out/.test(e.message)));
  await sleep(20);
  check(`late ${what} 401: the old body never reaches the ESP`, !esp.log.some(e => e.body === '{"stale":1}'));
  const next = await auth.request("POST", "/api/node", "{}");
  check(`late ${what} 401: login 2 keeps its session`, next.ok && esp.log.at(-1).sid === sid2);
}
{
  // A login superseded while its /api/auth is in flight resolves nothing: it rejects,
  // and leaves the newer state alone (logged out, or the second login's key).
  const esp = mockEsp();
  const auth = loadAuth(esp.fetch);
  esp.holds.add("/api/auth");
  const first = auth.login("checkpoint");
  while (!esp.held.length) await sleep(5);
  auth.logout();
  esp.release(200, { admin: true });
  check("login superseded by a logout rejects", await first.then(() => false, e => /logged out/.test(e.message)));
  check("...and stays logged out", await auth.request("POST", "/api/node", "{}").then(() => false, () => true));

  esp.holds.add("/api/auth");
  const one = auth.login("checkpoint");
  while (!esp.held.length) await sleep(5);
  const two = auth.login("checkpoint");
  esp.release(200, { admin: true });
  check("login 1 superseded by login 2 rejects", await one.then(() => false, e => /logged out/.test(e.message)));
  check("login 2 succeeds", (await two) === true);
  check("...and keeps its key", (await auth.request("POST", "/api/node", "{}")).ok);

  esp.holds.add("/api/auth");
  const right = auth.login("checkpoint");
  while (!esp.held.length) await sleep(5);
  const wrong = auth.login("nope");
  check("a newer wrong login fails", (await wrong) === false);
  esp.release(200, { admin: true });
  check("the older right login, superseded, does not log in", await right.then(() => false, () => true));
  check("...nor leave a key", await auth.request("POST", "/api/node", "{}").then(() => false, () => true));

  // Logout while a send() reopens its session after a session:false: no re-sign afterwards.
  check("login for the reopen case", await auth.login("checkpoint"));
  esp.sessions.clear();
  esp.holds.add("/api/session");
  const reopening = auth.request("POST", "/api/reset", '{"reopen":1}');
  while (!esp.held.length) await sleep(5);
  auth.logout();
  esp.release(200, { sid: "f".repeat(32), salt: esp.salt, iter: ITER, open: false });
  check("a reopen interrupted by a logout rejects", await reopening.then(() => false, e => /logged out/.test(e.message)));
  await sleep(20);
  const sent = esp.log.filter(e => e.body === '{"reopen":1}');
  check("...and is not re-signed after it (only the pre-logout attempt went out)",
        sent.length === 1 && sent[0].sid !== "f".repeat(32));
}
{
  // A reply whose body never arrives is cut by the same timeout as one without headers.
  const esp = mockEsp();
  const auth = loadAuth(esp.fetch);
  auth._test.cfg.timeoutMs = 100;
  esp.bodyStalls.add("/api/session");
  check("a session reply without a body rejects the login", await auth.login("checkpoint").then(() => false, () => true));
  check("login works again afterwards", await auth.login("checkpoint"));
}
{
  // A login whose /api/auth never answers fails and leaves no key behind.
  const esp = mockEsp();
  const auth = loadAuth(esp.fetch);
  auth._test.cfg.timeoutMs = 100;
  esp.stalls.add("/api/auth");
  check("login with a silent ESP rejects", await auth.login("checkpoint").then(() => false, () => true));
  check("...and leaves no key", await auth.request("POST", "/api/node", "{}").then(() => false, e => /not logged in/.test(e.message)));
}
{
  const esp = mockEsp({ open: true });
  const auth = loadAuth(esp.fetch);
  check("no password on the ESP: any login opens admin", (await auth.login("whatever")) === true);
}
{
  const esp = mockEsp({ password: "é ünïcödé \"q\" ${x}" });
  const auth = loadAuth(esp.fetch);
  check("UTF-8 password with quotes and ${...}", (await auth.login("é ünïcödé \"q\" ${x}")) === true);
}

if (fails) { console.log(`${fails} test(s) FAILED`); process.exit(1); }
console.log(`all web_auth page tests passed (${oks})`);
