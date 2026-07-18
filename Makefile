# Helper targets for the common dev loops. idf.py needs the ESP-IDF
# environment; each recipe sources it so `make build` works from a fresh
# shell (Homebrew python first on PATH, per the toolchain setup).
#
#   make build                  incremental firmware build
#   make cleanbuild             wipe build/ + rebuild (fixes stale CMake cache)
#   make ota                    fresh-stamp build + OTA push + verify (auto-retry)
#   make ota VIEWPORT=<host>    same, against a specific device
#   make verify                 post-push pending-verify -> valid check only
#   make check                  type-check the Scrypted plugin
SHELL := /bin/bash

VIEWPORT ?= 10.0.13.83
ESP_ENV  ?= ../../env.sh
IDF = export PATH="/opt/homebrew/bin:$$PATH" && source $(ESP_ENV) >/dev/null 2>&1 && idf.py

.PHONY: build cleanbuild ota verify check

build:
	$(IDF) build

cleanbuild:
	rm -rf build
	$(IDF) build

# The embedded git hash stamps at CMake configure time only, so an OTA of
# fresh commits must reconfigure first or the binary reports a stale SHA.
ota:
	$(IDF) reconfigure >/dev/null
	$(IDF) build
	tools/ota.sh push $(VIEWPORT)

verify:
	tools/ota.sh verify $(VIEWPORT)

check:
	cd scrypted && npx tsc --noEmit
