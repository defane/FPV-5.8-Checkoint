#!/usr/bin/env bash
# Build / flash / monitor the FPV 5.8 Checkpoint firmwares.
#
# Called by the root Makefile, but usable directly:
#   tools/fw.sh build [name...]        build the named firmwares (default: all)
#   tools/fw.sh flash  [FW] [PORT]     flash one (interactive menu if omitted)
#   tools/fw.sh monitor [FW] [PORT]    open the serial monitor
#   tools/fw.sh clean                  remove all build dirs
#
# ESP-IDF is sourced from $IDF_EXPORT (default ~/esp/esp-idf-v6.0.2/export.sh).
# Kept POSIX-bash-3.2 friendly so it runs on stock macOS /bin/bash.

set -eu

# Run from the repository root regardless of the caller's CWD.
cd "$(dirname "$0")/.." || exit 1

IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v6.0.2/export.sh}"
FIRMWARES="detector node master master-c5"

# resolve <name> -> sets DIR IDFARGS BUILDDIR BIN
resolve() {
  case "$1" in
    detector)  DIR=firmware/detector; IDFARGS=""; BUILDDIR=build;    BIN=detector-c5.bin ;;
    node)      DIR=firmware/node;     IDFARGS=""; BUILDDIR=build;    BIN=node-c3.bin ;;
    master)    DIR=firmware/master;   IDFARGS=""; BUILDDIR=build;    BIN=master-c6.bin ;;
    master-c5) DIR=firmware/master
               IDFARGS="-DIDF_TARGET=esp32c5 -DSDKCONFIG=sdkconfig.c5 -DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults"
               BUILDDIR=build-c5; BIN=master-c5.bin ;;
    "") echo "no firmware selected" >&2; return 2 ;;
    *)  echo "unknown firmware: '$1' (choose: $FIRMWARES)" >&2; return 2 ;;
  esac
}

idf_env() {
  if [ ! -f "$IDF_EXPORT" ]; then
    echo "ESP-IDF export.sh not found: $IDF_EXPORT" >&2
    echo "Set IDF_EXPORT=/path/to/esp-idf/export.sh" >&2
    exit 1
  fi
  # shellcheck disable=SC1090
  . "$IDF_EXPORT" >/dev/null 2>&1
}

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

cmd="${1:-help}"
[ $# -gt 0 ] && shift || true

case "$cmd" in
  build)
    [ $# -ge 1 ] || set -- $FIRMWARES
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
    idf_env
    [ -n "$port" ] || port="$(pick_port)"
    [ -n "$port" ] || { echo "no port selected" >&2; exit 2; }
    echo ">>> $cmd $fw -> $port"
    ( cd "$DIR" && idf.py $IDFARGS -B "$BUILDDIR" -p "$port" "$cmd" )
    ;;
  clean)
    rm -rf firmware/detector/build firmware/node/build \
           firmware/master/build firmware/master/build-c5
    echo ">>> build dirs removed."
    ;;
  help|-h|--help)
    cat <<EOF
FPV 5.8 Checkpoint firmware helper (firmwares: $FIRMWARES)

  tools/fw.sh build [name...]      build named firmwares (default: all)
  tools/fw.sh flash  [FW] [PORT]   flash one (menu if omitted)
  tools/fw.sh monitor [FW] [PORT]  serial monitor (Ctrl-] to quit)
  tools/fw.sh clean                remove all build dirs

Env: IDF_EXPORT=$IDF_EXPORT
EOF
    ;;
  *)
    echo "unknown command: '$cmd' (build|flash|monitor|clean|help)" >&2
    exit 2
    ;;
esac
