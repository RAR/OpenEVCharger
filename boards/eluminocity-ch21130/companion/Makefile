# delta-bridge — companion MQTT bridge for the Eluminocity CH-21130 / Delta EVMU30.
#
#   make            cross-compile the static armv5te binary (delta-bridge)
#   make test       build + run all host unit tests
#   make clean
#
# CROSS must point at the musl armv5te toolchain prefix (see ../README.md).

CROSS  ?= armv5l-linux-musleabi-
CC     ?= cc
CFLAGS_WARN := -Wall -Wextra
CFLAGS_STD  := -std=c11
CFLAGS_OPT  ?= -O2
CFLAGS       = $(CFLAGS_WARN) $(CFLAGS_STD) $(CFLAGS_OPT) -Isrc

SRC := src/shmem.c src/charger_state.c src/mqtt_codec.c src/mqtt_client.c \
       src/mqtt_adapter.c src/commands.c src/config.c src/web.c src/rfid.c \
       src/meter.c src/adc.c src/led.c src/main.c

# TESTS grows as each module's test lands — every task that adds a test/ link
# rule also appends its target here, so `make test` always builds only what
# exists.
TESTS := test/test_smoke test/test_shmem test/test_charger_state test/test_mqtt_codec \
         test/test_mqtt_adapter test/test_config test/test_backoff test/test_commands \
         test/test_web test/test_rfid test/test_meter test/test_adc test/test_led

.PHONY: all test test-image clean dcofimage

all: delta-bridge

delta-bridge: $(SRC)
	$(CROSS)gcc $(CFLAGS) -static -o $@ $(SRC)

test: $(TESTS) test-image
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; exit $$fail

# wrap_dco round-trip tests (run as part of `make test`); see image/.
test-image:
	@cd image && python3 test_wrap_dco.py

# --- DcoFImage build (M12, see docs/22) ---
# Inputs:
#   STOCK_ROOTFS — path to extracted stock rootfs
#   BRIDGE_BIN   — cross-compiled delta-bridge binary
#   OUT_DIR      — where to write DcoFImage / DcoFImage-stock-restore
# Defaults match this repo's layout; override on the command line.
STOCK_ROOTFS ?= ../../../../../testcharger/delta/dump/rootfs-unpacked
BRIDGE_BIN   ?= delta-bridge.m12
OUT_DIR      ?= ../../../../../../build/m12

dcofimage:
	@if [ ! -f "$(BRIDGE_BIN)" ]; then \
		echo "error: $(BRIDGE_BIN) missing — see docs/22 for the docker-cc invocation"; \
		exit 1; \
	fi
	image/build-dcofimage.sh "$(STOCK_ROOTFS)" "$(BRIDGE_BIN)" "$(OUT_DIR)"
	SHA=$$(sha256sum "$(BRIDGE_BIN)" | cut -d' ' -f1); \
	image/verify_dcofimage.py --expected-sha256 "$$SHA" "$(OUT_DIR)/DcoFImage"

# --- host test link rules (added as modules land) ---
test/test_smoke: test/test_smoke.c test/test_harness.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_smoke.c

test/test_shmem: test/test_shmem.c src/shmem.c src/shmem.h src/shmem_offsets.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_shmem.c src/shmem.c

test/test_charger_state: test/test_charger_state.c src/charger_state.c src/shmem.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_charger_state.c src/charger_state.c src/shmem.c

test/test_mqtt_codec: test/test_mqtt_codec.c src/mqtt_codec.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_mqtt_codec.c src/mqtt_codec.c

test/test_mqtt_adapter: test/test_mqtt_adapter.c src/mqtt_adapter.c \
                        src/charger_state.c src/shmem.c src/mqtt_codec.c \
                        src/commands.c test/fake_mqtt_client.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_mqtt_adapter.c src/mqtt_adapter.c \
	      src/charger_state.c src/shmem.c src/mqtt_codec.c src/commands.c \
	      test/fake_mqtt_client.c

test/test_commands: test/test_commands.c src/commands.c src/shmem.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_commands.c src/commands.c src/shmem.c

test/test_web: test/test_web.c src/web.c src/web.h src/web_html.h \
               src/commands.c src/shmem.c src/charger_state.c src/config.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_web.c src/web.c \
	      src/commands.c src/shmem.c src/charger_state.c src/config.c

test/test_config: test/test_config.c src/config.c src/config.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_config.c src/config.c

test/test_backoff: test/test_backoff.c src/backoff.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_backoff.c

test/test_rfid: test/test_rfid.c src/rfid.c src/rfid.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_rfid.c src/rfid.c

test/test_meter: test/test_meter.c src/meter.c src/meter.h src/shmem.c src/shmem.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_meter.c src/meter.c src/shmem.c

test/test_adc: test/test_adc.c src/adc.c src/adc.h src/shmem.c src/shmem.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_adc.c src/adc.c src/shmem.c

test/test_led: test/test_led.c src/led.c src/led.h src/shmem.c src/shmem.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_led.c src/led.c src/shmem.c

clean:
	rm -f delta-bridge $(TESTS)
