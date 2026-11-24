#!/usr/bin/env python3
"""Flash the detector (ESP32-C5) over USB without a line reset.

From C5VRX-5's tools/flash.py (Twotoz and the C5VRX contributors).
`idf.py flash` resets the chip through the USB DTR/RTS lines. That reset does
not reset the C5's modem domain: on a running C5VRX image it landed in a ROM
loader that no longer answered, esptool stopped with a pySerial "Write
timeout" and the board stayed hung until USB and battery power were both
removed. The detector keeps the route that worked:

1. the console download key '`' (tools/enter_download.py): the firmware
   stops its captures and Wi-Fi and reboots into the ROM loader itself;
2. esptool --before no-reset write-flash @flash_args, so no line reset
   reaches the chip and no esptool sync is sent to live firmware;
3. --after watchdog-reset: the LP watchdog starts the new image (an RTS
   reset would leave a C5 in the loader).

    ( cd firmware/detector && idf.py build )
    python tools/flash.py COM6
    python tools/flash.py /dev/ttyACM0 --no-key   # board already in the ROM loader
    python tools/flash.py COM6 --app-only         # app image only (flash_app_args)
    python tools/flash.py COM6 --monitor          # then show the console
    python tools/flash.py COM6 --monitor-only     # console only, no flashing
    python tools/flash.py COM6 --check            # flash, then a pass/fail health check
    python tools/flash.py COM6 --monitor-only --log bench.log
    python tools/flash.py COM6 --build firmware/detector/build-ipex   # a variant

The default build is firmware/detector/build; --build picks a variant's.
--app-only skips the bootloader and partition table, so it is faster for
firmware-only changes; use the full set after a partition or bootloader
change, or when switching from another generation.

--monitor replaces `idf.py flash monitor`, whose monitor resets the chip
through the same lines. It reopens the port with DTR and RTS low, prints
the console at 115200 and sends each key as typed (no Enter, as the
detector expects). Ctrl+] or Ctrl+C exits. --monitor-only opens the same
console without flashing (no build or esptool needed).

--log FILE appends every console line with a local timestamp
(HH:MM:SS.mmm), plus the keys sent and the tool's own notes as '#' lines.
tools/scan_analyze.py reads these logs directly (a K S scan recording).

--check waits for the boot (SDR ready), sends T and exits 1 if the board
crashed or rebooted, runs another image than the one flashed (the FIRMWARE
elf_sha256 prefix), did not answer T (no FIRMWARE or SDR plan= line) or
reports engine faults (SDR faults=, capture faults since boot). cal_needed=1
is a warning: an LO's DC offset or IQ estimate is out of range (F with the
VTX off). It ends with one 'CHECK result=pass|fail ...' line for scripts.
With --monitor-only it checks the running board without waiting for a boot
or comparing the image.

The check also reads the heap from T's STATUS line and warns when the
largest free block is below --heap-warn (8192 bytes) or the lowest free
heap since boot (heap_min, which includes the boot peak of the Wi-Fi init a
reading after the boot cannot show) is below --heap-min-warn (1024 bytes).

Before flashing, every value set in sdkconfig.defaults is compared with the
build's configuration. ESP-IDF never reapplies the defaults to an existing
sdkconfig, so an old one keeps old values: on 2026-10-26 a stale
ESP_WIFI_STATIC_RX_BUFFER_NUM=4 (defaults: 2) cost 3.4 KB of heap and the
board boot-looped with ESP_ERR_NO_MEM. A difference is a warning (delete
sdkconfig and rebuild); it does not stop the flash.

Run it in the ESP-IDF environment, which provides esptool and pyserial.
"""
import argparse
import contextlib
import datetime
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_BUILD = HERE.parent / "firmware" / "detector" / "build"
sys.path.insert(0, str(HERE))   # enter_download

KV = re.compile(r"(\w+)=(\S+)")
BOOT_DONE = ("SDR ready",)
BOOT_FAILED = ("SDR init failed", "BOOT failed")
BOOT_BANNER = ("rst:0x",)
CRASH = ("Guru Meditation", "ESP_ERROR_CHECK failed", "abort() was called", "task_wdt")
T_DONE = ("LETZTIMER source=",)   # the last line T prints


def esptool_cmd(port, baud, args_file="flash_args"):
    return [sys.executable, "-m", "esptool", "--chip", "esp32c5", "-p", port, "-b", str(baud),
            "--before", "no-reset", "--after", "watchdog-reset", "write-flash", "@" + args_file]


def send_download_key(port):
    from enter_download import enter_download
    return enter_download(port)


def wait_for_port(port, timeout_s=6.0, settle_s=1.0):
    """The ROM loader re-enumerates on USB after the restart; wait until it is back."""
    from serial.tools import list_ports
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if any(p.device.lower() == port.lower() for p in list_ports.comports()):
            break
        time.sleep(0.2)
    time.sleep(settle_s)


def open_quiet(port):
    """Open the console with DTR and RTS low before the open, so it never resets the chip."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0.05, 0.5
    s.dtr = False
    s.rts = False
    s.open()
    return s


def elf_sha256(build):
    """SHA-256 of the built app ELF, as the firmware prints it in FIRMWARE elf_sha256=."""
    with contextlib.suppress(OSError, ValueError, KeyError):
        desc = json.loads((build / "project_description.json").read_text())
        return hashlib.sha256((build / desc["app_elf"]).read_bytes()).hexdigest()
    return None


def defaults_values(text):
    """CONFIG_ values of an sdkconfig.defaults text, typed like sdkconfig.json."""
    want = {}
    for line in text.splitlines():
        m = (re.fullmatch(r"CONFIG_(\w+)=(.*)", line.strip()) or
             re.fullmatch(r"# CONFIG_(\w+) is not set()", line.strip()))
        if not m:
            continue
        name, v = m.groups()
        with contextlib.suppress(ValueError):
            want[name] = (v == "y" if v in ("y", "n", "") else v[1:-1] if v.startswith('"') else int(v, 0))
    return want


def kconfig_choices(build):
    """Each member of a Kconfig choice -> every member of that choice (build/config/kconfig_menus.json).

    {} when the build has no menus file.
    """
    members = {}

    def walk(items):
        for item in items:
            children = item.get("children") or []
            if item.get("type") == "choice":
                names = [c["name"] for c in children if c.get("type") == "bool" and c.get("name")]
                for name in names:
                    members[name] = names
            walk(children)

    with contextlib.suppress(OSError, ValueError, KeyError, AttributeError, TypeError):
        walk(json.loads((build / "config" / "kconfig_menus.json").read_text()))
    return members


def config_drift(build):
    """Values the build's sdkconfig sets differently from the project's sdkconfig.defaults.

    Options the build does not have (another target, a dependency off) are skipped.
    """
    with contextlib.suppress(OSError, ValueError, KeyError):
        desc = json.loads((build / "project_description.json").read_text())
        project = Path(desc["project_path"])
        choices = kconfig_choices(build)
        want = {}
        # ESP-IDF records the ordered defaults list, including board overlays.
        # Later files override earlier values, just as they do at configure time;
        # a choice member set to y replaces whichever member an earlier file chose.
        defaults = desc.get("config_defaults", "sdkconfig.defaults")
        for path in defaults.split(";"):
            if path:
                for k, v in defaults_values((project / path).read_text()).items():
                    if v is True:
                        for other in choices.get(k, ()):
                            want.pop(other, None)
                    want[k] = v
        have = json.loads((build / "config" / "sdkconfig.json").read_text())
        return [f"{k}={have[k]} (sdkconfig.defaults: {v})" for k, v in want.items() if k in have and have[k] != v]
    return []


class Console:
    """Console lines to the screen and the --log file."""

    def __init__(self, out=None, log=None):
        self.out, self.log = out or sys.stdout, log
        self.buf, self.shown = "", 0
        self.lines = []   # every complete line

    def feed(self, text):
        self.buf += text
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self.line(line.rstrip("\r"), self.shown)
            self.shown = 0
        self.out.flush()

    def idle(self):
        """Nothing new arrived: show a partial line (a prompt)."""
        if self.buf[self.shown:]:
            self.out.write(self.buf[self.shown:])
            self.out.flush()
            self.shown = len(self.buf)

    def line(self, line, shown=0):
        self.lines.append(line)
        self.out.write(line[shown:] + "\n")
        self.write_log(line)

    def note(self, text):
        print(text, file=self.out, flush=True)
        self.write_log("# " + " ".join(text.split()))

    def write_log(self, line):
        if self.log:
            self.log.write(f"{datetime.datetime.now():%H:%M:%S.%f}"[:-3] + " " + line + "\n")
            self.log.flush()


@contextlib.contextmanager
def console_keys():
    """Yield a poll() that returns the next key pressed, without Enter, or None."""
    if os.name == "nt":
        import msvcrt

        def poll():
            if not msvcrt.kbhit():
                return None
            ch = msvcrt.getwch()
            if ch in "\x00\xe0":   # arrow and function keys: drop the second half
                msvcrt.getwch()
                return None
            return ch
        yield poll
        return
    fd = sys.stdin.fileno()
    if not os.isatty(fd):
        yield lambda: None
        return
    import select
    import termios
    import tty
    old = termios.tcgetattr(fd)
    tty.setcbreak(fd)
    try:
        yield lambda: os.read(fd, 1).decode(errors="ignore") if select.select([fd], [], [], 0)[0] else None
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def monitor(port, poll, console):
    import serial
    console.note(f"flash: console on {port} at 115200; Ctrl+] or Ctrl+C exits")
    s = None
    try:
        while True:
            if s is None:
                # The watchdog restart re-enumerates USB; keep trying until it is back.
                try:
                    s = open_quiet(port)
                except serial.SerialException:
                    time.sleep(0.2)
                    continue
            try:
                data = s.read(4096)
                if data:
                    console.feed(data.decode(errors="replace"))
                    # Monitoring can run for days, including K S scan dumps.
                    # Only check_board needs an in-memory transcript.
                    console.lines.clear()
                else:
                    console.idle()
                key = poll()
                if key in ("\x1d", "\x03"):
                    break
                if key:
                    s.write(key.encode("ascii", errors="ignore"))
                    console.write_log("# key " + key)
            except serial.SerialTimeoutException:
                console.note("\n[flash: console not reading keys]")
            except serial.SerialException:
                with contextlib.suppress(serial.SerialException):
                    s.close()
                s = None
                console.note("\n[flash: port lost, reopening]")
    except KeyboardInterrupt:
        pass
    finally:
        if s is not None:
            with contextlib.suppress(serial.SerialException):
                s.close()
    return 0


def check_board(port, console, booting, boot_timeout=60.0, status_wait=2.0):
    """Wait for SDR ready (if booting), then send T; return the console lines read."""
    import serial
    start = len(console.lines)
    s = None

    def pump(seconds, until=None):
        nonlocal s
        mark = len(console.lines)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if s is None:
                try:
                    s = open_quiet(port)
                except serial.SerialException:
                    time.sleep(0.2)
                    continue
            try:
                data = s.read(4096)
            except serial.SerialException:
                with contextlib.suppress(serial.SerialException):
                    s.close()
                s = None
                continue
            if data:
                console.feed(data.decode(errors="replace"))
            if until and any(u in line for line in console.lines[mark:] for u in until):
                return True
        return False

    def send(key):
        nonlocal s
        for _ in range(2):
            if s is None:
                pump(1.0)   # still re-enumerating: give it a moment
            if s is None:
                continue
            try:
                s.write(key)
                console.write_log("# key " + key.decode())
                return
            except serial.SerialException:
                with contextlib.suppress(serial.SerialException):
                    s.close()
                s = None

    try:
        if booting:
            console.note(f"flash: check waits up to {boot_timeout:.0f} s for the boot (SDR ready)")
            pump(boot_timeout, BOOT_DONE + BOOT_FAILED)
        send(b"T")
        if pump(status_wait, T_DONE):
            pump(0.3)
    finally:
        if s is not None:
            with contextlib.suppress(serial.SerialException):
                s.close()
    return console.lines[start:]


def evaluate(lines, booting, elf=None, heap_warn=8192, heap_min_warn=1024):
    """Judge the check from the console lines: (problems, warnings, summary fields)."""
    problems, warnings, fields = [], [], {}
    boot_at = next((i for i, line in enumerate(lines) if any(b in line for b in BOOT_DONE)), None)
    if booting and boot_at is None:
        failed = [line.strip() for line in lines if any(f in line for f in BOOT_FAILED)]
        problems.append("no SDR ready line: the detector did not finish booting" +
                        (f" ({failed[-1]})" if failed else ""))
    for i, line in enumerate(lines):
        if any(c in line for c in CRASH):
            problems.append("crash: " + line.strip())
        elif (not booting or (boot_at is not None and i > boot_at)) and any(b in line for b in BOOT_BANNER):
            problems.append("rebooted after the boot: " + line.strip())

    def last(tag):
        found = [dict(KV.findall(line[line.find(tag):])) for line in lines if tag in line]
        return found[-1] if found else None

    fw = last("FIRMWARE id=")
    if fw is None:
        problems.append("no FIRMWARE line: the board did not answer T")
    else:
        fields["firmware"] = fw.get("id", "?")
        running = fw.get("elf_sha256", "")
        if elf:
            fields["elf_match"] = int(bool(running) and elf.startswith(running))
            if not fields["elf_match"]:
                problems.append(f"running image elf_sha256={running[:16] or '?'} is not the flashed build {elf[:16]}")
    status = last("STATUS ") or {}
    for key, floor in (("heap_largest", heap_warn), ("heap_min", heap_min_warn)):
        if status.get(key, "").isdigit():
            fields[key] = int(status[key])
            if fields[key] < floor:
                warnings.append(f"{key}={fields[key]} below {floor} bytes (internal heap is nearly exhausted)")
    sdr = last("SDR plan=")
    if sdr is None:
        problems.append("no SDR plan= line: the board did not answer T")
    else:
        fields.update({k: sdr[k] for k in ("gain", "rot_hz_x10", "faults", "cal_needed") if k in sdr})
        if "faults" not in sdr:
            problems.append("incomplete SDR plan= line: engine health was not confirmed")
        elif sdr["faults"] != "0":
            problems.append(f"engine faults={sdr['faults']} (capture faults since boot)")
        if sdr.get("cal_needed") == "1":
            warnings.append("cal_needed=1: an LO's DC offset or IQ estimate is out of range "
                            "(F to recalibrate, VTX off)")
    # A boot loop repeats the same crash every boot: report each once, with a count.
    counted = {p: problems.count(p) for p in problems}
    problems = [p if n == 1 else f"{p} (x{n})" for p, n in counted.items()]
    return problems, warnings, fields


def run_check(port, console, booting, elf=None, boot_timeout=60.0, heap_warn=8192, heap_min_warn=1024,
              drift=()):
    lines = check_board(port, console, booting, boot_timeout)
    problems, warnings, fields = evaluate(lines, booting, elf, heap_warn, heap_min_warn)
    for w in ["sdkconfig differs from sdkconfig.defaults: " + d for d in drift] + warnings:
        console.note("CHECK warn: " + w)
    for p in problems:
        console.note("CHECK fail: " + p)
    console.note(" ".join(["CHECK result=" + ("fail" if problems else "pass")] +
                          [f"{k}={v}" for k, v in fields.items()]))
    return 1 if problems else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Flash the detector (ESP32-C5) without a USB line reset.")
    ap.add_argument("port", help="serial port, for example COM6 or /dev/ttyACM0")
    ap.add_argument("--build", type=Path, default=DEFAULT_BUILD,
                    help="idf.py build directory (default: %(default)s)")
    ap.add_argument("-b", "--baud", type=int, default=460800)
    ap.add_argument("--no-key", action="store_true",
                    help="skip the download key: the board is already in the ROM loader "
                         "(BOOT held while plugging in, or an interrupted flash)")
    ap.add_argument("--app-only", action="store_true",
                    help="flash only the app image (flash_app_args), not the bootloader "
                         "and partition table")
    ap.add_argument("--monitor", action="store_true",
                    help="after flashing, show the console without resetting the chip")
    ap.add_argument("--monitor-only", action="store_true",
                    help="do not flash: only show the console (or --check the running board)")
    ap.add_argument("--log", type=Path, metavar="FILE",
                    help="append the console to FILE with timestamps (scan_analyze.py reads it)")
    ap.add_argument("--check", action="store_true",
                    help="after flashing, wait for the boot, run T and exit 1 if the health check fails")
    ap.add_argument("--boot-timeout", type=float, default=60.0, metavar="SECONDS",
                    help="with --check: how long to wait for SDR ready (default: %(default)s)")
    ap.add_argument("--heap-warn", type=int, default=8192, metavar="BYTES",
                    help="with --check: warn when T's heap_largest is below this (default: %(default)s)")
    ap.add_argument("--heap-min-warn", type=int, default=1024, metavar="BYTES",
                    help="with --check: warn when T's heap_min (lowest since boot) is below this "
                         "(default: %(default)s)")
    args = ap.parse_args(argv)
    args_file = "flash_app_args" if args.app_only else "flash_args"

    if not args.monitor_only:
        # Check everything before the key: it reboots the detector into the loader.
        if not (args.build / args_file).is_file():
            print(f"flash: {args.build / args_file} not found; run idf.py build first")
            return 2
        if importlib.util.find_spec("esptool") is None:
            print("flash: esptool not found; run this in the ESP-IDF environment "
                  "(export.ps1 / . ./export.sh) or pip install esptool")
            return 2

    with contextlib.ExitStack() as stack:
        log = None
        if args.log:
            log = stack.enter_context(open(args.log, "a", encoding="utf-8"))
        console = Console(log=log)
        console.write_log(f"# flash.py log {datetime.datetime.now().isoformat(timespec='seconds')} "
                          f"port={args.port} argv={' '.join(sys.argv[1:] if argv is None else argv)}")
        rc = 0
        drift = []
        if not args.monitor_only:
            drift = config_drift(args.build)
            for d in drift:
                console.note("flash: warning: sdkconfig differs from sdkconfig.defaults: " + d)
            if drift:
                console.note("flash: ESP-IDF keeps an existing sdkconfig; delete it and rebuild to take the "
                             "defaults (flashing anyway)")
            rc = flash(args, args_file, console)
            if rc != 0:
                return rc
        if args.check:
            elf = None if args.monitor_only else elf_sha256(args.build)
            rc = run_check(args.port, console, not args.monitor_only, elf, args.boot_timeout,
                           args.heap_warn, args.heap_min_warn, drift)
        if args.monitor or (args.monitor_only and not args.check):
            with console_keys() as poll:
                monitor(args.port, poll, console)
        return rc


def flash(args, args_file, console):
    if not args.no_key:
        if not send_download_key(args.port):
            console.note("flash: the firmware did not acknowledge the download key, so nothing was "
                         "flashed (esptool's sync would reach a running detector).\n"
                         "  Board already in the ROM loader: rerun with --no-key.\n"
                         "  Otherwise remove USB and the battery, hold BOOT while plugging USB "
                         "back in, release it, and rerun with --no-key.")
            return 1
        wait_for_port(args.port)

    rc = subprocess.run(esptool_cmd(args.port, args.baud, args_file), cwd=args.build).returncode
    if rc == 0:
        console.note("flash: done; the detector restarts into the new image")
    else:
        console.note("flash: esptool failed. If it timed out, remove USB and the battery, hold BOOT "
                     "while plugging USB back in, and rerun with --no-key.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
