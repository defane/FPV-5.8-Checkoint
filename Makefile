# FPV 5.8 Checkpoint — global build/flash helper.
# All logic lives in tools/fw.sh so the recipes stay single-line and portable
# (works with stock macOS GNU make 3.81 — no .ONESHELL needed).
#
#   make firmware                            build all: detector node master master-c5
#   make build-<name>                        build one (detector|node|master|master-c5)
#   make flash  [FW=<name>] [PORT=<port>]    flash one (interactive menu if omitted)
#   make monitor [FW=<name>] [PORT=<port>]   serial monitor (Ctrl-] to quit)
#   make clean                               remove all build dirs
#
# Override the ESP-IDF location if needed:
#   make firmware IDF_EXPORT=/path/to/esp-idf/export.sh

IDF_EXPORT ?= $(HOME)/esp/esp-idf-v6.0.2/export.sh
FIRMWARES  := detector node master master-c5
FW         ?=
PORT       ?=
export IDF_EXPORT

FW_SH := tools/fw.sh

.DEFAULT_GOAL := help
.PHONY: help firmware flash monitor variants clean

help:
	@$(FW_SH) help

variants:
	@$(FW_SH) variants

firmware:
	@$(FW_SH) build $(FIRMWARES)

build-%:
	@$(FW_SH) build $*

flash:
	@$(FW_SH) flash "$(FW)" "$(PORT)"

monitor:
	@$(FW_SH) monitor "$(FW)" "$(PORT)"

clean:
	@$(FW_SH) clean
