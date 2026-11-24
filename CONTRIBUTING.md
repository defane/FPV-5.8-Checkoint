# Contributing

Contributions are welcome under the repository's `GPL-3.0-only` public license.
Preserve contributor attribution and third-party license notices.

Three firmwares build from `firmware/detector`, `firmware/node` and
`firmware/master` on the shared `components/`. Read [CLAUDE.md](CLAUDE.md) and
[AGENTS.md](AGENTS.md) first: AGENTS.md is the reuse/attribution policy. The
detector's receive engine, `components/sdr_rx`, is ported from widscan58
(CapnBry) and esp-sdr (ESPARGOS); its ported files keep their GPL-3.0-or-later
notices and list their changes in their headers. A change to its radio files
(`sdr_radio_c5.c`, `sdr_recal_c5.c`), `hot_iram.lf`, `sram_guard.ld` or the
`--wrap` list needs a board bench ([docs/bench.md](docs/bench.md) "Engine
bench") and a line in that file's header. Clearly distinguish simulation, bounded
hardware tests, and long-duration physical proof.

The project does not currently claim a Contributor License Agreement or
copyright assignment. If commercial dual licensing becomes a project goal, a
prospective contribution-rights policy should be established explicitly for
future contributions; it must not be applied retroactively without consent.
