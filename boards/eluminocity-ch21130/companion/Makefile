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
       src/mqtt_adapter.c src/config.c src/main.c

# TESTS grows as each module's test lands — every task that adds a test/ link
# rule also appends its target here, so `make test` always builds only what
# exists.
TESTS := test/test_smoke test/test_shmem test/test_charger_state test/test_mqtt_codec \
         test/test_mqtt_adapter test/test_config test/test_backoff

.PHONY: all test clean
all: delta-bridge

delta-bridge: $(SRC)
	$(CROSS)gcc $(CFLAGS) -static -o $@ $(SRC)

test: $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; exit $$fail

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
                        test/fake_mqtt_client.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_mqtt_adapter.c src/mqtt_adapter.c \
	      src/charger_state.c src/shmem.c src/mqtt_codec.c test/fake_mqtt_client.c

test/test_config: test/test_config.c src/config.c src/config.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_config.c src/config.c

test/test_backoff: test/test_backoff.c src/backoff.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_backoff.c

clean:
	rm -f delta-bridge $(TESTS)
