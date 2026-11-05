# FPV 5.8 Checkpoint — global build/flash helper.
# All logic lives in tools/fw.sh so the recipes stay single-line and portable
# (works with stock macOS GNU make 3.81 — no .ONESHELL needed).
#
#   make firmware                            build every variant
#   make variants                            list the FW= names and their boards
#   make build-<name>                        build one (e.g. build-detector-xiao-c5)
#   make flash  [FW=<name>] [PORT=<port>]    flash one (interactive menu if omitted)
#   make monitor [FW=<name>] [PORT=<port>]   serial monitor (Ctrl-] to quit)
#   make clean                               remove all build dirs
#   make web-master / web-node               the web pages on a mock server, no board
#                                            (tools/web_mock.py; WEB_ARGS="--vtx --latency 300")
#
# FW= names: a role (detector/node/master) is its default board; "<role>-<tag>"
# is a board variant (see `make variants`).
#
# Override the ESP-IDF location if needed:
#   make firmware IDF_EXPORT=/path/to/esp-idf/export.sh

IDF_EXPORT ?= $(HOME)/esp/esp-idf-v6.0.2/export.sh
FW         ?=
PORT       ?=
WEB_ARGS   ?=
export IDF_EXPORT

FW_SH := tools/fw.sh

.DEFAULT_GOAL := help
.PHONY: help firmware flash monitor variants clean

help:
	@$(FW_SH) help
	@echo "Web pages without a board: make web-master | web-node [WEB_ARGS=...] (tools/web_mock.py --help)"

variants:
	@$(FW_SH) variants

firmware:
	@$(FW_SH) build

build-%:
	@$(FW_SH) build $*

flash:
	@$(FW_SH) flash "$(FW)" "$(PORT)"

monitor:
	@$(FW_SH) monitor "$(FW)" "$(PORT)"

clean:
	@$(FW_SH) clean

web-%:
	@python3 tools/web_mock.py $* $(WEB_ARGS)
