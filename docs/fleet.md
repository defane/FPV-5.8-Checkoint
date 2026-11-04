# Race-timing fleet (C6 master)

For a multi-gate race, an **ESP32-C6 master** (`firmware/master`) takes over the
C3's brain role and carries the speaker. Each **gate** is a C5; the master's C5
is UART-wired to the C6 (gate 0), the others are each wired to a **C3 node** that
relays over **ESP-NOW** to the C6.

Overall element/interaction diagram: [`fleet.svg`](fleet.svg). Pin-level wiring
per set type: [`wiring-master.svg`](wiring-master.svg) (C6 + C5),
[`wiring-node.svg`](wiring-node.svg) (C3 + C5; also a C3 + C5 with no master),
[`wiring-c5c5.svg`](wiring-c5c5.svg) (temporary C5+C5). Only the master carries
an amplifier; a gate C5 never does. Consolidated pinout: the
[README](../README.md#pinout).

**Temporary C5+C5 set** (until a C6 is available): a second C5 runs the master
firmware built for esp32c5 (`firmware/master`, target esp32c5). The amp moves to
the C5 pins 9/8/7/10 (GPIO 16‑22 are the C5 flash); UART stays 4/5. Build: `make
flash FW=master-wave-c5` (or `idf.py -DIDF_TARGET=esp32c5 -DSDKCONFIG=sdkconfig.c5
-DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults -B build-c5 build` from
`firmware/master`, → `build-c5/master-c5.bin`). Later swap that C5 for a real C6
with no change on the gate side.

- **Protocols:** `components/link_proto` v2 (C5↔bridge UART: `CONFIG_*`,
  `EVENT_PASS`, `STATS` with the C5's health, `SET_CHANNEL` which triggers an
  `rf_set_channel` retune the C5 saves, `COMMAND`); `components/fleet_proto` v2
  (C6↔C3 ESP-NOW: pairing, time beacons, `CONFIG`, `GATE_PASS`, `NODE_STATUS`
  relaying that health, `COMMAND`); `components/espnow_link` (transport);
  `components/race_timing` (laps/sectors per channel). A board left on a v1
  firmware is not misread, it simply does not talk: **reflash every C5, C3 and the
  C6 together** after a protocol change.
- **C6** (`c6_hub`): node table in NVS, periodic time beacons, the `race_timing`
  engine, **centralized sound**, and an **admin web** (AP) to pair, order the
  gates, assign channels and see the timings.
- **Pairing:** C6 web → *pairing open*; an unpaired C3 broadcasts a
  `PAIR_REQUEST`; the C6 assigns it a gate + channel (`PAIR_ACCEPT` + `CONFIG`),
  persisted on both sides.
- **Degraded C3:** if the C6 is unreachable for `CONFIG_C3_FLEET_TIMEOUT_MS` at
  boot, the C3 serves its own local AP + web (band selection → `SET_CHANNEL`,
  pass display).
- **Web roles:** two levels. A **spectator** sees timings/status read-only (no
  login); **admin** actions (pairing, gate order, channel, reset, volume, a
  gate's calibration or reboot; detector tuning and band selection on a degraded
  C3) need the admin password (`CONFIG_*_ADMIN_PASSWORD`, no
  username): the header's spectator/admin toggle opens a password dialog, the
  password goes out as an HTTP Basic header with an empty user and is checked on
  the ESP; toggling back to spectator forgets it. An empty password disables
  admin auth. The firmware never holds the password, only its salted SHA-256
  (generated at build time; 128 bytes max, a longer one fails the build). It
  still crosses the Wi-Fi in clear (plain HTTP): anyone on the AP can sniff it.
- **Constraint:** the C6 and C3 APs **and** ESP-NOW share the **same Wi-Fi
  channel** (`CONFIG_*_AP_CHANNEL` / `CONFIG_C6_AP_CHANNEL`).
- **Limit:** the time base (ESP-NOW beacons) aims for ~10 ms; not a sub-ms photo
  finish.

## Gate health and remote commands (no console in production)

Nobody opens a detector C5's USB console in production, so everything an operator
needs from it goes through its bridge: the C6 admin web for the fleet (gate 0 is
its own C5 over UART, the others go through their C3), or the degraded C3's own
page for a gate with no master.

- **Health**, from the C5's `STATS` every `CONFIG_CHECKPOINT_LINK_STATS_MS`
  (relayed in `NODE_STATUS`): a calibration badge (OK / calibration needed, i.e.
  `CAL_NEEDED`/`DC_FAULT` / calibrating), RX gain, passes since boot, C5 uptime,
  lowest free heap, transport fault counters and the firmware id. A C5 silent for
  **3 s** is shown as absent: no stale health, and commands to it fail.
- **Commands** (admin): *Cal B* = the console's `B` (full RF optimisation),
  *Cal F* = `F` (quick filter calibration), *Reboot*; on the C6 also *Calibrate
  all gates (B)*. They call the same engine functions as the console
  (`filter_request_full()`, `filter_request()`), so the engine still refuses a
  calibration that is already running.
- **Guard:** a calibration blinds the gate for minutes and wants the VTXs off.
  The page asks for confirmation; then the bridge refuses, unless forced, any
  gate whose C/N is above that C5's activity threshold (`active_db`, read from
  its `CONFIG_STATE` echo), i.e. a VTX is probably on. The C6 also skips gates
  already calibrating (listed as *busy*); the C3 page disables its calibration
  buttons meanwhile. Reboot is not VTX-guarded.
- **API:** C6 `POST /api/command` `{"gate": 0..15 | -1 (all), "cmd": "cal_full" |
  "cal_quick" | "reboot", "force": 0|1}` → `{"ok": true, "sent": [...], "failed":
  [...], "busy": [...]}` or `{"ok": false, "vtx": [{"gate": g, "cn_x10": x}]}`. The
  degraded C3's `POST /api/command` `{"cmd": ..., "force": 0|1}` (its one gate)
  replies `{"ok": true}`, `{"ok": false, "vtx": true, "cn_x10": x}` or
  `{"ok": false, "msg": "no C5 linked"}`.
- **Channel persistence:** a channel set over the link (`SET_CHANNEL`) is saved by
  the C5, and the C6 re-sends a node's `CONFIG` (gate, channel, detector settings)
  when the node comes back online, so a rebooted gate returns to its assignment.
- **Known limitation:** a retune over the link does not re-apply the receive
  profile (filter class) of the new channel, as the console's `c`/`b` do through
  `control.c`'s `after_retune()`; that needs a `control.c` change.
