"""tools/flash.py (from C5VRX-5, by Twotoz and the C5VRX contributors) never line-resets or syncs a
running detector, and checks a board from its console lines."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

# CI runs this with the container's plain python3, which has no pyserial, so
# the monitor tests swap in this stand-in for the serial module.
flash_serial = types.ModuleType("serial")
flash_serial.SerialException = type("SerialException", (OSError,), {})
flash_serial.SerialTimeoutException = type("SerialTimeoutException", (flash_serial.SerialException,), {})

spec = importlib.util.spec_from_file_location("flash", Path(__file__).parents[1] / "tools/flash.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def run(argv, key_ack=True, has_esptool=True, flash_args=("flash_args",), rc=0):
    """main() with the board side mocked; returns (rc, key mock, esptool mock, output)."""
    with tempfile.TemporaryDirectory() as build:
        for name in flash_args:
            (Path(build) / name).write_text("--flash-mode dio\n0x10000 detector-c5.bin\n")
        key = mock.Mock(return_value=key_ack)
        esptool = mock.Mock(return_value=mock.Mock(returncode=rc))
        out = io.StringIO()
        with mock.patch.object(flash, "send_download_key", key), \
                mock.patch.object(flash, "wait_for_port"), \
                mock.patch.object(flash.subprocess, "run", esptool), \
                mock.patch.object(flash.importlib.util, "find_spec",
                                  return_value=object() if has_esptool else None), \
                contextlib.redirect_stdout(out):
            code = flash.main(argv + ["--build", build])
        return code, key, esptool, out.getvalue(), build


class FlashToolTest(unittest.TestCase):
    def test_command_has_no_line_reset(self):
        cmd = flash.esptool_cmd("COM6", 460800)
        self.assertEqual(cmd[cmd.index("--before") + 1], "no-reset")
        self.assertEqual(cmd[cmd.index("--after") + 1], "watchdog-reset")
        self.assertEqual(cmd[cmd.index("--chip") + 1], "esp32c5")
        self.assertEqual(cmd[-2:], ["write-flash", "@flash_args"])

    def test_key_then_flash_from_build_dir(self):
        code, key, esptool, _, build = run(["COM6"])
        self.assertEqual(code, 0)
        key.assert_called_once_with("COM6")
        self.assertEqual(esptool.call_args.kwargs["cwd"], Path(build))

    def test_no_ack_flashes_nothing(self):
        code, _, esptool, out, _ = run(["COM6"], key_ack=False)
        self.assertEqual(code, 1)
        esptool.assert_not_called()
        self.assertIn("--no-key", out)

    def test_no_key_skips_the_key(self):
        code, key, esptool, _, _ = run(["COM6", "--no-key"])
        self.assertEqual(code, 0)
        key.assert_not_called()
        esptool.assert_called_once()

    def test_checks_run_before_the_key(self):
        # The key reboots the detector into the loader, so a missing build or
        # esptool must stop the tool before it is sent.
        for kw in ({"flash_args": ()}, {"has_esptool": False}):
            code, key, esptool, _, _ = run(["COM6"], **kw)
            self.assertEqual(code, 2)
            key.assert_not_called()
            esptool.assert_not_called()

    def test_app_only_uses_flash_app_args(self):
        code, _, esptool, _, _ = run(["COM6", "--app-only"], flash_args=("flash_args", "flash_app_args"))
        self.assertEqual(code, 0)
        self.assertEqual(esptool.call_args.args[0][-1], "@flash_app_args")
        # Only the full set present: --app-only must not fall back to it.
        code, key, _, out, _ = run(["COM6", "--app-only"])
        self.assertEqual(code, 2)
        key.assert_not_called()
        self.assertIn("flash_app_args", out)

    def test_esptool_failure_is_returned(self):
        code, _, _, out, _ = run(["COM6"], rc=2)
        self.assertEqual(code, 2)
        self.assertIn("--no-key", out)

    def test_monitor_only_after_a_good_flash(self):
        for rc, calls in ((0, 1), (2, 0)):
            mon = mock.Mock(return_value=0)
            with mock.patch.object(flash, "monitor", mon), \
                    mock.patch.object(flash, "console_keys", lambda: contextlib.nullcontext(None)):
                code, *_ = run(["COM6", "--monitor"], rc=rc)
            self.assertEqual(code, rc)
            self.assertEqual(mon.call_count, calls)

    def test_monitor_only_needs_no_build_and_flashes_nothing(self):
        mon = mock.Mock(return_value=0)
        with mock.patch.object(flash, "monitor", mon), \
                mock.patch.object(flash, "console_keys", lambda: contextlib.nullcontext(None)):
            code, key, esptool, _, _ = run(["COM6", "--monitor-only"], flash_args=(), has_esptool=False)
        self.assertEqual(code, 0)
        key.assert_not_called()
        esptool.assert_not_called()
        mon.assert_called_once()

    def test_check_after_flash_and_on_a_running_board(self):
        for argv, flashes, booting in ((["--check"], 1, True), (["--monitor-only", "--check"], 0, False)):
            chk = mock.Mock(return_value=1)
            mon = mock.Mock()
            with mock.patch.object(flash, "run_check", chk), mock.patch.object(flash, "monitor", mon):
                code, _, esptool, _, _ = run(["COM6"] + argv)
            self.assertEqual(code, 1)
            self.assertEqual(esptool.call_count, flashes)
            self.assertEqual(chk.call_args.args[2], booting)
            mon.assert_not_called()   # --check alone exits with its result

    def test_c5vrx_only_options_are_gone(self):
        # IQ snapshots (key I) and the video check have no counterpart on the sdr_rx detector.
        for argv in (["--iq", "x"], ["--iq-every", "5", "--monitor"], ["--check", "--expect-video"]):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                flash.main(["COM6"] + argv)


class FakePort:
    """Serial port double: records line states at open, replays reads, can fail."""
    opened = []

    def __init__(self, reads=(), fail_open=0):
        self.reads, self.fail_open, self.writes = list(reads), fail_open, []

    def __call__(self):
        return self

    def open(self):
        if self.fail_open:
            self.fail_open -= 1
            raise flash_serial.SerialException("gone")
        FakePort.opened.append((self.dtr, self.rts))

    def read(self, n):
        item = self.reads.pop(0) if self.reads else b""
        if isinstance(item, Exception):
            raise item
        return item

    def write(self, b):
        self.writes.append(b)

    def close(self):
        pass



class MonitorTest(unittest.TestCase):
    def setUp(self):
        FakePort.opened = []

    def monitor(self, port, keys, console=None):
        keys = list(keys)
        console = console or flash.Console(io.StringIO())
        flash_serial.Serial = port
        with mock.patch.dict(sys.modules, {"serial": flash_serial}), mock.patch.object(flash.time, "sleep"):
            flash.monitor("COM6", lambda: keys.pop(0) if keys else "\x1d", console)
        return console.out.getvalue()

    def test_opens_with_lines_low_and_forwards_keys(self):
        port = FakePort([b"boot\n", b"", b"T: ch A1\n"])
        out = self.monitor(port, [None, "T", None])
        self.assertEqual(FakePort.opened, [(False, False)])
        self.assertIn("boot\nT: ch A1\n", out)
        self.assertEqual(port.writes, [b"T"])

    def test_exit_keys_are_not_sent(self):
        for key in ("\x1d", "\x03"):
            port = FakePort()
            self.monitor(port, [key, "T"])
            self.assertEqual(port.writes, [])

    def test_reopens_after_reenumeration(self):
        # Open fails while the restart re-enumerates USB, then the port drops mid-read.
        port = FakePort([b"a", flash_serial.SerialException("lost"), b"b"], fail_open=3)
        out = self.monitor(port, [None, None, None])
        self.assertEqual(FakePort.opened, [(False, False), (False, False)])
        self.assertIn("port lost, reopening", out)
        self.assertIn("b", out.split("reopening")[1])

    def test_monitor_does_not_retain_transcript(self):
        console = flash.Console(io.StringIO())
        self.monitor(FakePort([b"SCAN,1,-240,-236,-238,-241,-239,-237,-240,-238\n"] * 100), [None] * 100, console)
        self.assertEqual(console.lines, [])
        self.assertEqual(console.out.getvalue().count("SCAN,1,"), 100)


class ConsoleTest(unittest.TestCase):
    def test_log_is_timestamped_and_scan_analyze_reads_it(self):
        spec = importlib.util.spec_from_file_location("scan_analyze", Path(__file__).parents[1] / "tools/scan_analyze.py")
        analyze = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(analyze)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "bench.log"
            with open(path, "w") as log:
                c = flash.Console(io.StringIO(), log)
                # A K S recording (cp_bench.c), split across reads as USB delivers it.
                c.feed("SCANDUMP begin rotations=28 kept=2 kept_s=0.1 decim=14 format=qdb order=R1+R2,R3+R4,"
                       "R5+R6,R7+R8 gain=40 rot_us=1961 fw=x\r\nSCAN,27454,-240,-236,-238,-241,-239,-237,-240,-238\r\n")
                c.feed("SCAN,54908,-120,-236,-32768,-241,-239,-237,-240,-238\nSCANDUMP end rot")
                c.feed("ations=28 decim=14 format=qdb order=R1+R2,R3+R4,R5+R6,R7+R8 gain=40\n")
                c.note("CHECK result=pass")
            text = path.read_text()
            self.assertRegex(text.splitlines()[0], r"^\d\d:\d\d:\d\d\.\d{3} SCANDUMP begin ")
            self.assertIn(" # CHECK result=pass", text)
            rows, header = analyze.read_scans(text)
            self.assertEqual([t for t, _ in rows], [27454, 54908])
            self.assertEqual(rows[1][1][:3], [-120, -236, None])
            self.assertEqual(analyze.read_dump(header, rows)["rot_us"], 1961.0)

    def test_partial_line_shown_once(self):
        c = flash.Console(io.StringIO())
        c.feed("menu> ")
        c.idle()
        c.feed("ok\n")
        self.assertEqual(c.out.getvalue(), "menu> ok\n")
        self.assertEqual(c.lines, ["menu> ok"])


ELF = "ab" * 32
# The detector's boot and T lines, after the printf formats of firmware/detector/main/main.c,
# components/checkpoint and components/sdr_rx (no board log of the sdr_rx engine yet: the
# values are illustrative).
GOOD = [
    "ESP-ROM:esp32c5-eco2-20250121",
    "rst:0x1 (POWERON),boot:0x18 (SPI_FAST_FLASH_BOOT)",
    f'FIRMWARE id=5.0.0-dev+f5a6999c0ffe version=5.0.0-dev commit=f5a6999c0ffee elf_sha256={ELF} '
    'built="Nov 15 2026 10:00:00" idf=v6.0.2 engine=sdr_rx chip_rev=v1.0',
    "BOOT_TIMING radio_ms=420",
    "CHECKPOINT prefs=defaults gain=40",
    "CHECKPOINT link=on uart=1 tx=6 rx=8 baud=115200",
    "SDR cal lo=5677 ms=44 irr_db=47.9 verify_gain=60 tries=1 dc=6 sat=0",
    "SDR cal lo=5751 ms=43 irr_db=51.2 verify_gain=60 tries=1 dc=5 sat=0",
    "SDR cal lo=5825 ms=43 irr_db=49.6 verify_gain=60 tries=1 dc=7 sat=0",
    "SDR cal lo=5899 ms=44 irr_db=46.3 verify_gain=60 tries=1 dc=6 sat=0",
    "SDR ready lo=5677,5751,5825,5899 gain=40 cal_ms=174 boot_ms=1630 chip_rev=v1.0",
    # T, about 6 s after the boot.
    f'FIRMWARE id=5.0.0-dev+f5a6999c0ffe version=5.0.0-dev commit=f5a6999c0ffee elf_sha256={ELF} '
    'built="Nov 15 2026 10:00:00" idf=v6.0.2 engine=sdr_rx chip_rev=v1.0',
    "STATUS uptime_s=6 heap_internal=61234 heap_largest=31744 heap_min=24100 heap_ok=1 scan_stack_free=1800 "
    "console_stack_free=3900 usb_stack_free=1700 link_stack_free=2100 fw=5.0.0-dev+f5a6999c0ffe",
    "SDR plan=pairs lo=5677,5751,5825,5899 gain=40/83 rot_hz_x10=5100 rot_us=1961/2350 tune_us=225/310 "
    "cap_us=24 wait_us=1 dsp_us=237/280 faults=0 timeout=0 short=0 overrun=0 dsp=0 stale=0 recal=4 "
    "foreign_dc=0 dc_max=7 iq_bad=0x0 cal_needed=0 calibrating=0 init_failed=0 phy=1200,abcdef0,Oct 1 2025",
    "CHECKPOINT source=sdr live=1 learning=0 bench=0 engine_failed=0 stalled=0 stalls=0 gain=40 top=R1 "
    "rise_db=0.3 state=IDLE rotations=2450 readings=19600 passes=0 alpha=0.25 tau_ms=60.8 active_db=8.0 "
    "hyst_db=3.0 drop_db=4.0 min_peak_db=10.0 refractory_ms=1000 stale_ms=1000",
]
READY = next(i for i, line in enumerate(GOOD) if line.startswith("SDR ready"))
AFTER_BOOT = GOOD[READY + 1:]   # what T prints


class EvaluateTest(unittest.TestCase):
    def test_healthy_board_passes(self):
        problems, warnings, fields = flash.evaluate(GOOD, True, ELF)
        self.assertEqual((problems, warnings), ([], []))
        self.assertEqual(fields["elf_match"], 1)
        self.assertEqual((fields["gain"], fields["rot_hz_x10"], fields["faults"], fields["cal_needed"]),
                         ("40/83", "5100", "0", "0"))

    def fails(self, lines, booting=True, elf=ELF, **kw):
        return " | ".join(flash.evaluate(lines, booting, elf, **kw)[0])

    def swap(self, prefix, new):
        return [new if line.startswith(prefix) else line for line in GOOD]

    def test_failures(self):
        self.assertIn("not the flashed build", self.fails(GOOD, elf="cd" * 32))
        self.assertIn("did not finish booting", self.fails([line for line in GOOD if "SDR ready" not in line]))
        self.assertIn("rebooted after the boot", self.fails(GOOD + ["rst:0xc (SW_CPU),boot:0x18"]))
        self.assertIn("crash", self.fails(GOOD + ["Guru Meditation Error: Core 0 panic'ed"]))
        self.assertIn("crash", self.fails(GOOD + ["E (12000) task_wdt: Task watchdog got triggered."]))
        self.assertIn("engine faults=3", self.fails([line.replace(" faults=0 ", " faults=3 ") for line in GOOD]))
        self.assertIn("did not answer T", self.fails([line for line in GOOD if "FIRMWARE" not in line]))
        # The boot prints a FIRMWARE line too: only the SDR plan= line proves T was answered.
        self.assertEqual(self.fails(GOOD[:READY + 1]), "no SDR plan= line: the board did not answer T")
        self.assertIn("engine health was not confirmed", self.fails([line.replace(" faults=0 ", " ") for line in GOOD]))

    def test_failed_engine_init_is_named(self):
        # sdr_rx_init failed: no SDR ready; T still answers (CAL_NEEDED, a fault, init_failed=1).
        lines = [line for line in GOOD[:READY] if not line.startswith("SDR cal")] + [
            "SDR cal lo=5677 err=ESP_ERR_TIMEOUT", "SDR init error err=ESP_ERR_TIMEOUT",
            "SDR init failed: no detection; console stays up"] + [
            line.replace(" faults=0 ", " faults=1 ").replace("cal_needed=0", "cal_needed=1")
                .replace("init_failed=0", "init_failed=1") for line in AFTER_BOOT]
        problems, warnings, _ = flash.evaluate(lines, True, ELF)
        self.assertIn("did not finish booting (SDR init failed: no detection; console stays up)", problems[0])
        self.assertIn("engine faults=1", " | ".join(problems))
        self.assertIn("cal_needed=1", " | ".join(warnings))

    def test_running_board_check_skips_boot_and_image(self):
        self.assertEqual(self.fails(AFTER_BOOT, booting=False, elf=None), "")

    def test_running_board_reboot_fails(self):
        self.assertIn("rebooted", self.fails(AFTER_BOOT + [GOOD[1]], booting=False, elf=None))

    def test_boot_loop_reported_once_with_a_count(self):
        # Board, 2026-10-26 (C5VRX image): a stale sdkconfig ran out of heap at every boot.
        crash = ["ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at 0x420182f6",
                 "abort() was called at PC 0x4080c967 on core 0", "Rebooting...",
                 "ESP-ROM:esp32c5-eco2-20250121", "rst:0xc (SW_CPU),boot:0x18 (SPI_FAST_FLASH_BOOT)"]
        problems = flash.evaluate(GOOD[:3] + crash * 3 + GOOD[3:], True, ELF)[0]
        self.assertEqual(len(problems), 2)
        self.assertIn("ESP_ERR_NO_MEM", problems[0])
        self.assertTrue(all(p.endswith("(x3)") for p in problems))

    def test_cal_needed_only_warns(self):
        problems, warnings, fields = flash.evaluate(self.swap("SDR plan=", GOOD[-2].replace(
            "cal_needed=0", "cal_needed=1")), True, ELF)
        self.assertEqual(problems, [])
        self.assertEqual(len(warnings), 1)
        self.assertIn("cal_needed=1", warnings[0])
        self.assertEqual(fields["cal_needed"], "1")

    def test_heap_read_from_status_and_warned_when_low(self):
        _, warnings, fields = flash.evaluate(GOOD, True, ELF)
        self.assertEqual((fields["heap_largest"], fields["heap_min"]), (31744, 24100))
        low = [line.replace("heap_largest=31744", "heap_largest=6144").replace("heap_min=24100", "heap_min=900")
               for line in GOOD]
        problems, warnings, _ = flash.evaluate(low, True, ELF)
        self.assertEqual(problems, [])   # a warning, not a failure
        self.assertEqual([w.split("=")[0] for w in warnings], ["heap_largest", "heap_min"])
        _, warnings, _ = flash.evaluate(low, True, ELF, heap_warn=4096, heap_min_warn=512)
        self.assertEqual(warnings, [])

    def test_firmware_without_heap_min(self):
        old = [line.replace(" heap_min=24100", "") for line in GOOD]
        _, warnings, fields = flash.evaluate(old, True, ELF)
        self.assertNotIn("heap_min", fields)
        self.assertEqual(warnings, [])


class ConfigDriftTest(unittest.TestCase):
    def build(self, defaults, config):
        d = Path(self.tmp.name)
        (d / "build" / "config").mkdir(parents=True)
        (d / "sdkconfig.defaults").write_text(defaults)
        (d / "build" / "project_description.json").write_text(json.dumps({"project_path": str(d)}))
        (d / "build" / "config" / "sdkconfig.json").write_text(json.dumps(config))
        return d / "build"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_defaults_parsed_like_sdkconfig_json(self):
        self.assertEqual(flash.defaults_values('# note\nCONFIG_A=y\nCONFIG_B=n\n# CONFIG_C is not set\n'
                                               'CONFIG_D=0x10\nCONFIG_E="esp32c5"\nCONFIG_F=2\n'),
                         {"A": True, "B": False, "C": False, "D": 16, "E": "esp32c5", "F": 2})

    def test_stale_sdkconfig_reported(self):
        # The 2026-10-26 case: an sdkconfig from before the defaults cut the RX buffers.
        build = self.build("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=2\nCONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n\n"
                           "CONFIG_ESP_COEX_SW_COEXIST_ENABLE=n\n",
                           {"ESP_WIFI_STATIC_RX_BUFFER_NUM": 4, "ESP_WIFI_AMPDU_RX_ENABLED": False})
        self.assertEqual(flash.config_drift(build),
                         ["ESP_WIFI_STATIC_RX_BUFFER_NUM=4 (sdkconfig.defaults: 2)"])

    def test_no_project_description_is_no_drift(self):
        self.assertEqual(flash.config_drift(Path(self.tmp.name)), [])

    def test_board_overlay_overrides_base_defaults(self):
        build = self.build("CONFIG_SPIRAM=n\nCONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=2\n",
                           {"SPIRAM": True, "ESP_WIFI_STATIC_RX_BUFFER_NUM": 4})
        project = build.parent
        (project / "sdkconfig.psram.defaults").write_text("CONFIG_SPIRAM=y\n")
        (build / "project_description.json").write_text(json.dumps({
            "project_path": str(project),
            "config_defaults": "sdkconfig.defaults;sdkconfig.psram.defaults"}))
        self.assertEqual(flash.config_drift(build),
                         ["ESP_WIFI_STATIC_RX_BUFFER_NUM=4 (sdkconfig.defaults: 2)"])

    def test_drift_warned_before_flashing(self):
        drift = mock.Mock(return_value=["ESP_WIFI_STATIC_RX_BUFFER_NUM=4 (sdkconfig.defaults: 2)"])
        with mock.patch.object(flash, "config_drift", drift):
            code, _, esptool, out, _ = run(["COM6"])
        self.assertEqual(code, 0)
        esptool.assert_called_once()   # a warning: the flash still runs
        self.assertIn("sdkconfig differs from sdkconfig.defaults: ESP_WIFI_STATIC_RX_BUFFER_NUM=4", out)


class OrderedPort(FakePort):
    """FakePort that also records how many reads were still queued at each write."""

    def __init__(self, reads=(), fail_open=0):
        super().__init__(reads, fail_open)
        self.unread_at_write = []

    def write(self, b):
        self.unread_at_write.append(len(self.reads))
        super().write(b)


class CheckBoardTest(unittest.TestCase):
    def check(self, reads, booting=True):
        FakePort.opened = []
        port = OrderedPort(reads, fail_open=2)
        flash_serial.Serial = port
        clock = (x * 0.1 for x in range(100000))
        console = flash.Console(io.StringIO())
        with mock.patch.dict(sys.modules, {"serial": flash_serial}), mock.patch.object(flash.time, "sleep"), \
                mock.patch.object(flash.time, "monotonic", lambda: next(clock)):
            lines = flash.check_board("COM6", console, booting)
        return port, lines, next(clock)

    def test_waits_for_sdr_ready_then_sends_t(self):
        boot = "\n".join(GOOD[:READY + 1]) + "\n"
        port, lines, _ = self.check([b"", boot[:150].encode(), b"", boot[150:].encode()] +
                                    [b""] * 5 + ["\n".join(AFTER_BOOT).encode() + b"\n"])
        self.assertEqual(port.writes, [b"T"])   # no D: the sdr_rx console has no such key
        self.assertTrue(all(o == (False, False) for o in FakePort.opened))
        self.assertEqual(lines, GOOD)

    def test_t_is_sent_only_after_sdr_ready(self):
        # Quiet reads while the LOs calibrate: T goes out only once SDR ready has been read.
        boot = "\n".join(GOOD[:READY]) + "\n"
        port, lines, _ = self.check([boot.encode()] + [b""] * 20 + [GOOD[READY].encode() + b"\n"])
        self.assertEqual(port.writes, [b"T"])
        self.assertEqual(port.unread_at_write, [0])
        self.assertEqual(lines, GOOD[:READY + 1])

    def test_a_failed_engine_init_ends_the_wait(self):
        # No SDR ready will come: T goes out at once instead of after the 60 s boot timeout.
        _, lines, t = self.check([b"SDR init failed: no detection; console stays up\n"])
        self.assertLess(t, 10.0)
        self.assertEqual(lines, ["SDR init failed: no detection; console stays up"])


if __name__ == "__main__":
    unittest.main()
