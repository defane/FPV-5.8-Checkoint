// Admin request signing for the C3/C6 pages (protocol: components/web_auth/include/web_auth.h).
// The password never leaves the browser: login derives K = PBKDF2-HMAC-SHA256(password,
// salt, iterations) once and forgets the password; each admin request carries
// HMAC-SHA256(K, sid "\n" seq "\n" method "\n" path "\n" body) in a session the ESP
// opened. crypto.subtle only exists on HTTPS or localhost, not on http://192.168.4.1,
// hence SHA-256 below.
"use strict";
const AdminAuth = (() => {
  const RK = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  const H0 = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const ror = (x, n) => x >>> n | x << (32 - n);
  const w = new Uint32Array(64);               // typed-array stores wrap mod 2^32

  // One SHA-256 compression of the 16-word block blk into the state h, in place.
  function compress(h, blk) {
    w.set(blk);
    for (let i = 16; i < 64; i++)
      w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >>> 3)
           + w[i - 7] + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >>> 10);
    let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (let i = 0; i < 64; i++) {
      const t1 = (k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + (e & f ^ ~e & g) + RK[i] + w[i]) >>> 0;
      const t2 = ((ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + (a & b ^ a & c ^ b & c)) >>> 0;
      k = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
  }

  const bytes = h => { const o = new Uint8Array(32), v = new DataView(o.buffer);
                       for (let i = 0; i < 8; i++) v.setUint32(4 * i, h[i]); return o; };

  function sha256(msg) {                       // Uint8Array -> Uint8Array(32)
    const len = msg.length, total = (len + 72) & ~63;   // + 0x80, 8-byte length, padded to 64
    const m = new Uint8Array(total);
    m.set(msg); m[len] = 0x80;
    const dv = new DataView(m.buffer), blk = new Uint32Array(16), h = H0.slice();
    dv.setUint32(total - 8, Math.floor(len / 0x20000000)); dv.setUint32(total - 4, len << 3 >>> 0);
    for (let o = 0; o < total; o += 64) {
      for (let i = 0; i < 16; i++) blk[i] = dv.getUint32(o + 4 * i);
      compress(h, blk);
    }
    return bytes(h);
  }

  function hmac(key, msg) {                    // RFC 2104, 64-byte block
    const k = new Uint8Array(64);
    k.set(key.length > 64 ? sha256(key) : key);
    const inner = new Uint8Array(64 + msg.length), outer = new Uint8Array(64 + 32);
    for (let i = 0; i < 64; i++) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
    inner.set(msg, 64);
    outer.set(sha256(inner), 64);
    return sha256(outer);
  }

  // PBKDF2-HMAC-SHA256 (RFC 8018), one 32-byte block. Past U1 each iteration is two
  // compressions from the key's precomputed inner/outer pad states.
  function pbkdf2(password, salt, iterations) {
    const k = new Uint8Array(64);
    k.set(password.length > 64 ? sha256(password) : password);
    const padState = x => {
      const blk = new Uint32Array(16), h = H0.slice();
      for (let i = 0; i < 16; i++)
        blk[i] = (k[4 * i] ^ x) << 24 | (k[4 * i + 1] ^ x) << 16 | (k[4 * i + 2] ^ x) << 8 | (k[4 * i + 3] ^ x);
      compress(h, blk);
      return h;
    };
    const ist = padState(0x36), ost = padState(0x5c);
    const s1 = new Uint8Array(salt.length + 4);
    s1.set(salt); s1[salt.length + 3] = 1;      // INT(1), big-endian
    const u1 = new DataView(hmac(password, s1).buffer);
    const u = new Uint32Array(8), t = new Uint32Array(8), h = new Uint32Array(8);
    for (let i = 0; i < 8; i++) u[i] = t[i] = u1.getUint32(4 * i);
    const blk = new Uint32Array(16);
    for (let n = 1; n < iterations; n++) {
      blk.set(u); blk[8] = 0x80000000; blk.fill(0, 9, 15); blk[15] = (64 + 32) * 8;
      h.set(ist); compress(h, blk);            // inner hash of U(n-1)
      blk.set(h);
      h.set(ost); compress(h, blk);            // outer: U(n)
      u.set(h);
      for (let i = 0; i < 8; i++) t[i] ^= h[i];
    }
    return bytes(t);
  }

  const utf8 = s => new TextEncoder().encode(s);
  const hex = b => Array.from(b, x => x.toString(16).padStart(2, "0")).join("");
  const sign = (key, sid, seq, method, path, body) =>
    hex(hmac(key, utf8(`${sid}\n${seq}\n${method}\n${path}\n${body}`)));
  const cfg = { timeoutMs: 10000 };            // a request the ESP never answers

  // gen counts logouts: every step of a request or a login re-checks, after each
  // await, that no logout came in between, so nothing started before a logout
  // reads or rewrites the next login's key, session or seq.
  let key = null, sid = null, seq = 0, gen = 0, queue = Promise.resolve();
  const live = g => { if (g !== gen) throw new Error("logged out"); };

  // The whole exchange, body included, within cfg.timeoutMs (the replies are small JSON).
  async function timedFetch(path, init) {
    const ctl = new AbortController(), timer = setTimeout(() => ctl.abort(), cfg.timeoutMs);
    try {
      const r = await fetch(path, { ...init, signal: ctl.signal });
      return new Response(await r.arrayBuffer(), { status: r.status, statusText: r.statusText, headers: r.headers });
    } finally { clearTimeout(timer); }
  }

  // Lets the page paint (e.g. "checking...") before the synchronous derivation.
  const paint = () => new Promise(r => {
    const t = setTimeout(r, 100);              // rAF never fires in a hidden tab
    if (typeof requestAnimationFrame === "function")
      requestAnimationFrame(() => setTimeout(() => { clearTimeout(t); r(); }, 0));
  });

  async function open(g) {
    const r = await timedFetch("/api/session", { method: "POST" });
    if (!r.ok) throw new Error("session " + r.status);
    const s = await r.json();
    live(g);
    sid = s.sid; seq = 0;
    return s;
  }

  // One request at a time, so the ESP sees each session's seq in order.
  async function send(g, method, path, body) {
    for (let retry = false; ; retry = true) {
      if (!sid) await open(g);
      live(g);
      const n = String(++seq);                 // spent even if the fetch fails: never reused
      const headers = { "X-Auth-Session": sid, "X-Auth-Seq": n,
                        "X-Auth-Sig": sign(key, sid, n, method, path, body) };
      if (method !== "GET") headers["Content-Type"] = "application/json";
      const r = await timedFetch(path, { method, headers, body: method === "GET" ? undefined : body });
      if (r.status === 401) live(g);           // a stale 401 must not log the next login out
      if (r.status !== 401 || retry) return r;
      const j = await r.clone().json().catch(() => ({}));
      if (j.session !== false) return r;       // bad signature: the caller drops admin
      live(g);
      sid = null;                              // session evicted or ESP rebooted: reopen once
    }
  }

  // Queued requests belong to their login: one made before a logout never runs after.
  function request(method, path, body = "") {
    if (!key) return Promise.reject(new Error("not logged in"));
    const g = gen;
    const p = queue.then(() => { live(g); return send(g, method, path, body); });
    queue = p.catch(() => {});
    return p;
  }

  function logout() { key = sid = null; seq = 0; gen++; queue = Promise.resolve(); }

  // Resolves true when the ESP accepts the password (any one when it has none);
  // a failed or interrupted login leaves no key behind.
  async function login(password) {
    logout();
    const g = gen;
    try {
      const s = await open(g);
      await paint();
      live(g);
      key = pbkdf2(utf8(password), utf8(s.salt), s.iter);
      const r = await request("GET", "/api/auth", "");
      live(g);                                 // superseded by a logout or another login
      if (!r.ok) logout();
      return r.ok;
    } catch (e) {
      if (g === gen) logout();
      throw e;
    }
  }

  return { login, logout, request, _test: { sha256, hmac, pbkdf2, sign, cfg } };   // test/test_web_auth.mjs
})();
