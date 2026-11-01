#!/usr/bin/env bash
# Build / flash / monitor the FPV 5.8 Checkpoint firmwares.
#
# Called by the root Makefile, but usable directly:
#   tools/fw.sh build [name...]        build the named firmwares (default: all)
#   tools/fw.sh flash  [FW] [PORT]     flash one (interactive menu if omitted)
#   tools/fw.sh monitor [FW] [PORT]    open the serial monitor
#   tools/fw.sh clean                  remove all build dirs
#
# build always needs ESP-IDF (sourced from $IDF_EXPORT, default
# ~/esp/esp-idf-v6.0.2/export.sh). flash/monitor use ESP-IDF when it is
# available and otherwise fall back to esptool / a plain serial terminal on the
# already-built image -- handy when you build in one place and flash in another.
# Kept POSIX-bash-3.2 friendly so it runs on stock macOS /bin/bash.

set -eu

# Run from the repository root regardless of the caller's CWD.
cd "$(dirname "$0")/.." || exit 1

IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v6.0.2/export.sh}"

# Firmware variants, one FW= name each. A role's plain name (detector/node/
# master) is its DEFAULT board; "<role>-<tag>" is a board variant. Each variant
# builds in its own dir with its own sdkconfig, so they never clobber. To add a
# variant: drop a resolve() case below (its own BUILDDIR, SDKCONFIG, CHIP and a
# sdkconfig.<tag>.defaults in the role dir) and list it in FIRMWARES.
FIRMWARES="detector node master master-c5"

# resolve <name> -> sets DIR IDFARGS BUILDDIR BIN CHIP LABEL
resolve() {
    case "$1" in
        detector)  DIR=firmware/detector; IDFARGS=""; BUILDDIR=build; BIN=detector-c5.bin; CHIP=esp32c5
                   LABEL="RF detector  -- ESP32-C5-Zero (default)" ;;
        node)      DIR=firmware/node;     IDFARGS=""; BUILDDIR=build; BIN=node-c3.bin;     CHIP=esp32c3
                   LABEL="node bridge  -- ESP32-C3-Zero, WS2812 GPIO10 (default)" ;;
        master)    DIR=firmware/master;   IDFARGS=""; BUILDDIR=build; BIN=master-c6.bin;   CHIP=esp32c6
                   LABEL="fleet master -- ESP32-C6-Zero, WS2812 GPIO8 (default)" ;;
        master-c5) DIR=firmware/master; CHIP=esp32c5
                   IDFARGS="-DIDF_TARGET=esp32c5 -DSDKCONFIG=sdkconfig.c5 -DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults"
                   BUILDDIR=build-c5; BIN=master-c5.bin
                   LABEL="fleet master -- ESP32-C5-Zero stand-in, WS2812 GPIO27" ;;
        "") echo "no firmware selected" >&2; return 2 ;;
        *)  echo "unknown firmware: '$1' (choose: $FIRMWARES)" >&2; return 2 ;;
    esac
}

idf_available() { [ -f "$IDF_EXPORT" ]; }
idf_env() { . "$IDF_EXPORT" >/dev/null 2>&1; }

# echoes the chosen firmware on stdout; menu goes to stderr
pick_fw() {
    local f
    echo "Select the firmware:" >&2
    select f in $FIRMWARES; do
        if [ -n "${f:-}" ]; then echo "$f"; return 0; fi
    done
    return 1
}

# echoes the chosen port on stdout; menu/prompts go to stderr
pick_port() {
    local ports p
    ports="$(ls /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null || true)"
    if [ -z "$ports" ]; then
        read -r -p "No serial port auto-detected. Enter port: " p
        echo "$p"; return 0
    fi
    echo "Select the serial port:" >&2
    select p in $ports "other (type manually)"; do
        if [ "${p:-}" = "other (type manually)" ]; then
            read -r -p "Port: " p; echo "$p"; return 0
        elif [ -n "${p:-}" ]; then
            echo "$p"; return 0
        fi
    done
    return 1
}

# esptool fallback: flash the already-built image, no ESP-IDF needed.
esptool_flash() {   # <build dir> <chip> <port>
    local bd="$1" chip="$2" port="$3" tool
    if [ ! -f "$bd/flash_args" ]; then
        echo "no build at $bd/ (flash_args missing)." >&2
        echo "Build it where ESP-IDF is installed first: make build-<name>" >&2
        exit 1
    fi
    if   command -v esptool    >/dev/null 2>&1; then tool="esptool"
    elif command -v esptool.py >/dev/null 2>&1; then tool="esptool.py"
    elif python3 -c "import esptool" >/dev/null 2>&1; then tool="python3 -m esptool"
    else
        echo "esptool not found. Install it:  pip3 install esptool  (needs v5+)" >&2
        exit 1
    fi
    echo ">>> esptool flash ($chip) from $bd/ -> $port"
    ( cd "$bd" && $tool --chip "$chip" -p "$port" write-flash "@flash_args" )
}

# plain serial terminal fallback when ESP-IDF's monitor is unavailable.
serial_monitor() {  # <port>
    local port="$1"
    echo ">>> serial monitor on $port @115200 (no ESP-IDF decode)" >&2
    if python3 -c "import serial.tools.miniterm" >/dev/null 2>&1; then
        echo "    quit with Ctrl-]" >&2
        exec python3 -m serial.tools.miniterm "$port" 115200
    elif command -v screen >/dev/null 2>&1; then
        echo "    quit with Ctrl-A then K" >&2
        exec screen "$port" 115200
    else
        echo "install a terminal: pip3 install pyserial  (or use screen)" >&2
        exit 1
    fi
}

cmd="${1:-help}"
[ $# -gt 0 ] && shift || true

case "$cmd" in
    build)
        [ $# -ge 1 ] || set -- $FIRMWARES
        if ! idf_available; then
            echo "ESP-IDF not found at $IDF_EXPORT (building needs it)." >&2
            echo "Set IDF_EXPORT=/path/to/esp-idf/export.sh" >&2
            exit 1
        fi
        idf_env
        for fw in "$@"; do
            resolve "$fw"
            echo ">>> building $fw  ($DIR, idf args: ${IDFARGS:-none})"
            ( cd "$DIR" && idf.py $IDFARGS -B "$BUILDDIR" build )
            echo ">>> $fw -> $DIR/$BUILDDIR/$BIN"
        done
        echo ">>> done."
        ;;
    flash|monitor)
        fw="${1:-${FW:-}}"
        port="${2:-${PORT:-}}"
        [ -n "$fw" ] || fw="$(pick_fw)"
        resolve "$fw"
        [ -n "$port" ] || port="$(pick_port)"
        [ -n "$port" ] || { echo "no port selected" >&2; exit 2; }
        if idf_available; then
            idf_env
            ( cd "$DIR" && idf.py $IDFARGS -B "$BUILDDIR" -p "$port" "$cmd" )
        elif [ "$cmd" = flash ]; then
            esptool_flash "$DIR/$BUILDDIR" "$CHIP" "$port"
        else
            serial_monitor "$port"
        fi
        ;;
    clean)
        rm -rf firmware/detector/build firmware/node/build \
               firmware/master/build firmware/master/build-c5
        echo ">>> build dirs removed."
        ;;
    variants|list)
        echo "Firmware variants (FW=<name>):"
        for fw in $FIRMWARES; do
            resolve "$fw"
            printf "  %-11s %s\n" "$fw" "$LABEL"
        done
        ;;
    help|-h|--help)
        cat <<EOF
FPV 5.8 Checkpoint firmware helper (firmwares: $FIRMWARES)

  tools/fw.sh build [name...]      build named firmwares (default: all)
  tools/fw.sh flash  [FW] [PORT]   flash one (menu if omitted)
  tools/fw.sh monitor [FW] [PORT]  serial monitor
  tools/fw.sh variants             list the FW= names and their boards
  tools/fw.sh clean                remove all build dirs

FW= names: a role (detector/node/master) is its default board; "<role>-<tag>"
is a variant. build needs ESP-IDF; flash/monitor use it when present, else fall
back to esptool / a serial terminal on the already-built image.
Env: IDF_EXPORT=$IDF_EXPORT
EOF
        ;;
    *)
        echo "unknown command: '$cmd' (build|flash|monitor|variants|clean|help)" >&2
        exit 2
        ;;
esac
