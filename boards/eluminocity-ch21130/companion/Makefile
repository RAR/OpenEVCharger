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
TESTS := test/test_smoke test/test_shmem

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

clean:
	rm -f delta-bridge $(TESTS)
