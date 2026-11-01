# Race-timing fleet (C6 master)

For a multi-gate race, an **ESP32-C6 master** (`firmware/master`) takes over the
C3's brain role and carries the speaker. Each **gate** is a C5; the master's C5
is UART-wired to the C6 (gate 0), the others are each wired to a **C3 node** that
relays over **ESP-NOW** to the C6.

Overall element/interaction diagram: [`fleet.svg`](fleet.svg). Pin-level wiring
per set type: [`wiring-master.svg`](wiring-master.svg) (C6 + C5),
[`wiring-node.svg`](wiring-node.svg) (C3 + C5),
[`wiring-c5c5.svg`](wiring-c5c5.svg) (temporary C5+C5),
[`wiring.svg`](wiring.svg) (standalone C5). Consolidated pinout: the
[README](../README.md#pinout).

**Temporary C5+C5 set** (until a C6 is available): a second C5 runs the master
firmware built for esp32c5 (`firmware/master`, target esp32c5). The amp moves to
the C5 pins 9/8/7/10 (GPIO 16‑22 are the C5 flash); UART stays 4/5. Build: `make
flash FW=master-wave-c5` (or `idf.py -DIDF_TARGET=esp32c5 -DSDKCONFIG=sdkconfig.c5
-DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults -B build-c5 build` from
`firmware/master`, → `build-c5/master-c5.bin`). Later swap that C5 for a real C6
with no change on the gate side.

- **Protocols:** `components/link_proto` (C5↔bridge UART, incl. `SET_CHANNEL`
  which triggers an `rf_set_channel` retune on the C5); `components/fleet_proto`
  (C6↔C3 ESP-NOW: pairing, time beacons, `CONFIG`, `GATE_PASS`, `NODE_STATUS`);
  `components/espnow_link` (transport); `components/race_timing` (laps/sectors
  per channel).
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
  login); **admin** actions (pairing, gate order, channel, reset; band selection
  on a degraded C3) need a login (`CONFIG_*_ADMIN_USER`/`_PASSWORD`, HTTP Basic
  header checked on the ESP). An empty password disables admin auth.
- **Constraint:** the C6 and C3 APs **and** ESP-NOW share the **same Wi-Fi
  channel** (`CONFIG_*_AP_CHANNEL` / `CONFIG_C6_AP_CHANNEL`).
- **Limit:** the time base (ESP-NOW beacons) aims for ~10 ms; not a sub-ms photo
  finish.
