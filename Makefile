TOOLS_DIR ?= /opt/mraf-tools
ARDUINO_CLI ?= $(TOOLS_DIR)/bin/arduino-cli
ARDUINO_CONFIG ?= $(TOOLS_DIR)/arduino-cli.yaml
ARDUINO_CONFIG_ARG = $(if $(wildcard $(ARDUINO_CONFIG)),--config-file $(ARDUINO_CONFIG),)
FQBN ?= esp32:esp32:esp32
BUILD_DIR ?= /tmp/mraf-build
WOKWI_DIR ?= hardware/wokwi
WOKWI_BUILD ?= $(WOKWI_DIR)/build
PORT ?= /dev/ttyUSB0
SPEED ?= 60
HTTP_PORT ?= 8080
SCENARIO ?= none
NODE ?= node
NPM ?= npm
MATTER_PORT ?= 5540

.PHONY: all ui compile flash monitor test test-all test-cpp test-sim server-test \
	server-build server-run matter-install matter-run matter-check matter-test \
	sim sim-scenario diagrams wokwi clean help

all: test compile

ui:
	python3 tools/gen_webui.py

compile: ui
	PATH="$(TOOLS_DIR)/bin:$$PATH" FQBN="$(FQBN)" BUILD_DIR="$(BUILD_DIR)" \
		./scripts/compile_firmware.sh

flash: compile
	PATH="$(TOOLS_DIR)/bin:$$PATH" $(ARDUINO_CLI) $(ARDUINO_CONFIG_ARG) \
		upload -p "$(PORT)" --fqbn "$(FQBN)" --input-dir "$(BUILD_DIR)"

monitor:
	PATH="$(TOOLS_DIR)/bin:$$PATH" $(ARDUINO_CLI) $(ARDUINO_CONFIG_ARG) \
		monitor -p "$(PORT)" -b "$(FQBN)" --config 115200

test: test-cpp test-sim

test-cpp:
	g++ -std=c++11 -O2 -Wall -Wextra tests/test_schedule.cpp -o /tmp/mraf-test-schedule
	/tmp/mraf-test-schedule
	g++ -std=c++11 -O2 -Wall -Wextra -Wno-unused-variable -Itests/shim \
		tests/test_controller.cpp firmware/mraf/controller.cpp -o /tmp/mraf-test-controller
	/tmp/mraf-test-controller

test-sim:
	cd simulator && python3 -m unittest -v test_sim

server-test:
	cd server && mvn -q test

test-all: test server-test

server-build:
	cd server && mvn -q -DskipTests package

server-run:
	cd server && mvn spring-boot:run

matter-install:
	cd matter && $(NPM) install

matter-run:
	cd matter && MRAF_MATTER_PORT="$(MATTER_PORT)" $(NODE) src/mraf-matter-bridge.mjs

matter-check:
	cd matter && $(NODE) --check src/mapping.mjs && $(NODE) --check src/mraf-matter-bridge.mjs

matter-test:
	cd matter && $(NODE) --test test/*.test.mjs

sim:
	python3 simulator/mraf_sim.py --speed "$(SPEED)" --port "$(HTTP_PORT)"

sim-scenario:
	@test "$(SCENARIO)" != "none" || (echo "usage: make sim-scenario SCENARIO=pump-morning" && exit 1)
	python3 simulator/mraf_sim.py --speed "$(SPEED)" --port "$(HTTP_PORT)" --scenario "$(SCENARIO)"

diagrams:
	python3 tools/gen_diagrams.py

wokwi: ui
	mkdir -p "$(WOKWI_BUILD)"
	PATH="$(TOOLS_DIR)/bin:$$PATH" $(ARDUINO_CLI) $(ARDUINO_CONFIG_ARG) \
		compile --fqbn "$(FQBN)" --output-dir "$(WOKWI_BUILD)" firmware/mraf
	@echo "Open $(WOKWI_DIR)/diagram.json in the Wokwi extension or wokwi.com"

clean:
	rm -rf "$(BUILD_DIR)" "$(WOKWI_BUILD)" server/target simulator/__pycache__ tests/__pycache__

help:
	@echo "make test                 run C++ schedule/controller tests and Python simulator tests"
	@echo "make test-all             run firmware/simulator tests and Spring Boot tests"
	@echo "make server-test          run Spring Boot tests only"
	@echo "make server-build         build server/target/mraf-server-1.0.0.jar"
	@echo "make server-run           run the Spring Boot server on :8080"
	@echo "make matter-install       install the Node.js Matter bridge dependencies"
	@echo "make matter-run           run the Matter bridge (Node 20+ required)"
	@echo "make matter-check         syntax-check the Matter bridge sources"
	@echo "make matter-test          run the Matter bridge mapping tests"
	@echo "make compile              generate webui.h and compile the ESP32 firmware"
	@echo "make flash PORT=/dev/ttyUSB0  compile and upload"
	@echo "make monitor PORT=/dev/ttyUSB0"
	@echo "make sim                  native simulator on http://localhost:8080"
	@echo "make sim-scenario SCENARIO=pump-morning"
	@echo "make diagrams             regenerate SVG wiring diagrams"
	@echo "make wokwi                build firmware for the Wokwi browser simulator"
	@echo "make clean"
