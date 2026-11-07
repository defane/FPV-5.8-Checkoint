# Status LED

Each board with an addressable WS2812 shows its state via the
`components/status_led` component: a pure-C state→colour core (host-tested,
`test/test_status_led.c`) + a WS2812 driver over the RMT bytes encoder (no
external dependency), a priority-1 render task started after the splash, ~15%
brightness. At boot the task plays a **🔴 red → 🟢 green → 🔵 blue self-test**
(~0.4 s each): a glance confirms a new board's wiring, pin and colour order (and
tells the WS2812 apart from a separate power LED).

Onboard pins: **node** C3-Zero **GP10**, **master** C6-Zero **GP8**
(C5-as-master variant: **GP27**, free because that board runs no receiver).

| State | Colour | Pattern |
| --- | --- | --- |
| boot / self-test | 🔴→🟢→🔵 then 🟡 yellow | blink |
| healthy / operating | 🟢 green | breathing |
| degraded (node: no master · master: a gate dropped) | 🔵 blue | breathing |
| pairing (fleet) | 🟣 magenta | double-pulse |
| recoverable error (node: C5 link lost) | 🔴 red | fast blink |
| fatal error (init) | 🔴 red | solid |
| a pass | ⚪ white | brief flash (priority, even when sound is off) |

Per role:

- **node** (C3-Zero, GP10): 🟢 green when the master is alive (a fleet message
  within 5 s), 🔵 blue in degraded mode (local AP), 🟣 magenta while pairing, 🔴
  red if the UART link to its C5 is silent (> 3 s, even if the C5 never spoke), ⚪ white flash on each pass.
- **master** (C6-Zero, GP8): 🟢 green while running, 🟣 magenta while no gate is
  paired yet, 🔵 blue if a paired gate goes offline, ⚪ white flash on each pass
  (local or remote gate).

No-colour cases:

- **detector** (C5): **no status LED**. The C5-Zero's onboard WS2812 is on
  **GPIO27**, one of the receiver's I/Q-loopback pins (`RF_IQ_GPIO_NUMS`); the
  receive chain comes first, so the LED is disabled
  (`CONFIG_STATUS_LED_ENABLE=n`). *(The C5 has no GPIO29 — the Waveshare pinout's
  "GP29" is wrong.)*
- **ESP32-C3 SuperMini**: no addressable WS2812 (only a mono blue LED on GPIO8 +
  a red power LED). The colour scheme does not apply — use a C3-Zero for a node
  with a status LED.

Settings: `CONFIG_STATUS_LED_*` (ENABLE, GPIO, BRIGHTNESS ~40/255, TICK, STACK);
the pin is set per board in the firmware's `sdkconfig.defaults`.
