# Eluminocity CH-21130 Companion MQTT Bridge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `delta-bridge` — a read-only Linux-userland daemon that attaches to the Delta EVMU30 stock firmware's SysV shared-memory segment and publishes charger state to Home Assistant over MQTT.

**Architecture:** Strictly layered C modules — `shmem` (read-only segment accessors) ← `charger_state` (normalize + scale + change-detect) ← `northbound` (adapter interface) ← `mqtt_adapter` (HA discovery + publish) → `mqtt_codec`/`mqtt_client` (MQTT 3.1.1). Cross-compiled static with the musl armv5te toolchain; every layer host-tested in isolation. Packaged for distribution via a `DcoFImage` builder.

**Tech Stack:** C11, static musl (armv5l-linux-musleabi), MQTT 3.1.1 over plain TCP, SysV IPC shared memory, JFFS2 image packing.

---

## Conventions for every task

- **Build/test commands run from** `boards/eluminocity-ch21130/companion/` unless stated otherwise.
- **Host test build** uses the system `cc`. **Cross build** uses `$(CROSS)gcc` where `CROSS ?= armv5l-linux-musleabi-` (toolchain must be on `PATH`; provisioning is documented in the board README, Task 1).
- **TDD:** write the failing test, run it to confirm it fails, implement minimally, run to confirm it passes, commit.
- **Commits** are made from the OpenEVCharger repo root (`esphome/rippleon/OpenEVCharger/`). Paths in `git add` are relative to that root.
- All offsets/encodings in `src/shmem_offsets.h` are **RE-derived and bench-verify-pending** (milestones M0/M1). They are isolated in that one header on purpose.

---

## File structure

```
OpenEVCharger/boards/eluminocity-ch21130/
├── README.md                       # Task 1  — board overview, toolchain provisioning
├── docs/                           # Task 2  — migrated RE docs
│   ├── 01-Pri_Comm-protocol.md
│   ├── 02-IPC-and-main-architecture.md
│   ├── 03-OCPP-and-firmware-bundle.md
│   └── 04-sharemem-decoded.md
└── companion/
    ├── Makefile                    # Task 1  — host-test + cross build
    ├── delta-bridge.conf.example   # Task 10 — sample config
    ├── src/
    │   ├── shmem_offsets.h          # Task 3  — single source of truth for offsets
    │   ├── shmem.h / shmem.c        # Task 3  — read-only segment accessors + attach
    │   ├── charger_state.h / .c     # Task 4,5 — normalized state, scaling, diff, faults
    │   ├── northbound.h             # Task 6  — adapter interface
    │   ├── mqtt_codec.h / .c        # Task 7  — MQTT 3.1.1 packet encode/parse
    │   ├── mqtt_client.h / .c       # Task 8  — socket transport + keepalive
    │   ├── mqtt_adapter.h / .c      # Task 9  — northbound impl: HA discovery + publish
    │   ├── config.h / .c            # Task 10 — config-file parser
    │   └── main.c                   # Task 11 — wiring, poll loop, signals
    ├── test/
    │   ├── test_harness.h           # Task 1
    │   ├── test_smoke.c             # Task 1
    │   ├── fixtures/
    │   │   ├── make_shmem_fixture.py # Task 3
    │   │   └── shmem_snapshot.bin    # Task 3 (generated, committed)
    │   ├── fake_mqtt_client.c       # Task 9  — recording stub for adapter tests
    │   ├── test_shmem.c             # Task 3
    │   ├── test_charger_state.c     # Task 4,5
    │   ├── test_mqtt_codec.c        # Task 7
    │   ├── test_mqtt_adapter.c      # Task 9
    │   └── test_config.c            # Task 10
    └── image/
        ├── wrap_dco.py              # Task 12 — DELTADCOF wrapper
        ├── build-dcofimage.sh       # Task 12 — image orchestration
        └── dropbear/README.md       # Task 13 — dropbear provisioning notes
.github/workflows/eluminocity-companion.yml   # Task 14
```

---

### Task 1: Board scaffold, build system, test harness

**Files:**
- Create: `boards/eluminocity-ch21130/README.md`
- Create: `boards/eluminocity-ch21130/companion/Makefile`
- Create: `boards/eluminocity-ch21130/companion/test/test_harness.h`
- Create: `boards/eluminocity-ch21130/companion/test/test_smoke.c`

- [ ] **Step 1: Write the smoke test**

`boards/eluminocity-ch21130/companion/test/test_smoke.c`:
```c
/* Smoke test: proves the test harness compiles and runs. */
#include "test_harness.h"

int main(void)
{
    CHECK(1 + 1 == 2);
    CHECK_EQ(0xa10, 2576);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write the test harness**

`boards/eluminocity-ch21130/companion/test/test_harness.h`:
```c
/* Minimal dependency-free C test harness. Each test_*.c file is a standalone
 * program: it CHECK()s, then calls TEST_MAIN_END() which returns non-zero on
 * any failure. `make test` compiles and runs each one. */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H
#include <stdio.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                                   \
    tests_run++;                                                           \
    if (!(cond)) {                                                         \
        tests_failed++;                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                      \
} while (0)

#define CHECK_EQ(a, b) do {                                                \
    tests_run++;                                                           \
    long _a = (long)(a), _b = (long)(b);                                   \
    if (_a != _b) {                                                        \
        tests_failed++;                                                    \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%ld != %ld)\n",             \
                __FILE__, __LINE__, #a, #b, _a, _b);                       \
    }                                                                      \
} while (0)

#define CHECK_STR(a, b) do {                                               \
    tests_run++;                                                           \
    if (strcmp((a), (b)) != 0) {                                           \
        tests_failed++;                                                    \
        fprintf(stderr, "FAIL %s:%d: \"%s\" == \"%s\"\n",                  \
                __FILE__, __LINE__, (a), (b));                             \
    }                                                                      \
} while (0)

#define TEST_MAIN_END() do {                                               \
    fprintf(stderr, "%s: %d/%d passed\n",                                  \
            __FILE__, tests_run - tests_failed, tests_run);                \
    return tests_failed ? 1 : 0;                                           \
} while (0)

#endif /* TEST_HARNESS_H */
```

- [ ] **Step 3: Write the Makefile**

`boards/eluminocity-ch21130/companion/Makefile`:
```makefile
# delta-bridge — companion MQTT bridge for the Eluminocity CH-21130 / Delta EVMU30.
#
#   make            cross-compile the static armv5te binary (delta-bridge)
#   make test       build + run all host unit tests
#   make clean
#
# CROSS must point at the musl armv5te toolchain prefix (see ../README.md).

CROSS  ?= armv5l-linux-musleabi-
CC     ?= cc
CFLAGS ?= -Wall -Wextra -std=c11 -O2 -Isrc

SRC := src/shmem.c src/charger_state.c src/mqtt_codec.c src/mqtt_client.c \
       src/mqtt_adapter.c src/config.c src/main.c

# TESTS grows as each module's test lands — every task that adds a test/ link
# rule also appends its target here, so `make test` always builds only what
# exists.
TESTS := test/test_smoke

.PHONY: all test clean
all: delta-bridge

delta-bridge: $(SRC)
	$(CROSS)gcc $(CFLAGS) -static -o $@ $(SRC)

test: $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; exit $$fail

# --- host test link rules (added as modules land) ---
test/test_smoke: test/test_smoke.c test/test_harness.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_smoke.c

clean:
	rm -f delta-bridge $(TESTS)
```

- [ ] **Step 4: Write the board README**

`boards/eluminocity-ch21130/README.md`:
```markdown
# Eluminocity CH-21130 (Delta EVMU30) — companion-only board

This board is **companion-only**. There is no MCU clean-room firmware port —
the unit's STM32F334 safety MCU is left running its stock Delta firmware and
treated as a trusted black box. Accordingly this directory has **no**
`board.cmake` / `pin_map.h` / linker script, and is not part of the top-level
CMake board matrix.

## Layout

- `companion/` — `delta-bridge`, a read-only Linux-userland daemon that runs on
  the unit's SPEAr320 application processor. It attaches to the stock firmware's
  SysV shared-memory segment and publishes charger state to Home Assistant over
  MQTT. Built with its own `Makefile` (musl armv5te cross-compile), not CMake.
- `docs/` — the reverse-engineering docs the bridge codes against.

## Toolchain

`companion/Makefile` cross-compiles with a musl armv5te toolchain. Provision it
and put its `bin/` on `PATH` (the prefix `armv5l-linux-musleabi-` is the
Makefile default; override with `make CROSS=...`):

    # musl.cc prebuilt toolchain
    curl -LO https://musl.cc/armv5l-linux-musleabi-cross.tgz
    tar xzf armv5l-linux-musleabi-cross.tgz
    export PATH="$PWD/armv5l-linux-musleabi-cross/bin:$PATH"

Host unit tests use the system `cc` and need no cross toolchain.

## Design

See `../../docs/superpowers/specs/2026-05-14-eluminocity-ch21130-mqtt-bridge-design.md`.
```

- [ ] **Step 5: Run the smoke test to verify it passes**

Run (from `boards/eluminocity-ch21130/companion/`):
```bash
make test
```
Expected: `test/test_smoke.c: 2/2 passed`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add boards/eluminocity-ch21130/README.md \
        boards/eluminocity-ch21130/companion/Makefile \
        boards/eluminocity-ch21130/companion/test/test_harness.h \
        boards/eluminocity-ch21130/companion/test/test_smoke.c
git commit -m "eluminocity-companion: board scaffold + build system + test harness"
```

---

### Task 2: Migrate RE docs into the board

**Files:**
- Create: `boards/eluminocity-ch21130/docs/01-Pri_Comm-protocol.md` (copied)
- Create: `boards/eluminocity-ch21130/docs/02-IPC-and-main-architecture.md` (copied)
- Create: `boards/eluminocity-ch21130/docs/03-OCPP-and-firmware-bundle.md` (copied)
- Create: `boards/eluminocity-ch21130/docs/04-sharemem-decoded.md` (copied)

- [ ] **Step 1: Copy the four canonical RE docs**

The source docs live in the *outer* repo (different git repo), so this is a
copy, not a `git mv`:
```bash
SRC=../../../../testcharger/delta/docs
DST=boards/eluminocity-ch21130/docs
mkdir -p "$DST"
cp "$SRC/01-Pri_Comm-protocol.md"          "$DST/"
cp "$SRC/02-IPC-and-main-architecture.md"  "$DST/"
cp "$SRC/03-OCPP-and-firmware-bundle.md"   "$DST/"
cp "$SRC/04-sharemem-decoded.md"           "$DST/"
```
(Run from the OpenEVCharger repo root. The raw 32 MB firmware dumps and bench RE
scripts intentionally stay in the outer repo as RE evidence — only the docs
migrate.)

- [ ] **Step 2: Verify the four files are present and non-empty**

Run:
```bash
wc -l boards/eluminocity-ch21130/docs/*.md
```
Expected: four files, each well over 100 lines.

- [ ] **Step 3: Commit**

```bash
git add boards/eluminocity-ch21130/docs/
git commit -m "eluminocity-companion: migrate Delta RE docs into the board"
```

---

### Task 3: shmem module — offsets, accessors, attach, fixture

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/shmem_offsets.h`
- Create: `boards/eluminocity-ch21130/companion/src/shmem.h`
- Create: `boards/eluminocity-ch21130/companion/src/shmem.c`
- Create: `boards/eluminocity-ch21130/companion/test/fixtures/make_shmem_fixture.py`
- Create: `boards/eluminocity-ch21130/companion/test/fixtures/shmem_snapshot.bin` (generated)
- Create: `boards/eluminocity-ch21130/companion/test/test_shmem.c`
- Modify: `boards/eluminocity-ch21130/companion/Makefile` (add `test/test_shmem` rule)

**Background:** Offsets/encodings below come from `decode_sharemem.py` (the
authoritative shmem decoder) and `docs/02`/`docs/04`. They are single-byte
values per the decoder. All are bench-verify-pending (M0/M1). The segment is
`MeterSMKey = 0x153E`, `MeterSMSize = 0x40000` (256 KiB).

- [ ] **Step 1: Write the fixture generator**

`boards/eluminocity-ch21130/companion/test/fixtures/make_shmem_fixture.py`:
```python
#!/usr/bin/env python3
"""Generate a deterministic 256 KiB shmem fixture with known sentinel values at
the offsets shmem.c reads. Tests assert against these sentinels — this verifies
accessor *logic*, independent of real-hardware RE uncertainty (that is covered
by bench milestones M0/M1). Run once; the output .bin is committed.

    python3 make_shmem_fixture.py
"""
SIZE = 0x40000
buf = bytearray(SIZE)

# (offset, value) — must match src/shmem_offsets.h
buf[0xa00] = 0x02          # connector state
buf[0xa07] = 0x00          # fault flags
buf[0xa08] = 0x55          # heartbeat
buf[0xa0b] = 0x01          # STM32 link OK
buf[0xa10] = 0x78          # VRMS raw (120)
buf[0xa24] = 0x10          # IRMS raw (16)
buf[0xa63] = 0x00          # FW-upgrade gate

# trap-alarm bitmap 0x138..0x158 (32 bytes): set byte +3 nonzero
buf[0x138 + 3] = 0x01

with open("shmem_snapshot.bin", "wb") as f:
    f.write(buf)
print(f"wrote shmem_snapshot.bin ({SIZE} bytes)")
```

- [ ] **Step 2: Generate and inspect the fixture**

Run (from `test/fixtures/`):
```bash
python3 make_shmem_fixture.py
```
Expected: `wrote shmem_snapshot.bin (262144 bytes)`.

- [ ] **Step 3: Write the offsets header**

`boards/eluminocity-ch21130/companion/src/shmem_offsets.h`:
```c
/* Delta EVMU30 SysV shared-memory layout — SINGLE SOURCE OF TRUTH.
 *
 * RE-derived from decode_sharemem.py + docs/02 + docs/04. Every value here is
 * BENCH-VERIFY-PENDING (milestones M0/M1). All reads are single bytes per the
 * decoder. If a value is wrong, this is the only file to change.
 */
#ifndef SHMEM_OFFSETS_H
#define SHMEM_OFFSETS_H

#define SHMEM_KEY          0x153E      /* MeterSMKey  */
#define SHMEM_SIZE         0x40000     /* MeterSMSize, 256 KiB */

#define OFF_CONNECTOR_STATE 0x0a00     /* u8  — see CONNECTOR_STATE_* */
#define OFF_FAULT_FLAGS     0x0a07     /* u8  — coarse fault flags */
#define OFF_HEARTBEAT       0x0a08     /* u8  — Pri_Comm heartbeat counter */
#define OFF_STM32_LINK      0x0a0b     /* u8  — 1 = inter-MCU link OK */
#define OFF_VRMS            0x0a10     /* u8  — line voltage, raw */
#define OFF_IRMS            0x0a24     /* u8  — line current, raw */
#define OFF_FW_UPGRADE_GATE 0x0a63     /* u8  — firmware-upgrade gate flag */

#define OFF_ALARM_BITMAP    0x0138     /* 32 bytes — SHRMEM_TRAP_ALARM */
#define ALARM_BITMAP_LEN    32

#endif /* SHMEM_OFFSETS_H */
```

- [ ] **Step 4: Write the failing test**

`boards/eluminocity-ch21130/companion/test/test_shmem.c`:
```c
#include <string.h>
#include "test_harness.h"
#include "shmem.h"
#include "shmem_offsets.h"

int main(void)
{
    struct shmem sm;
    CHECK_EQ(shmem_load_file(&sm, "test/fixtures/shmem_snapshot.bin"), 0);

    CHECK_EQ(shmem_u8(&sm, OFF_CONNECTOR_STATE), 0x02);
    CHECK_EQ(shmem_u8(&sm, OFF_HEARTBEAT),       0x55);
    CHECK_EQ(shmem_u8(&sm, OFF_STM32_LINK),      0x01);
    CHECK_EQ(shmem_u8(&sm, OFF_VRMS),            0x78);
    CHECK_EQ(shmem_u8(&sm, OFF_IRMS),            0x10);

    /* out-of-range offset returns 0 defensively, never crashes */
    CHECK_EQ(shmem_u8(&sm, SHMEM_SIZE + 100), 0);

    /* alarm bitmap copy */
    unsigned char alarm[ALARM_BITMAP_LEN];
    shmem_copy(&sm, OFF_ALARM_BITMAP, alarm, ALARM_BITMAP_LEN);
    CHECK_EQ(alarm[3], 0x01);
    CHECK_EQ(alarm[0], 0x00);

    shmem_release(&sm);
    TEST_MAIN_END();
}
```

- [ ] **Step 5: Write the shmem header**

`boards/eluminocity-ch21130/companion/src/shmem.h`:
```c
/* Read-only accessor layer over the Delta SysV shared-memory segment.
 *
 * Two ways to obtain a `struct shmem`:
 *   - shmem_attach()    — on-device: shmget()+shmat() of the live segment.
 *   - shmem_load_file() — host tests: read a fixture file into a buffer.
 * Either way, the accessors below are identical and never write the segment.
 */
#ifndef SHMEM_H
#define SHMEM_H

#include <stddef.h>

struct shmem {
    const unsigned char *base;   /* segment / buffer base */
    size_t               size;   /* bytes available at base */
    int                  shmid;  /* >=0 if attached via shmem_attach, else -1 */
    unsigned char       *owned;  /* non-NULL if shmem_load_file malloc'd it */
};

/* On-device: attach the live segment read-only. Returns 0 on success,
 * -1 if the segment does not exist yet (caller should retry/backoff). */
int  shmem_attach(struct shmem *sm);

/* Host tests: load a fixture file. Returns 0 on success, -1 on error. */
int  shmem_load_file(struct shmem *sm, const char *path);

/* Detach / free. Safe to call on a zeroed struct. */
void shmem_release(struct shmem *sm);

/* Read a single byte. Out-of-range offsets return 0 (defensive, never crash). */
unsigned char shmem_u8(const struct shmem *sm, size_t off);

/* Copy `len` bytes from `off` into `dst`. Out-of-range bytes are zero-filled. */
void shmem_copy(const struct shmem *sm, size_t off, void *dst, size_t len);

#endif /* SHMEM_H */
```

- [ ] **Step 6: Run the test to verify it fails**

Add the link rule to `Makefile` (after the `test/test_smoke` rule):
```makefile
test/test_shmem: test/test_shmem.c src/shmem.c src/shmem.h src/shmem_offsets.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_shmem.c src/shmem.c
```
Run: `make test/test_shmem`
Expected: FAIL — `shmem.c` does not exist / undefined references.

- [ ] **Step 7: Write the shmem implementation**

`boards/eluminocity-ch21130/companion/src/shmem.c`:
```c
#include "shmem.h"
#include "shmem_offsets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

int shmem_attach(struct shmem *sm)
{
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
    /* No IPC_CREAT: if the segment is absent we must NOT create it. */
    int id = shmget(SHMEM_KEY, SHMEM_SIZE, 0);
    if (id < 0)
        return -1;
    void *p = shmat(id, NULL, SHM_RDONLY);
    if (p == (void *)-1)
        return -1;
    sm->base  = (const unsigned char *)p;
    sm->size  = SHMEM_SIZE;
    sm->shmid = id;
    return 0;
}

int shmem_load_file(struct shmem *sm, const char *path)
{
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    unsigned char *buf = malloc(SHMEM_SIZE);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, SHMEM_SIZE, f);
    fclose(f);
    if (n != SHMEM_SIZE) {
        free(buf);
        return -1;
    }
    sm->base  = buf;
    sm->owned = buf;
    sm->size  = SHMEM_SIZE;
    return 0;
}

void shmem_release(struct shmem *sm)
{
    if (sm->shmid >= 0 && sm->base)
        shmdt(sm->base);
    if (sm->owned)
        free(sm->owned);
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
}

unsigned char shmem_u8(const struct shmem *sm, size_t off)
{
    if (!sm->base || off >= sm->size)
        return 0;
    return sm->base[off];
}

void shmem_copy(const struct shmem *sm, size_t off, void *dst, size_t len)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < len; i++)
        d[i] = shmem_u8(sm, off + i);
}
```

- [ ] **Step 8: Run the test to verify it passes**

Run: `make test/test_shmem && ./test/test_shmem`
Expected: `test/test_shmem.c: 8/8 passed`, exit 0.

- [ ] **Step 9: Add test_shmem to the `test` target and commit**

In `Makefile`, append `test/test_shmem` to the `TESTS` variable. Run `make test`
to confirm smoke + shmem both pass.

```bash
git add boards/eluminocity-ch21130/companion/src/shmem_offsets.h \
        boards/eluminocity-ch21130/companion/src/shmem.h \
        boards/eluminocity-ch21130/companion/src/shmem.c \
        boards/eluminocity-ch21130/companion/test/test_shmem.c \
        boards/eluminocity-ch21130/companion/test/fixtures/make_shmem_fixture.py \
        boards/eluminocity-ch21130/companion/test/fixtures/shmem_snapshot.bin \
        boards/eluminocity-ch21130/companion/Makefile
git commit -m "eluminocity-companion: read-only shmem accessor layer + fixture"
```

---

### Task 4: charger_state — struct, read, scaling

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/charger_state.h`
- Create: `boards/eluminocity-ch21130/companion/src/charger_state.c`
- Create: `boards/eluminocity-ch21130/companion/test/test_charger_state.c`
- Modify: `boards/eluminocity-ch21130/companion/Makefile` (add `test/test_charger_state` rule)

- [ ] **Step 1: Write the failing test**

`boards/eluminocity-ch21130/companion/test/test_charger_state.c`:
```c
#include <string.h>
#include "test_harness.h"
#include "shmem.h"
#include "charger_state.h"

int main(void)
{
    struct shmem sm;
    CHECK_EQ(shmem_load_file(&sm, "test/fixtures/shmem_snapshot.bin"), 0);

    struct charger_state cs;
    charger_state_init(&cs);
    charger_state_read(&cs, &sm);

    /* Fixture: VRMS raw 0x78 (120), IRMS raw 0x10 (16). v1 scaling is
     * identity-with-units until bench-tuned (M1) — see charger_state.c. */
    CHECK_EQ(cs.voltage_v,   120);
    CHECK_EQ(cs.current_a,   16);
    CHECK_EQ(cs.stm32_link,  1);
    CHECK_EQ(cs.evse_state,  EVSE_STATE_CONNECTED);   /* connector byte 0x02 */
    CHECK_EQ(cs.heartbeat,   0x55);

    /* unknown connector byte -> EVSE_STATE_UNKNOWN, never crashes */
    struct charger_state cs2;
    charger_state_init(&cs2);
    /* directly exercise the decoder via a hand-built shmem buffer */
    unsigned char raw[0x40000];
    memset(raw, 0, sizeof(raw));
    raw[0x0a00] = 0xEE;
    struct shmem sm2 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_read(&cs2, &sm2);
    CHECK_EQ(cs2.evse_state, EVSE_STATE_UNKNOWN);

    shmem_release(&sm);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write the charger_state header**

`boards/eluminocity-ch21130/companion/src/charger_state.h`:
```c
/* Normalized charger state: the hardware-independent struct the northbound
 * adapters publish. charger_state_read() pulls raw bytes from `shmem` and
 * applies scaling — ALL scaling lives in charger_state.c so bench-tuning (M1)
 * is a one-file change. */
#ifndef CHARGER_STATE_H
#define CHARGER_STATE_H

#include "shmem.h"

enum evse_state {
    EVSE_STATE_UNKNOWN = 0,
    EVSE_STATE_IDLE,
    EVSE_STATE_CONNECTED,
    EVSE_STATE_CHARGING,
    EVSE_STATE_FAULT,
};

#define CHARGER_MAX_FAULTS 32   /* one name slot per alarm-bitmap byte */

struct charger_state {
    int             voltage_v;     /* line voltage, volts */
    int             current_a;     /* line current, amps */
    int             stm32_link;    /* 1 = inter-MCU link OK */
    int             heartbeat;     /* Pri_Comm heartbeat counter */
    enum evse_state evse_state;
    /* faults: bitmap of which alarm slots are active (bit i = byte i nonzero) */
    unsigned int    fault_bits;
};

void charger_state_init(struct charger_state *cs);
void charger_state_read(struct charger_state *cs, const struct shmem *sm);

/* Human-readable EVSE-state string for publishing. */
const char *evse_state_str(enum evse_state s);

#endif /* CHARGER_STATE_H */
```

- [ ] **Step 3: Run the test to verify it fails**

Add to `Makefile`:
```makefile
test/test_charger_state: test/test_charger_state.c src/charger_state.c src/shmem.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_charger_state.c src/charger_state.c src/shmem.c
```
Run: `make test/test_charger_state`
Expected: FAIL — `charger_state.c` does not exist.

- [ ] **Step 4: Write the charger_state implementation**

`boards/eluminocity-ch21130/companion/src/charger_state.c`:
```c
#include "charger_state.h"
#include "shmem_offsets.h"
#include <string.h>

void charger_state_init(struct charger_state *cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->evse_state = EVSE_STATE_UNKNOWN;
}

/* Connector-state byte -> evse_state. Values from decode_sharemem.py /docs/02;
 * BENCH-VERIFY-PENDING (M0). Unknown bytes map to EVSE_STATE_UNKNOWN. */
static enum evse_state decode_connector(unsigned char b)
{
    switch (b) {
    case 0x00: return EVSE_STATE_IDLE;
    case 0x01: return EVSE_STATE_IDLE;        /* available, no cable */
    case 0x02: return EVSE_STATE_CONNECTED;   /* cable, not charging */
    case 0x03: return EVSE_STATE_CHARGING;
    case 0x04: return EVSE_STATE_FAULT;
    default:   return EVSE_STATE_UNKNOWN;
    }
}

void charger_state_read(struct charger_state *cs, const struct shmem *sm)
{
    /* v1 scaling is identity-with-units: the raw bytes are published as-is in
     * V / A. Real scale factors (and whether the per-unit Gain calibration
     * applies) are tuned against a multimeter at milestone M1 — when that
     * happens, only the two lines below change. */
    cs->voltage_v  = shmem_u8(sm, OFF_VRMS);
    cs->current_a  = shmem_u8(sm, OFF_IRMS);

    cs->stm32_link = shmem_u8(sm, OFF_STM32_LINK) ? 1 : 0;
    cs->heartbeat  = shmem_u8(sm, OFF_HEARTBEAT);
    cs->evse_state = decode_connector(shmem_u8(sm, OFF_CONNECTOR_STATE));

    unsigned char alarm[ALARM_BITMAP_LEN];
    shmem_copy(sm, OFF_ALARM_BITMAP, alarm, ALARM_BITMAP_LEN);
    cs->fault_bits = 0;
    for (int i = 0; i < ALARM_BITMAP_LEN && i < 32; i++)
        if (alarm[i])
            cs->fault_bits |= (1u << i);
}

const char *evse_state_str(enum evse_state s)
{
    switch (s) {
    case EVSE_STATE_IDLE:      return "idle";
    case EVSE_STATE_CONNECTED: return "connected";
    case EVSE_STATE_CHARGING:  return "charging";
    case EVSE_STATE_FAULT:     return "fault";
    default:                   return "unknown";
    }
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test/test_charger_state && ./test/test_charger_state`
Expected: `test/test_charger_state.c: 6/6 passed`, exit 0.

Append `test/test_charger_state` to the `TESTS` variable in the `Makefile`, then
run `make test` to confirm the whole suite still passes.

- [ ] **Step 6: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/charger_state.h \
        boards/eluminocity-ch21130/companion/src/charger_state.c \
        boards/eluminocity-ch21130/companion/test/test_charger_state.c \
        boards/eluminocity-ch21130/companion/Makefile
git commit -m "eluminocity-companion: charger_state — normalize + scale shmem"
```

---

### Task 5: charger_state — change detection + fault names

**Files:**
- Modify: `boards/eluminocity-ch21130/companion/src/charger_state.h`
- Modify: `boards/eluminocity-ch21130/companion/src/charger_state.c`
- Modify: `boards/eluminocity-ch21130/companion/test/test_charger_state.c`

- [ ] **Step 1: Add the failing test cases**

Append inside `main()` of `test/test_charger_state.c`, just before `shmem_release(&sm);`:
```c
    /* --- change detection --- */
    struct charger_state a, b;
    charger_state_init(&a);
    charger_state_init(&b);
    a.voltage_v = 120; a.current_a = 16;
    b.voltage_v = 120; b.current_a = 16;
    CHECK_EQ(charger_state_diff(&a, &b), 0);          /* identical -> no dirty bits */
    b.current_a = 17;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_CURRENT, CS_DIRTY_CURRENT);
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_VOLTAGE, 0);

    /* --- fault names --- */
    CHECK_STR(charger_fault_name(0), "RCD");
    CHECK(charger_fault_name(99) != NULL);            /* out-of-range is safe */
```

- [ ] **Step 2: Extend the charger_state header**

In `boards/eluminocity-ch21130/companion/src/charger_state.h`, add before `#endif`:
```c
/* Dirty-flag bits returned by charger_state_diff(). */
#define CS_DIRTY_VOLTAGE    (1u << 0)
#define CS_DIRTY_CURRENT    (1u << 1)
#define CS_DIRTY_LINK       (1u << 2)
#define CS_DIRTY_HEARTBEAT  (1u << 3)
#define CS_DIRTY_EVSE_STATE (1u << 4)
#define CS_DIRTY_FAULTS     (1u << 5)

/* Returns the OR of CS_DIRTY_* bits for every field that differs between
 * `prev` and `cur`. 0 means identical. */
unsigned int charger_state_diff(const struct charger_state *prev,
                                const struct charger_state *cur);

/* Name of alarm slot `i` (0..CHARGER_MAX_FAULTS-1). Out-of-range returns
 * "UNKNOWN". Names are RE-derived; refine from docs/01 as bench work confirms. */
const char *charger_fault_name(int i);
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `make test/test_charger_state`
Expected: FAIL — `charger_state_diff` / `charger_fault_name` undefined.

- [ ] **Step 4: Implement diff + fault names**

In `boards/eluminocity-ch21130/companion/src/charger_state.c`, append:
```c
unsigned int charger_state_diff(const struct charger_state *prev,
                                const struct charger_state *cur)
{
    unsigned int d = 0;
    if (prev->voltage_v  != cur->voltage_v)  d |= CS_DIRTY_VOLTAGE;
    if (prev->current_a  != cur->current_a)  d |= CS_DIRTY_CURRENT;
    if (prev->stm32_link != cur->stm32_link) d |= CS_DIRTY_LINK;
    if (prev->heartbeat  != cur->heartbeat)  d |= CS_DIRTY_HEARTBEAT;
    if (prev->evse_state != cur->evse_state) d |= CS_DIRTY_EVSE_STATE;
    if (prev->fault_bits != cur->fault_bits) d |= CS_DIRTY_FAULTS;
    return d;
}

/* Alarm-slot names. RE-derived from docs/01 / docs/02 ("31-alarm fault catalog
 * confirmed from Pri_Comm .data"). Slots whose name is not yet confirmed ship
 * as "RESERVED_nn" — a real, shippable value, refined as bench work confirms.
 * BENCH-VERIFY-PENDING: also confirm byte-vs-bit semantics of the bitmap (M0). */
static const char *const FAULT_NAMES[CHARGER_MAX_FAULTS] = {
    "RCD",          "RCDTRIP",      "GMI",          "OVP",
    "UVP",          "OCP",          "WELDING",      "PILOTERROR",
    "AMBIENT_OTP",  "RA_WATCHDOG",  "RA_CPU",       "RA_RAM",
    "RESERVED_12",  "RESERVED_13",  "RESERVED_14",  "RESERVED_15",
    "RESERVED_16",  "RESERVED_17",  "RESERVED_18",  "RESERVED_19",
    "RESERVED_20",  "RESERVED_21",  "RESERVED_22",  "RESERVED_23",
    "RESERVED_24",  "RESERVED_25",  "RESERVED_26",  "RESERVED_27",
    "RESERVED_28",  "RESERVED_29",  "RESERVED_30",  "RESERVED_31",
};

const char *charger_fault_name(int i)
{
    if (i < 0 || i >= CHARGER_MAX_FAULTS)
        return "UNKNOWN";
    return FAULT_NAMES[i];
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test/test_charger_state && ./test/test_charger_state`
Expected: `test/test_charger_state.c: 10/10 passed`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/charger_state.h \
        boards/eluminocity-ch21130/companion/src/charger_state.c \
        boards/eluminocity-ch21130/companion/test/test_charger_state.c
git commit -m "eluminocity-companion: charger_state — change detection + fault names"
```

---

### Task 6: northbound adapter interface

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/northbound.h`

This task defines a pure interface (a struct of function pointers). It has no
`.c` and no standalone test — it is exercised by `mqtt_adapter` (Task 9) and the
`fake_mqtt_client`-backed adapter test.

- [ ] **Step 1: Write the northbound interface header**

`boards/eluminocity-ch21130/companion/src/northbound.h`:
```c
/* Northbound adapter interface — the seam between the bridge core and whatever
 * it reports to. `mqtt_adapter` is the v1 implementation; an OCPP 1.6-J adapter
 * is a planned second implementation that conforms to this same vtable.
 *
 * Lifecycle:  init() once -> publish_state()/tick() on the poll loop -> shutdown().
 */
#ifndef NORTHBOUND_H
#define NORTHBOUND_H

#include "charger_state.h"

struct northbound {
    /* Opaque per-adapter context. */
    void *ctx;

    /* Bring the adapter up (open sockets, etc.). Returns 0 on success. */
    int  (*init)(struct northbound *nb);

    /* Publish state. `dirty` is the OR of CS_DIRTY_* bits; if `full` is
     * non-zero the adapter must publish every field regardless of `dirty`
     * (used on first connect and after a reconnect). Returns 0 on success. */
    int  (*publish_state)(struct northbound *nb,
                          const struct charger_state *cs,
                          unsigned int dirty, int full);

    /* Called every poll iteration for housekeeping (keepalive, reconnect).
     * Returns 0 normally, non-zero if the adapter wants the loop to treat the
     * link as down (so the next publish_state is called with full=1). */
    int  (*tick)(struct northbound *nb);

    /* Tear down (publish offline, close sockets, free ctx). */
    void (*shutdown)(struct northbound *nb);
};

#endif /* NORTHBOUND_H */
```

- [ ] **Step 2: Verify it compiles**

Run (from `companion/`):
```bash
cc -std=c11 -Wall -Wextra -Isrc -fsyntax-only -x c src/northbound.h
```
Expected: no output, exit 0.

- [ ] **Step 3: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/northbound.h
git commit -m "eluminocity-companion: northbound adapter interface"
```

---

### Task 7: mqtt_codec — MQTT 3.1.1 packet encode/parse

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/mqtt_codec.h`
- Create: `boards/eluminocity-ch21130/companion/src/mqtt_codec.c`
- Create: `boards/eluminocity-ch21130/companion/test/test_mqtt_codec.c`
- Modify: `boards/eluminocity-ch21130/companion/Makefile`

Pure functions: build packet byte buffers, parse fixed headers. No sockets.

- [ ] **Step 1: Write the failing test**

`boards/eluminocity-ch21130/companion/test/test_mqtt_codec.c`:
```c
#include <string.h>
#include "test_harness.h"
#include "mqtt_codec.h"

int main(void)
{
    unsigned char buf[256];
    int n;

    /* --- CONNECT, no auth, keepalive 60, client id "x" --- */
    n = mqtt_encode_connect(buf, sizeof(buf), "x", NULL, NULL, 60,
                            "delta-bridge/x/availability", "offline");
    CHECK(n > 0);
    CHECK_EQ(buf[0], 0x10);                 /* CONNECT packet type */
    /* variable header protocol name "MQTT" */
    CHECK_EQ(buf[2], 0x00); CHECK_EQ(buf[3], 0x04);
    CHECK_EQ(buf[4], 'M'); CHECK_EQ(buf[5], 'Q');
    CHECK_EQ(buf[6], 'T'); CHECK_EQ(buf[7], 'T');
    CHECK_EQ(buf[8], 0x04);                 /* protocol level 4 (3.1.1) */

    /* --- remaining-length codec round-trips multi-byte values --- */
    unsigned char rl[4];
    int rln = mqtt_encode_remlen(rl, 321);
    CHECK_EQ(rln, 2);
    size_t consumed = 0;
    CHECK_EQ(mqtt_decode_remlen(rl, rln, &consumed), 321);
    CHECK_EQ(consumed, 2);

    /* --- PUBLISH, QoS 0, retained --- */
    n = mqtt_encode_publish(buf, sizeof(buf), "delta-bridge/x/voltage",
                            "120", 1 /*retain*/);
    CHECK(n > 0);
    CHECK_EQ(buf[0], 0x31);                 /* PUBLISH | retain */

    /* --- PINGREQ --- */
    n = mqtt_encode_pingreq(buf, sizeof(buf));
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0], 0xC0);
    CHECK_EQ(buf[1], 0x00);

    /* --- DISCONNECT --- */
    n = mqtt_encode_disconnect(buf, sizeof(buf));
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0], 0xE0);

    /* --- CONNACK parse: 20 02 00 00 = accepted --- */
    unsigned char connack_ok[]  = { 0x20, 0x02, 0x00, 0x00 };
    unsigned char connack_bad[] = { 0x20, 0x02, 0x00, 0x05 };
    CHECK_EQ(mqtt_parse_connack(connack_ok, sizeof(connack_ok)), 0);
    CHECK(mqtt_parse_connack(connack_bad, sizeof(connack_bad)) != 0);

    /* truncated buffer is rejected, not over-read */
    CHECK(mqtt_encode_connect(buf, 4, "x", NULL, NULL, 60, NULL, NULL) < 0);

    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write the mqtt_codec header**

`boards/eluminocity-ch21130/companion/src/mqtt_codec.h`:
```c
/* MQTT 3.1.1 packet encode/parse — pure functions, no I/O. Every encoder
 * writes into a caller buffer and returns the byte count, or -1 if the buffer
 * is too small. */
#ifndef MQTT_CODEC_H
#define MQTT_CODEC_H

#include <stddef.h>

/* Encode the "remaining length" varint. Returns 1..4 bytes written. */
int    mqtt_encode_remlen(unsigned char *out, size_t value);

/* Decode a remaining-length varint from `in` (max `len` bytes available).
 * Sets *consumed to the byte count. Returns the value, or (size_t)-1 on error. */
size_t mqtt_decode_remlen(const unsigned char *in, size_t len, size_t *consumed);

/* CONNECT. `user`/`pass` may be NULL. `will_topic`/`will_msg` may be NULL
 * (no will). keepalive is seconds. Returns packet length or -1. */
int mqtt_encode_connect(unsigned char *buf, size_t cap,
                        const char *client_id,
                        const char *user, const char *pass,
                        int keepalive_s,
                        const char *will_topic, const char *will_msg);

/* PUBLISH, QoS 0. `retain` non-zero sets the retain flag. payload is a
 * NUL-terminated string. Returns packet length or -1. */
int mqtt_encode_publish(unsigned char *buf, size_t cap,
                        const char *topic, const char *payload, int retain);

int mqtt_encode_pingreq(unsigned char *buf, size_t cap);
int mqtt_encode_disconnect(unsigned char *buf, size_t cap);

/* SUBSCRIBE — stub for v1 (read-only). Defined so the v1.1 control path has a
 * seam; encodes a single-topic QoS-0 subscribe. Returns length or -1. */
int mqtt_encode_subscribe(unsigned char *buf, size_t cap,
                          unsigned short packet_id, const char *topic);

/* Parse a CONNACK. Returns 0 if the connection was accepted (return code 0),
 * non-zero otherwise (bad packet or non-zero return code). */
int mqtt_parse_connack(const unsigned char *buf, size_t len);

#endif /* MQTT_CODEC_H */
```

- [ ] **Step 3: Run the test to verify it fails**

Add to `Makefile`:
```makefile
test/test_mqtt_codec: test/test_mqtt_codec.c src/mqtt_codec.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_mqtt_codec.c src/mqtt_codec.c
```
Run: `make test/test_mqtt_codec`
Expected: FAIL — `mqtt_codec.c` does not exist.

- [ ] **Step 4: Write the mqtt_codec implementation**

`boards/eluminocity-ch21130/companion/src/mqtt_codec.c`:
```c
#include "mqtt_codec.h"
#include <string.h>

int mqtt_encode_remlen(unsigned char *out, size_t value)
{
    int n = 0;
    do {
        unsigned char b = value & 0x7F;
        value >>= 7;
        if (value)
            b |= 0x80;
        out[n++] = b;
    } while (value && n < 4);
    return n;
}

size_t mqtt_decode_remlen(const unsigned char *in, size_t len, size_t *consumed)
{
    size_t value = 0;
    int    shift = 0;
    size_t i = 0;
    for (; i < len && i < 4; i++) {
        value |= (size_t)(in[i] & 0x7F) << shift;
        shift += 7;
        if (!(in[i] & 0x80)) {
            *consumed = i + 1;
            return value;
        }
    }
    *consumed = 0;
    return (size_t)-1;
}

/* Append a 2-byte-length-prefixed string. Returns bytes written or -1. */
static int put_str(unsigned char *p, size_t room, const char *s)
{
    size_t l = strlen(s);
    if (l > 0xFFFF || room < l + 2)
        return -1;
    p[0] = (unsigned char)(l >> 8);
    p[1] = (unsigned char)(l & 0xFF);
    memcpy(p + 2, s, l);
    return (int)(l + 2);
}

/* Finalize: write fixed header byte + remaining-length in front of a payload
 * already laid out at buf+5. Returns total packet length or -1. */
static int finalize(unsigned char *buf, size_t cap,
                    unsigned char type_flags, size_t payload_len)
{
    unsigned char rl[4];
    int rln = mqtt_encode_remlen(rl, payload_len);
    size_t total = 1 + rln + payload_len;
    if (total > cap)
        return -1;
    /* shift payload to sit right after the header we are about to write */
    memmove(buf + 1 + rln, buf + 5, payload_len);
    buf[0] = type_flags;
    memcpy(buf + 1, rl, rln);
    return (int)total;
}

int mqtt_encode_connect(unsigned char *buf, size_t cap,
                        const char *client_id,
                        const char *user, const char *pass,
                        int keepalive_s,
                        const char *will_topic, const char *will_msg)
{
    if (cap < 16)
        return -1;
    unsigned char *p = buf + 5;            /* leave room for header */
    size_t room = cap - 5;
    size_t n = 0;
    int r;

    /* variable header: protocol name + level + flags + keepalive */
    static const unsigned char vh_name[] = { 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04 };
    if (room < sizeof(vh_name) + 3)
        return -1;
    memcpy(p, vh_name, sizeof(vh_name));
    n += sizeof(vh_name);

    unsigned char flags = 0x02;            /* clean session */
    int have_will = will_topic && will_msg;
    if (have_will) flags |= 0x04;          /* will flag, QoS 0, not retained */
    if (user)      flags |= 0x80;
    if (pass)      flags |= 0x40;
    p[n++] = flags;
    p[n++] = (unsigned char)(keepalive_s >> 8);
    p[n++] = (unsigned char)(keepalive_s & 0xFF);

    /* payload: client id, [will topic, will msg], [user], [pass] */
    r = put_str(p + n, room - n, client_id); if (r < 0) return -1; n += r;
    if (have_will) {
        r = put_str(p + n, room - n, will_topic); if (r < 0) return -1; n += r;
        r = put_str(p + n, room - n, will_msg);   if (r < 0) return -1; n += r;
    }
    if (user) { r = put_str(p + n, room - n, user); if (r < 0) return -1; n += r; }
    if (pass) { r = put_str(p + n, room - n, pass); if (r < 0) return -1; n += r; }

    return finalize(buf, cap, 0x10, n);
}

int mqtt_encode_publish(unsigned char *buf, size_t cap,
                        const char *topic, const char *payload, int retain)
{
    if (cap < 8)
        return -1;
    unsigned char *p = buf + 5;
    size_t room = cap - 5;
    size_t n = 0;
    int r = put_str(p, room, topic);       /* QoS 0: no packet id */
    if (r < 0) return -1;
    n += r;
    size_t pl = strlen(payload);
    if (room - n < pl)
        return -1;
    memcpy(p + n, payload, pl);
    n += pl;
    return finalize(buf, cap, retain ? 0x31 : 0x30, n);
}

int mqtt_encode_pingreq(unsigned char *buf, size_t cap)
{
    if (cap < 2) return -1;
    buf[0] = 0xC0; buf[1] = 0x00;
    return 2;
}

int mqtt_encode_disconnect(unsigned char *buf, size_t cap)
{
    if (cap < 2) return -1;
    buf[0] = 0xE0; buf[1] = 0x00;
    return 2;
}

int mqtt_encode_subscribe(unsigned char *buf, size_t cap,
                          unsigned short packet_id, const char *topic)
{
    if (cap < 8)
        return -1;
    unsigned char *p = buf + 5;
    size_t room = cap - 5;
    size_t n = 0;
    p[n++] = (unsigned char)(packet_id >> 8);
    p[n++] = (unsigned char)(packet_id & 0xFF);
    int r = put_str(p + n, room - n, topic);
    if (r < 0) return -1;
    n += r;
    if (room - n < 1) return -1;
    p[n++] = 0x00;                         /* requested QoS 0 */
    return finalize(buf, cap, 0x82, n);
}

int mqtt_parse_connack(const unsigned char *buf, size_t len)
{
    if (len < 4 || buf[0] != 0x20 || buf[1] != 0x02)
        return -1;
    return buf[3] == 0x00 ? 0 : (int)buf[3];
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test/test_mqtt_codec && ./test/test_mqtt_codec`
Expected: `test/test_mqtt_codec.c: 21/21 passed`, exit 0.

Append `test/test_mqtt_codec` to the `TESTS` variable in the `Makefile`, then
run `make test` to confirm the whole suite still passes.

- [ ] **Step 6: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/mqtt_codec.h \
        boards/eluminocity-ch21130/companion/src/mqtt_codec.c \
        boards/eluminocity-ch21130/companion/test/test_mqtt_codec.c \
        boards/eluminocity-ch21130/companion/Makefile
git commit -m "eluminocity-companion: MQTT 3.1.1 packet codec"
```

---

### Task 8: mqtt_client — socket transport + keepalive

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/mqtt_client.h`
- Create: `boards/eluminocity-ch21130/companion/src/mqtt_client.c`

This module wraps `mqtt_codec` with a TCP socket and a keepalive timer. The
socket path is not host-unit-tested (it needs a broker); correctness of the
*bytes* is already covered by Task 7. This task is verified by a compile gate
plus the optional integration check below. The same interface is re-implemented
as a recording fake in Task 9.

- [ ] **Step 1: Write the mqtt_client header**

`boards/eluminocity-ch21130/companion/src/mqtt_client.h`:
```c
/* MQTT client transport: TCP socket + keepalive over mqtt_codec. Single
 * connection, QoS 0, blocking sends with bounded timeouts. */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

struct mqtt_client {
    int   fd;                 /* socket, -1 when disconnected */
    int   keepalive_s;
    long  last_send_ms;       /* for keepalive scheduling */
};

/* Config passed to mqtt_client_connect(). */
struct mqtt_config {
    const char *host;
    int         port;
    const char *client_id;
    const char *user;         /* may be NULL */
    const char *pass;         /* may be NULL */
    int         keepalive_s;
    const char *will_topic;   /* may be NULL */
    const char *will_msg;     /* may be NULL */
};

void mqtt_client_init(struct mqtt_client *c);

/* Connect TCP, send CONNECT, wait for CONNACK. Returns 0 on success, -1 on any
 * failure (caller retries with backoff). */
int  mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg);

/* Publish QoS 0. Returns 0 on success, -1 on socket error (caller should treat
 * the link as down). */
int  mqtt_client_publish(struct mqtt_client *c, const char *topic,
                         const char *payload, int retain);

/* Housekeeping: send PINGREQ if keepalive is due, drain/ignore inbound packets.
 * `now_ms` is a monotonic millisecond clock. Returns 0 normally, -1 if the link
 * is down. */
int  mqtt_client_tick(struct mqtt_client *c, long now_ms);

/* Send DISCONNECT (best effort) and close the socket. */
void mqtt_client_disconnect(struct mqtt_client *c);

#endif /* MQTT_CLIENT_H */
```

- [ ] **Step 2: Write the mqtt_client implementation**

`boards/eluminocity-ch21130/companion/src/mqtt_client.c`:
```c
#include "mqtt_client.h"
#include "mqtt_codec.h"

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MQTT_BUF 512

void mqtt_client_init(struct mqtt_client *c)
{
    c->fd = -1;
    c->keepalive_s = 0;
    c->last_send_ms = 0;
}

/* Send all `len` bytes; returns 0 on success, -1 on error. */
static int send_all(int fd, const unsigned char *p, size_t len)
{
    while (len) {
        ssize_t w = send(fd, p, len, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg)
{
    mqtt_client_init(c);

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", cfg->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg->host, portstr, &hints, &res) != 0 || !res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* bounded I/O so a dead broker cannot stall the poll loop */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        close(fd);
        return -1;
    }

    unsigned char buf[MQTT_BUF];
    int n = mqtt_encode_connect(buf, sizeof(buf), cfg->client_id,
                                cfg->user, cfg->pass, cfg->keepalive_s,
                                cfg->will_topic, cfg->will_msg);
    if (n < 0 || send_all(fd, buf, (size_t)n) != 0) {
        close(fd);
        return -1;
    }

    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    if (r < 4 || mqtt_parse_connack(buf, (size_t)r) != 0) {
        close(fd);
        return -1;
    }

    c->fd = fd;
    c->keepalive_s = cfg->keepalive_s;
    return 0;
}

int mqtt_client_publish(struct mqtt_client *c, const char *topic,
                        const char *payload, int retain)
{
    if (c->fd < 0)
        return -1;
    unsigned char buf[MQTT_BUF];
    int n = mqtt_encode_publish(buf, sizeof(buf), topic, payload, retain);
    if (n < 0 || send_all(c->fd, buf, (size_t)n) != 0) {
        mqtt_client_disconnect(c);
        return -1;
    }
    return 0;
}

int mqtt_client_tick(struct mqtt_client *c, long now_ms)
{
    if (c->fd < 0)
        return -1;

    /* drain anything inbound (PINGRESP, etc.) without blocking — the RCVTIMEO
     * bounds it; we do not parse it for v1 (QoS 0, read-only). */
    unsigned char scratch[MQTT_BUF];
    /* non-blocking peek: a real recv would block up to RCVTIMEO, so only read
     * when keepalive is due and we just sent a PINGREQ below. */

    if (c->keepalive_s > 0 &&
        now_ms - c->last_send_ms >= (long)c->keepalive_s * 1000 / 2) {
        unsigned char buf[4];
        int n = mqtt_encode_pingreq(buf, sizeof(buf));
        if (send_all(c->fd, buf, (size_t)n) != 0) {
            mqtt_client_disconnect(c);
            return -1;
        }
        c->last_send_ms = now_ms;
        ssize_t r = recv(c->fd, scratch, sizeof(scratch), 0);
        if (r <= 0) {
            mqtt_client_disconnect(c);
            return -1;
        }
    }
    return 0;
}

void mqtt_client_disconnect(struct mqtt_client *c)
{
    if (c->fd >= 0) {
        unsigned char buf[4];
        int n = mqtt_encode_disconnect(buf, sizeof(buf));
        if (n > 0)
            (void)send_all(c->fd, buf, (size_t)n);
        close(c->fd);
    }
    c->fd = -1;
}
```

- [ ] **Step 3: Compile gate (host)**

Run (from `companion/`):
```bash
cc -std=c11 -Wall -Wextra -Isrc -c src/mqtt_client.c -o /tmp/mqtt_client.o
```
Expected: no warnings, no errors, exit 0.

- [ ] **Step 4: Optional integration check (only if a broker is reachable)**

If a local mosquitto is available, this is a quick manual smoke test — not part
of `make test`. Skip if no broker:
```bash
# terminal A: mosquitto_sub -h localhost -t 'delta-bridge/#' -v
# then build + run a 3-line throwaway main that connects + publishes once.
```
Document the result; do not commit a throwaway main.

- [ ] **Step 5: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/mqtt_client.h \
        boards/eluminocity-ch21130/companion/src/mqtt_client.c
git commit -m "eluminocity-companion: MQTT client TCP transport + keepalive"
```

---

### Task 9: mqtt_adapter — northbound impl with HA discovery

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/mqtt_adapter.h`
- Create: `boards/eluminocity-ch21130/companion/src/mqtt_adapter.c`
- Create: `boards/eluminocity-ch21130/companion/test/fake_mqtt_client.c`
- Create: `boards/eluminocity-ch21130/companion/test/test_mqtt_adapter.c`
- Modify: `boards/eluminocity-ch21130/companion/Makefile`

`mqtt_adapter` is tested by linking against `fake_mqtt_client.c` (records every
publish into a global ring) instead of the real `mqtt_client.c`. Both provide
the identical `mqtt_client.h` symbols.

- [ ] **Step 1: Write the recording fake**

`boards/eluminocity-ch21130/companion/test/fake_mqtt_client.c`:
```c
/* Test double for mqtt_client.h: records publishes so tests can assert topic
 * and payload. Same symbols as src/mqtt_client.c — link one OR the other. */
#include "mqtt_client.h"
#include <string.h>

#define FAKE_MAX 64
struct fake_pub {
    char topic[160];
    char payload[256];
    int  retain;
};
struct fake_pub fake_pubs[FAKE_MAX];
int             fake_pub_count;
int             fake_connected;

void mqtt_client_init(struct mqtt_client *c) { c->fd = -1; }

int mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg)
{
    (void)cfg;
    c->fd = 1;
    fake_connected = 1;
    fake_pub_count = 0;
    return 0;
}

int mqtt_client_publish(struct mqtt_client *c, const char *topic,
                        const char *payload, int retain)
{
    (void)c;
    if (fake_pub_count >= FAKE_MAX)
        return -1;
    struct fake_pub *p = &fake_pubs[fake_pub_count++];
    strncpy(p->topic, topic, sizeof(p->topic) - 1);
    p->topic[sizeof(p->topic) - 1] = '\0';
    strncpy(p->payload, payload, sizeof(p->payload) - 1);
    p->payload[sizeof(p->payload) - 1] = '\0';
    p->retain = retain;
    return 0;
}

int mqtt_client_tick(struct mqtt_client *c, long now_ms)
{
    (void)c; (void)now_ms;
    return 0;
}

void mqtt_client_disconnect(struct mqtt_client *c)
{
    c->fd = -1;
    fake_connected = 0;
}
```

- [ ] **Step 2: Write the failing test**

`boards/eluminocity-ch21130/companion/test/test_mqtt_adapter.c`:
```c
#include <string.h>
#include "test_harness.h"
#include "mqtt_adapter.h"
#include "northbound.h"

/* exposed by fake_mqtt_client.c */
struct fake_pub { char topic[160]; char payload[256]; int retain; };
extern struct fake_pub fake_pubs[];
extern int fake_pub_count;

static int find_pub(const char *topic, const char *payload)
{
    for (int i = 0; i < fake_pub_count; i++)
        if (strcmp(fake_pubs[i].topic, topic) == 0 &&
            strcmp(fake_pubs[i].payload, payload) == 0)
            return 1;
    return 0;
}

int main(void)
{
    struct mqtt_adapter_config cfg = {
        .broker_host = "localhost", .broker_port = 1883,
        .broker_user = NULL, .broker_pass = NULL,
        .topic_prefix = "delta-bridge", .device_id = "abc",
        .keepalive_s = 60,
    };
    struct northbound nb;
    CHECK_EQ(mqtt_adapter_create(&nb, &cfg), 0);
    CHECK_EQ(nb.init(&nb), 0);

    struct charger_state cs;
    charger_state_init(&cs);
    cs.voltage_v = 121; cs.current_a = 15; cs.stm32_link = 1;
    cs.evse_state = EVSE_STATE_CHARGING;

    /* full publish emits discovery + every state field */
    CHECK_EQ(nb.publish_state(&nb, &cs, 0, 1 /*full*/), 0);
    CHECK(find_pub("homeassistant/sensor/delta_abc_voltage/config",
                   "") == 0);              /* discovery payload is non-empty */
    /* discovery topic exists with *some* payload */
    int saw_discovery = 0, saw_voltage = 0, saw_state = 0;
    for (int i = 0; i < fake_pub_count; i++) {
        if (strcmp(fake_pubs[i].topic,
                   "homeassistant/sensor/delta_abc_voltage/config") == 0)
            saw_discovery = 1;
        if (strcmp(fake_pubs[i].topic, "delta-bridge/abc/voltage") == 0 &&
            strcmp(fake_pubs[i].payload, "121") == 0)
            saw_voltage = 1;
        if (strcmp(fake_pubs[i].topic, "delta-bridge/abc/evse_state") == 0 &&
            strcmp(fake_pubs[i].payload, "charging") == 0)
            saw_state = 1;
    }
    CHECK(saw_discovery);
    CHECK(saw_voltage);
    CHECK(saw_state);

    /* delta publish: only the dirty field is re-sent */
    fake_pub_count = 0;
    cs.current_a = 16;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_CURRENT, 0), 0);
    CHECK(find_pub("delta-bridge/abc/current", "16"));
    CHECK_EQ(fake_pub_count, 1);            /* nothing else re-published */

    /* availability published online on init */
    nb.shutdown(&nb);
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Write the mqtt_adapter header**

`boards/eluminocity-ch21130/companion/src/mqtt_adapter.h`:
```c
/* MQTT northbound adapter: HA discovery + per-field state publish + LWT. */
#ifndef MQTT_ADAPTER_H
#define MQTT_ADAPTER_H

#include "northbound.h"

struct mqtt_adapter_config {
    const char *broker_host;
    int         broker_port;
    const char *broker_user;   /* may be NULL */
    const char *broker_pass;   /* may be NULL */
    const char *topic_prefix;  /* e.g. "delta-bridge" */
    const char *device_id;     /* e.g. unit serial */
    int         keepalive_s;
};

/* Populate `nb` with the MQTT adapter vtable + a static context built from
 * `cfg`. `cfg` strings must outlive `nb`. Returns 0 on success. */
int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg);

#endif /* MQTT_ADAPTER_H */
```

- [ ] **Step 4: Run the test to verify it fails**

Add to `Makefile`:
```makefile
test/test_mqtt_adapter: test/test_mqtt_adapter.c src/mqtt_adapter.c \
                        src/charger_state.c src/shmem.c src/mqtt_codec.c \
                        test/fake_mqtt_client.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_mqtt_adapter.c src/mqtt_adapter.c \
	      src/charger_state.c src/shmem.c src/mqtt_codec.c test/fake_mqtt_client.c
```
Run: `make test/test_mqtt_adapter`
Expected: FAIL — `mqtt_adapter.c` does not exist.

- [ ] **Step 5: Write the mqtt_adapter implementation**

`boards/eluminocity-ch21130/companion/src/mqtt_adapter.c`:
```c
#include "mqtt_adapter.h"
#include "mqtt_client.h"
#include "charger_state.h"

#include <stdio.h>
#include <string.h>

/* Static single-instance context (the bridge has exactly one adapter). */
struct adapter_ctx {
    struct mqtt_adapter_config cfg;
    struct mqtt_client         client;
    char availability_topic[160];
    int  connected;
};
static struct adapter_ctx g_ctx;

/* --- topic helpers --- */
static void state_topic(const struct adapter_ctx *a, char *out, size_t cap,
                        const char *field)
{
    snprintf(out, cap, "%s/%s/%s", a->cfg.topic_prefix, a->cfg.device_id, field);
}
static void discovery_topic(const struct adapter_ctx *a, char *out, size_t cap,
                            const char *component, const char *field)
{
    snprintf(out, cap, "homeassistant/%s/delta_%s_%s/config",
             component, a->cfg.device_id, field);
}

/* Publish one HA discovery config for a sensor field. */
static void publish_discovery(struct adapter_ctx *a, const char *component,
                              const char *field, const char *name,
                              const char *unit, const char *device_class)
{
    char topic[160], st[160], payload[512];
    discovery_topic(a, topic, sizeof(topic), component, field);
    state_topic(a, st, sizeof(st), field);

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_%s\","
        "\"availability_topic\":\"%s\","
        "\"device\":{\"identifiers\":[\"delta_%s\"],"
        "\"name\":\"Delta EVMU30 (%s)\",\"manufacturer\":\"Delta\","
        "\"model\":\"EVMU30\"}",
        name, st, a->cfg.device_id, field, a->availability_topic,
        a->cfg.device_id, a->cfg.device_id);
    if (unit && n > 0 && n < (int)sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class && n > 0 && n < (int)sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"device_class\":\"%s\"", device_class);
    if (n > 0 && n < (int)sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

static void publish_all_discovery(struct adapter_ctx *a)
{
    publish_discovery(a, "sensor", "voltage", "Voltage", "V", "voltage");
    publish_discovery(a, "sensor", "current", "Current", "A", "current");
    publish_discovery(a, "sensor", "evse_state", "EVSE State", NULL, NULL);
    publish_discovery(a, "sensor", "heartbeat", "Heartbeat", NULL, NULL);
    publish_discovery(a, "binary_sensor", "stm32_link", "STM32 Link", NULL,
                      "connectivity");
    publish_discovery(a, "sensor", "faults", "Active Faults", NULL, NULL);
}

/* Build the comma-joined list of active fault names (or "none"). */
static void format_faults(unsigned int bits, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < CHARGER_MAX_FAULTS; i++) {
        if (!(bits & (1u << i)))
            continue;
        const char *nm = charger_fault_name(i);
        int w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", nm);
        if (w < 0 || (size_t)w >= cap - n)
            break;
        n += w;
    }
    if (n == 0)
        snprintf(out, cap, "none");
}

/* --- northbound vtable --- */
static int nb_init(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    struct mqtt_config mc = {
        .host = a->cfg.broker_host, .port = a->cfg.broker_port,
        .client_id = a->cfg.device_id,
        .user = a->cfg.broker_user, .pass = a->cfg.broker_pass,
        .keepalive_s = a->cfg.keepalive_s,
        .will_topic = a->availability_topic, .will_msg = "offline",
    };
    if (mqtt_client_connect(&a->client, &mc) != 0)
        return -1;
    mqtt_client_publish(&a->client, a->availability_topic, "online", 1);
    a->connected = 1;
    return 0;
}

static int nb_publish_state(struct northbound *nb,
                            const struct charger_state *cs,
                            unsigned int dirty, int full)
{
    struct adapter_ctx *a = nb->ctx;
    if (!a->connected)
        return -1;

    if (full)
        publish_all_discovery(a);

    char topic[160], val[256];

    if (full || (dirty & CS_DIRTY_VOLTAGE)) {
        state_topic(a, topic, sizeof(topic), "voltage");
        snprintf(val, sizeof(val), "%d", cs->voltage_v);
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    if (full || (dirty & CS_DIRTY_CURRENT)) {
        state_topic(a, topic, sizeof(topic), "current");
        snprintf(val, sizeof(val), "%d", cs->current_a);
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    if (full || (dirty & CS_DIRTY_EVSE_STATE)) {
        state_topic(a, topic, sizeof(topic), "evse_state");
        mqtt_client_publish(&a->client, topic, evse_state_str(cs->evse_state), 1);
    }
    if (full || (dirty & CS_DIRTY_HEARTBEAT)) {
        state_topic(a, topic, sizeof(topic), "heartbeat");
        snprintf(val, sizeof(val), "%d", cs->heartbeat);
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    if (full || (dirty & CS_DIRTY_LINK)) {
        state_topic(a, topic, sizeof(topic), "stm32_link");
        mqtt_client_publish(&a->client, topic, cs->stm32_link ? "ON" : "OFF", 1);
    }
    if (full || (dirty & CS_DIRTY_FAULTS)) {
        state_topic(a, topic, sizeof(topic), "faults");
        format_faults(cs->fault_bits, val, sizeof(val));
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    return 0;
}

static int nb_tick(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    /* now_ms is supplied by main.c in the real loop; the fake ignores it. */
    return mqtt_client_tick(&a->client, 0);
}

static void nb_shutdown(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    if (a->connected) {
        mqtt_client_publish(&a->client, a->availability_topic, "offline", 1);
        mqtt_client_disconnect(&a->client);
        a->connected = 0;
    }
}

int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.cfg = *cfg;
    mqtt_client_init(&g_ctx.client);
    snprintf(g_ctx.availability_topic, sizeof(g_ctx.availability_topic),
             "%s/%s/availability", cfg->topic_prefix, cfg->device_id);

    nb->ctx           = &g_ctx;
    nb->init          = nb_init;
    nb->publish_state = nb_publish_state;
    nb->tick          = nb_tick;
    nb->shutdown      = nb_shutdown;
    return 0;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `make test/test_mqtt_adapter && ./test/test_mqtt_adapter`
Expected: `test/test_mqtt_adapter.c: 8/8 passed`, exit 0.

Append `test/test_mqtt_adapter` to the `TESTS` variable in the `Makefile`, then
run `make test` to confirm the whole suite still passes.

- [ ] **Step 7: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/mqtt_adapter.h \
        boards/eluminocity-ch21130/companion/src/mqtt_adapter.c \
        boards/eluminocity-ch21130/companion/test/fake_mqtt_client.c \
        boards/eluminocity-ch21130/companion/test/test_mqtt_adapter.c \
        boards/eluminocity-ch21130/companion/Makefile
git commit -m "eluminocity-companion: MQTT adapter — HA discovery + state publish"
```

---

### Task 10: config — config-file parser

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/config.h`
- Create: `boards/eluminocity-ch21130/companion/src/config.c`
- Create: `boards/eluminocity-ch21130/companion/test/test_config.c`
- Create: `boards/eluminocity-ch21130/companion/delta-bridge.conf.example`
- Modify: `boards/eluminocity-ch21130/companion/Makefile`

The spec said config parsing could live in `main.c`; it is split into `config.c`
purely so the parse function is unit-testable. `main.c` (Task 11) just reads the
file and calls `config_parse()`.

- [ ] **Step 1: Write the failing test**

`boards/eluminocity-ch21130/companion/test/test_config.c`:
```c
#include <string.h>
#include "test_harness.h"
#include "config.h"

int main(void)
{
    struct config c;

    /* defaults applied when keys are absent */
    config_defaults(&c);
    CHECK_EQ(c.broker_port, 1883);
    CHECK_EQ(c.poll_hz, 1);
    CHECK_STR(c.topic_prefix, "delta-bridge");

    /* parse overrides; comments and blank lines ignored; whitespace trimmed */
    const char *text =
        "# sample config\n"
        "broker_host = 10.0.0.5\n"
        "broker_port=8883\n"
        "  topic_prefix =  evse  \n"
        "\n"
        "device_id = unitA\n"
        "poll_hz = 2\n";
    CHECK_EQ(config_parse(&c, text), 0);
    CHECK_STR(c.broker_host, "10.0.0.5");
    CHECK_EQ(c.broker_port, 8883);
    CHECK_STR(c.topic_prefix, "evse");
    CHECK_STR(c.device_id, "unitA");
    CHECK_EQ(c.poll_hz, 2);

    /* unknown keys are ignored, not fatal */
    CHECK_EQ(config_parse(&c, "bogus_key = 1\n"), 0);

    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write the config header**

`boards/eluminocity-ch21130/companion/src/config.h`:
```c
/* delta-bridge config — parsed from /Storage/delta-bridge.conf (key = value). */
#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_STR_MAX 128

struct config {
    char broker_host[CONFIG_STR_MAX];
    int  broker_port;
    char broker_user[CONFIG_STR_MAX];   /* empty = no auth */
    char broker_pass[CONFIG_STR_MAX];
    char topic_prefix[CONFIG_STR_MAX];
    char device_id[CONFIG_STR_MAX];     /* empty = derive at runtime */
    int  poll_hz;
    char log_level[CONFIG_STR_MAX];
    char log_path[CONFIG_STR_MAX];
};

/* Reset `c` to built-in defaults. */
void config_defaults(struct config *c);

/* Parse `text` (the whole config file) into `c`, overriding defaults for any
 * key present. Unknown keys are ignored. Returns 0 (always succeeds — a missing
 * or partial file just means defaults). Call config_defaults() first. */
int  config_parse(struct config *c, const char *text);

/* Read `path` and parse it. Returns 0 on success, -1 if the file cannot be
 * opened (caller may proceed with defaults). Calls config_defaults() itself. */
int  config_load(struct config *c, const char *path);

#endif /* CONFIG_H */
```

- [ ] **Step 3: Run the test to verify it fails**

Add to `Makefile`:
```makefile
test/test_config: test/test_config.c src/config.c
	$(CC) $(CFLAGS) -Itest -o $@ test/test_config.c src/config.c
```
Run: `make test/test_config`
Expected: FAIL — `config.c` does not exist.

- [ ] **Step 4: Write the config implementation**

`boards/eluminocity-ch21130/companion/src/config.c`:
```c
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->broker_host,  sizeof(c->broker_host),  "127.0.0.1");
    c->broker_port = 1883;
    snprintf(c->topic_prefix, sizeof(c->topic_prefix), "delta-bridge");
    c->poll_hz = 1;
    snprintf(c->log_level, sizeof(c->log_level), "info");
    snprintf(c->log_path,  sizeof(c->log_path),  "/Storage/delta-bridge.log");
}

/* Trim leading/trailing ASCII whitespace in place; returns the new start. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' '  || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static void set_str(char *dst, const char *src)
{
    snprintf(dst, CONFIG_STR_MAX, "%s", src);
}

int config_parse(struct config *c, const char *text)
{
    char line[256];
    const char *p = text;
    while (*p) {
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n')
            p++;

        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if      (!strcmp(key, "broker_host"))  set_str(c->broker_host, val);
        else if (!strcmp(key, "broker_port"))  c->broker_port = atoi(val);
        else if (!strcmp(key, "broker_user"))  set_str(c->broker_user, val);
        else if (!strcmp(key, "broker_pass"))  set_str(c->broker_pass, val);
        else if (!strcmp(key, "topic_prefix")) set_str(c->topic_prefix, val);
        else if (!strcmp(key, "device_id"))    set_str(c->device_id, val);
        else if (!strcmp(key, "poll_hz"))      c->poll_hz = atoi(val);
        else if (!strcmp(key, "log_level"))    set_str(c->log_level, val);
        else if (!strcmp(key, "log_path"))     set_str(c->log_path, val);
        /* unknown key: ignored */
    }
    if (c->poll_hz < 1)
        c->poll_hz = 1;
    return 0;
}

int config_load(struct config *c, const char *path)
{
    config_defaults(c);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return config_parse(c, buf);
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test/test_config && ./test/test_config`
Expected: `test/test_config.c: 13/13 passed`, exit 0.

Append `test/test_config` to the `TESTS` variable in the `Makefile`, then run
`make test` to confirm the whole suite still passes.

- [ ] **Step 6: Write the example config**

`boards/eluminocity-ch21130/companion/delta-bridge.conf.example`:
```ini
# delta-bridge configuration — install as /Storage/delta-bridge.conf
broker_host  = 192.168.1.10
broker_port  = 1883
# broker_user = mqttuser
# broker_pass = mqttpass
topic_prefix = delta-bridge
# device_id left blank -> derived at runtime
poll_hz      = 1
log_level    = info
log_path     = /Storage/delta-bridge.log
```

- [ ] **Step 7: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/config.h \
        boards/eluminocity-ch21130/companion/src/config.c \
        boards/eluminocity-ch21130/companion/test/test_config.c \
        boards/eluminocity-ch21130/companion/delta-bridge.conf.example \
        boards/eluminocity-ch21130/companion/Makefile
git commit -m "eluminocity-companion: config-file parser"
```

---

### Task 11: main.c — wiring, poll loop, signals; cross-compile gate

**Files:**
- Create: `boards/eluminocity-ch21130/companion/src/backoff.h`
- Create: `boards/eluminocity-ch21130/companion/src/main.c`
- Create: `boards/eluminocity-ch21130/companion/test/test_backoff.c`
- Modify: `boards/eluminocity-ch21130/companion/Makefile`

The poll loop itself is integration-verified on hardware (M0). The one piece of
non-trivial pure logic — the reconnect backoff — is extracted to `backoff.h` and
unit-tested.

- [ ] **Step 1: Write the failing backoff test**

`boards/eluminocity-ch21130/companion/test/test_backoff.c`:
```c
#include "test_harness.h"
#include "backoff.h"

int main(void)
{
    /* doubles from 1s, capped at 60s */
    CHECK_EQ(backoff_next(0),  1);
    CHECK_EQ(backoff_next(1),  2);
    CHECK_EQ(backoff_next(2),  4);
    CHECK_EQ(backoff_next(32), 60);
    CHECK_EQ(backoff_next(60), 60);
    CHECK_EQ(backoff_next(100), 60);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write backoff.h**

`boards/eluminocity-ch21130/companion/src/backoff.h`:
```c
/* Reconnect backoff: double from 1s, cap at 60s. Header-only (pure). */
#ifndef BACKOFF_H
#define BACKOFF_H

#define BACKOFF_MAX_S 60

static inline int backoff_next(int cur_s)
{
    int n = (cur_s < 1) ? 1 : cur_s * 2;
    return n > BACKOFF_MAX_S ? BACKOFF_MAX_S : n;
}

#endif /* BACKOFF_H */
```

- [ ] **Step 3: Run the backoff test to verify it fails then passes**

Add to `Makefile`:
```makefile
test/test_backoff: test/test_backoff.c src/backoff.h
	$(CC) $(CFLAGS) -Itest -o $@ test/test_backoff.c
```
Also add `test/test_backoff` to the `TESTS` variable.
Run: `make test/test_backoff && ./test/test_backoff`
Expected: `test/test_backoff.c: 6/6 passed`, exit 0.

- [ ] **Step 4: Write main.c**

`boards/eluminocity-ch21130/companion/src/main.c`:
```c
/* delta-bridge — companion MQTT bridge entry point.
 *
 * Loop: attach shmem (retry+backoff) -> init adapter (retry+backoff) ->
 *       every 1/poll_hz s: read state, diff, publish dirty fields, tick.
 * Read-only: never writes the shmem segment, never touches /dev/watchdog. */
#include "config.h"
#include "shmem.h"
#include "charger_state.h"
#include "northbound.h"
#include "mqtt_adapter.h"
#include "backoff.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Sleep `s` seconds but wake early on a stop signal. */
static void interruptible_sleep(int s)
{
    for (int i = 0; i < s && !g_stop; i++)
        sleep(1);
}

int main(int argc, char **argv)
{
    const char *conf_path = (argc > 1) ? argv[1]
                                       : "/Storage/delta-bridge.conf";
    struct config cfg;
    if (config_load(&cfg, conf_path) != 0)
        fprintf(stderr, "delta-bridge: no config at %s, using defaults\n",
                conf_path);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* device_id: config value, else a fixed fallback (M0 will wire the real
     * serial source — /Storage/SerialNumber or a shmem offset). */
    const char *device_id = cfg.device_id[0] ? cfg.device_id : "evmu30";

    struct mqtt_adapter_config acfg = {
        .broker_host  = cfg.broker_host,
        .broker_port  = cfg.broker_port,
        .broker_user  = cfg.broker_user[0] ? cfg.broker_user : NULL,
        .broker_pass  = cfg.broker_pass[0] ? cfg.broker_pass : NULL,
        .topic_prefix = cfg.topic_prefix,
        .device_id    = device_id,
        .keepalive_s  = 60,
    };
    struct northbound nb;
    mqtt_adapter_create(&nb, &acfg);

    /* 1. attach shmem, retrying — we may have raced Delta's `main` at boot */
    struct shmem sm;
    int bo = 0;
    while (!g_stop && shmem_attach(&sm) != 0) {
        bo = backoff_next(bo);
        fprintf(stderr, "delta-bridge: shmem not ready, retry in %ds\n", bo);
        interruptible_sleep(bo);
    }

    struct charger_state prev, cur;
    charger_state_init(&prev);
    int adapter_up = 0;
    int period_us  = 1000000 / cfg.poll_hz;
    bo = 0;

    while (!g_stop) {
        /* 2. ensure the adapter is up */
        if (!adapter_up) {
            if (nb.init(&nb) == 0) {
                adapter_up = 1;
                bo = 0;
                charger_state_init(&prev);   /* force a full publish */
            } else {
                bo = backoff_next(bo);
                fprintf(stderr, "delta-bridge: broker down, retry in %ds\n", bo);
                interruptible_sleep(bo);
                continue;
            }
        }

        /* 3. read + publish */
        charger_state_read(&cur, &sm);
        unsigned int dirty = charger_state_diff(&prev, &cur);
        int full = (prev.evse_state == EVSE_STATE_UNKNOWN &&
                    prev.voltage_v == 0 && prev.current_a == 0);
        if (full || dirty) {
            if (nb.publish_state(&nb, &cur, dirty, full) != 0) {
                adapter_up = 0;             /* link down -> reconnect + full */
                continue;
            }
        }
        prev = cur;

        /* 4. housekeeping; tick() reporting down also forces reconnect */
        if (nb.tick(&nb) != 0)
            adapter_up = 0;

        (void)now_ms;                       /* used by the real keepalive path */
        usleep(period_us);
    }

    /* 5. graceful shutdown */
    nb.shutdown(&nb);
    shmem_release(&sm);
    fprintf(stderr, "delta-bridge: stopped\n");
    return 0;
}
```

- [ ] **Step 5: Run the full host test suite**

Run: `make test`
Expected: all of `test_smoke`, `test_shmem`, `test_charger_state`,
`test_mqtt_codec`, `test_mqtt_adapter`, `test_config`, `test_backoff` print
`N/N passed`; `make` exits 0.

- [ ] **Step 6: Cross-compile gate**

Ensure the musl armv5te toolchain is on `PATH` (see board README), then:
```bash
make clean && make
file delta-bridge
```
Expected: `make` exits 0; `file` reports `ELF 32-bit LSB executable, ARM,
EABI5 ... statically linked`.

- [ ] **Step 7: Commit**

```bash
git add boards/eluminocity-ch21130/companion/src/backoff.h \
        boards/eluminocity-ch21130/companion/src/main.c \
        boards/eluminocity-ch21130/companion/test/test_backoff.c \
        boards/eluminocity-ch21130/companion/Makefile
git commit -m "eluminocity-companion: main loop, signals, backoff; cross-compile gate"
```

---

### Task 12: DcoFImage builder

**Files:**
- Create: `boards/eluminocity-ch21130/companion/image/wrap_dco.py`
- Create: `boards/eluminocity-ch21130/companion/image/test_wrap_dco.py`
- Create: `boards/eluminocity-ch21130/companion/image/build-dcofimage.sh`

`wrap_dco.py` is the testable core (the `DELTADCOF` magic + byte-sum trailer
format). `build-dcofimage.sh` is orchestration (rootfs assembly + `mkfs.jffs2`)
verified on hardware at M2.

- [ ] **Step 1: Write the failing wrapper test**

`boards/eluminocity-ch21130/companion/image/test_wrap_dco.py`:
```python
#!/usr/bin/env python3
"""Tests for wrap_dco.py — the DELTADCOF bundle wrapper."""
import wrap_dco

def test_wrap_appends_magic_and_sum():
    payload = bytes([0x01, 0x02, 0x03, 0xFE])
    out = wrap_dco.wrap(payload)
    # payload, then 9-byte ASCII magic, then BE-u32 byte-sum
    assert out[:4] == payload
    assert out[4:13] == b"DELTADCOF"
    expected_sum = sum(payload) & 0xFFFFFFFF
    assert int.from_bytes(out[13:17], "big") == expected_sum
    assert len(out) == len(payload) + 13

def test_roundtrip_unwrap():
    payload = bytes(range(256)) * 3
    out = wrap_dco.wrap(payload)
    assert wrap_dco.unwrap(out) == payload

def test_unwrap_rejects_bad_magic():
    try:
        wrap_dco.unwrap(b"x" * 20)
    except ValueError:
        return
    assert False, "expected ValueError on bad magic"

if __name__ == "__main__":
    test_wrap_appends_magic_and_sum()
    test_roundtrip_unwrap()
    test_unwrap_rejects_bad_magic()
    print("test_wrap_dco: 3/3 passed")
```

- [ ] **Step 2: Run the test to verify it fails**

Run (from `companion/image/`):
```bash
python3 test_wrap_dco.py
```
Expected: FAIL — `ModuleNotFoundError: No module named 'wrap_dco'`.

- [ ] **Step 3: Write wrap_dco.py**

`boards/eluminocity-ch21130/companion/image/wrap_dco.py`:
```python
#!/usr/bin/env python3
"""DELTADCOF bundle wrapper for the Eluminocity CH-21130 / Delta EVMU30.

Our unit's UpdateCSU() accepts /UsbFlash/DcoFImage when it carries the legacy
9-byte ASCII magic "DELTADCOF" followed by a big-endian uint32 byte-sum of the
payload. (The newer AC Mini Plus uses a different model-string trailer — we do
NOT use that format here.)

    python3 wrap_dco.py <payload-in> <DcoFImage-out>
"""
import sys

MAGIC = b"DELTADCOF"

def wrap(payload: bytes) -> bytes:
    checksum = sum(payload) & 0xFFFFFFFF
    return payload + MAGIC + checksum.to_bytes(4, "big")

def unwrap(bundle: bytes) -> bytes:
    if len(bundle) < 13 or bundle[-13:-4] != MAGIC:
        raise ValueError("not a DELTADCOF bundle (bad magic)")
    payload = bundle[:-13]
    stored = int.from_bytes(bundle[-4:], "big")
    if (sum(payload) & 0xFFFFFFFF) != stored:
        raise ValueError("DELTADCOF checksum mismatch")
    return payload

def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1
    with open(argv[1], "rb") as f:
        payload = f.read()
    with open(argv[2], "wb") as f:
        f.write(wrap(payload))
    print(f"wrote {argv[2]} ({len(payload)} + 13 bytes)")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 test_wrap_dco.py`
Expected: `test_wrap_dco: 3/3 passed`, exit 0.

- [ ] **Step 5: Write the image build script**

`boards/eluminocity-ch21130/companion/image/build-dcofimage.sh`:
```bash
#!/bin/sh
# build-dcofimage.sh — assemble a USB-flashable DcoFImage for the Delta EVMU30.
#
# Produces, in the output dir:
#   DcoFImage                — stock rootfs + delta-bridge + dropbear + autostart
#   DcoFImage-stock-restore  — untouched stock rootfs, same wrapper (revert path)
#
# Usage:
#   build-dcofimage.sh <stock-rootfs-dir> <delta-bridge-binary> <output-dir> \
#                      [--authorized-key <pubkey-file>] [--dropbear <binary>]
#
# Requires: mkfs.jffs2 (mtd-utils), python3.
# mtd5 geometry — MUST match the unit (validated at milestone M2 by flashing
# DcoFImage-stock-restore first and confirming an identical boot):
#   erase block size 128 KiB, little-endian, 16 MiB total.
set -e

STOCK_ROOTFS="$1"
BRIDGE_BIN="$2"
OUT_DIR="$3"
shift 3 || { echo "usage: see header"; exit 1; }

AUTH_KEY=""
DROPBEAR_BIN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --authorized-key) AUTH_KEY="$2"; shift 2 ;;
        --dropbear)       DROPBEAR_BIN="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

JFFS2_OPTS="--little-endian --eraseblock=0x20000 --pad=0x1000000"
HERE="$(dirname "$0")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT_DIR"

# --- 1. stock-restore image: untouched rootfs, just wrapped ---
mkfs.jffs2 $JFFS2_OPTS -r "$STOCK_ROOTFS" -o "$WORK/stock.jffs2"
python3 "$HERE/wrap_dco.py" "$WORK/stock.jffs2" "$OUT_DIR/DcoFImage-stock-restore"

# --- 2. our image: copy rootfs, inject bridge + dropbear + autostart ---
cp -a "$STOCK_ROOTFS" "$WORK/rootfs"
install -m 0755 "$BRIDGE_BIN" "$WORK/rootfs/root/delta-bridge"

# autostart line(s) appended to /etc/funs
echo '/root/delta-bridge &' >> "$WORK/rootfs/etc/funs"

if [ -n "$DROPBEAR_BIN" ]; then
    install -m 0755 "$DROPBEAR_BIN" "$WORK/rootfs/sbin/dropbear"
    mkdir -p "$WORK/rootfs/etc/dropbear"
    # host keys are generated on the unit at first boot onto /Storage; the
    # autostart line points dropbear there. key-only auth (-s: no passwords).
    echo 'mkdir -p /Storage/dropbear' >> "$WORK/rootfs/etc/funs"
    echo '[ -f /Storage/dropbear/dropbear_rsa_host_key ] || dropbearkey -t rsa -f /Storage/dropbear/dropbear_rsa_host_key' \
        >> "$WORK/rootfs/etc/funs"
    echo '/sbin/dropbear -s -r /Storage/dropbear/dropbear_rsa_host_key &' \
        >> "$WORK/rootfs/etc/funs"
    if [ -n "$AUTH_KEY" ]; then
        mkdir -p "$WORK/rootfs/root/.ssh"
        install -m 0600 "$AUTH_KEY" "$WORK/rootfs/root/.ssh/authorized_keys"
    fi
fi

mkfs.jffs2 $JFFS2_OPTS -r "$WORK/rootfs" -o "$WORK/ours.jffs2"
python3 "$HERE/wrap_dco.py" "$WORK/ours.jffs2" "$OUT_DIR/DcoFImage"

echo "built:"
echo "  $OUT_DIR/DcoFImage"
echo "  $OUT_DIR/DcoFImage-stock-restore"
```

- [ ] **Step 6: Verify the script parses and shows usage**

Run:
```bash
sh -n build-dcofimage.sh && echo "syntax OK"
```
Expected: `syntax OK`. (A full build run is a hardware-validation step — M2 —
not part of this task; it needs the stock rootfs tree and `mkfs.jffs2`.)

- [ ] **Step 7: Commit**

```bash
chmod +x boards/eluminocity-ch21130/companion/image/build-dcofimage.sh
git add boards/eluminocity-ch21130/companion/image/wrap_dco.py \
        boards/eluminocity-ch21130/companion/image/test_wrap_dco.py \
        boards/eluminocity-ch21130/companion/image/build-dcofimage.sh
git commit -m "eluminocity-companion: DcoFImage builder + DELTADCOF wrapper"
```

---

### Task 13: dropbear provisioning notes

**Files:**
- Create: `boards/eluminocity-ch21130/companion/image/dropbear/README.md`

dropbear is not built here — the build script (Task 12) takes the binary as a
`--dropbear` argument. This task documents where to get it. Lifting the binary
or cross-compiling it is a bench step, not a CI step.

- [ ] **Step 1: Write the dropbear provisioning notes**

`boards/eluminocity-ch21130/companion/image/dropbear/README.md`:
```markdown
# dropbear for the Delta EVMU30

`build-dcofimage.sh --dropbear <binary>` injects an SSH daemon into the
distribution image. dropbear is *not* built here — provision the binary one of
two ways.

## Option A — lift it from the AC Mini Plus firmware (preferred, try first)

The Delta AC Mini Plus firmware (V02.0B.06) ships a `dropbear` binary built for
the same SoC (SPEAr320), the same 2.6.37.6 kernel family, and the same
STLinux/glibc-2.10 toolchain era. It very likely runs on our older EVMU3015
as-is.

1. Unpack the V02.0B.06 `DcoFImage` rootfs (see the firmware-comparison notes).
2. Extract `/sbin/dropbear` and `/usr/bin/dropbearkey`.
3. Bench-test: `cp` it to the unit's `/Storage`, run `dropbear -s -r <hostkey>`,
   confirm an SSH login works.
4. If it runs, commit nothing binary to the repo — keep the binary out of git;
   document the V02.0B.06 source here and pass it via `--dropbear`.

## Option B — cross-compile (fallback, only if Option A fails)

Build dropbear with the musl armv5te toolchain (same `CROSS` prefix as the
bridge). Static, `--disable-zlib`, password auth disabled at build time.

## Runtime posture (enforced by build-dcofimage.sh)

- Key-only auth (`dropbear -s`) — no password login. The stock `vern` account
  password is unknown anyway.
- Host keys generated at **first boot** onto `/Storage/dropbear/` so they
  survive a rootfs re-flash.
- `authorized_keys` supplied to the image builder via `--authorized-key`.
```

- [ ] **Step 2: Commit**

```bash
git add boards/eluminocity-ch21130/companion/image/dropbear/README.md
git commit -m "eluminocity-companion: dropbear provisioning notes"
```

---

### Task 14: CI workflow

**Files:**
- Create: `.github/workflows/eluminocity-companion.yml`

- [ ] **Step 1: Write the workflow**

`.github/workflows/eluminocity-companion.yml`:
```yaml
name: eluminocity-companion

on:
  push:
    paths:
      - 'boards/eluminocity-ch21130/**'
      - '.github/workflows/eluminocity-companion.yml'
  pull_request:
    paths:
      - 'boards/eluminocity-ch21130/**'
      - '.github/workflows/eluminocity-companion.yml'

jobs:
  host-tests:
    runs-on: ubuntu-latest
    defaults:
      run:
        working-directory: boards/eluminocity-ch21130/companion
    steps:
      - uses: actions/checkout@v4
      - name: Host unit tests
        run: make test
      - name: DcoFImage wrapper tests
        run: cd image && python3 test_wrap_dco.py
      - name: Image build script syntax
        run: cd image && sh -n build-dcofimage.sh

  cross-compile:
    runs-on: ubuntu-latest
    defaults:
      run:
        working-directory: boards/eluminocity-ch21130/companion
    steps:
      - uses: actions/checkout@v4
      - name: Fetch musl armv5te toolchain
        run: |
          cd /tmp
          curl -fsSL -O https://musl.cc/armv5l-linux-musleabi-cross.tgz
          tar xzf armv5l-linux-musleabi-cross.tgz
          echo "/tmp/armv5l-linux-musleabi-cross/bin" >> "$GITHUB_PATH"
      - name: Cross-compile delta-bridge
        run: make clean && make
      - name: Verify it is a static ARM binary
        run: file delta-bridge | grep -E 'ARM.*statically linked'
```

- [ ] **Step 2: Validate the YAML**

Run (from the OpenEVCharger repo root):
```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/eluminocity-companion.yml')); print('YAML OK')"
```
Expected: `YAML OK`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/eluminocity-companion.yml
git commit -m "eluminocity-companion: CI — host tests + cross-compile"
```

---

## Post-implementation: bench validation (not code tasks)

These milestones run on hardware and are tracked in `projectstate-delta.txt`,
not as plan steps:

- **M0** — serial-deploy `delta-bridge` to `/root`, run it, confirm shmem attach
  + entities appear in HA. Wire the real `device_id` source (`/Storage/SerialNumber`
  or a shmem offset) and confirm the connector-state enum + offset map against
  live values.
- **M1** — validate scaling against a multimeter / known load; tune the two
  scaling lines in `charger_state.c` and the offsets in `shmem_offsets.h`.
- **M2** — flash `DcoFImage-stock-restore` via USB; confirm an identical boot
  (validates the builder + the JFFS2 geometry + the `UpdateCSU` USB path).
- **M3** — flash the full `DcoFImage` via USB; confirm cold-flash autostart +
  SSH + HA.

---

## Self-review

**Spec coverage:**
- shmem read-only accessor layer → Task 3 ✓
- charger_state normalize/scale/diff/faults → Tasks 4, 5 ✓
- northbound adapter seam → Task 6 ✓
- MQTT 3.1.1 client → Tasks 7 (codec), 8 (transport) ✓
- mqtt_adapter HA discovery + per-field publish + LWT → Task 9 ✓
- config `/Storage/delta-bridge.conf` → Task 10 ✓
- main poll loop + signals + retry/backoff → Task 11 ✓
- on-device paths, autostart via `/etc/funs` → Task 12 (build script) ✓
- DcoFImage builder + stock-restore image → Task 12 ✓
- dropbear in the image builder, key-only, first-boot host keys → Tasks 12, 13 ✓
- RE docs migrated into the board → Task 2 ✓
- board README, companion-only, no `board.cmake` → Task 1 ✓
- standalone musl-cross Makefile, not the CMake matrix → Task 1 ✓
- host unit tests per layer + cross-compile gate → Tasks 1–11 ✓
- CI workflow alongside `fc41d-config.yml` → Task 14 ✓
- Safety: read-only attach, no `IPC_CREAT`, no `/dev/watchdog`, bounded socket
  timeouts, defensive accessors, graceful `SIGTERM` → Tasks 3, 8, 11 ✓
- Out-of-scope items (shmem writes, OCPP 1.6-J, TLS, real SUBSCRIBE) are not
  implemented; the SUBSCRIBE *codec stub* and the northbound interface leave the
  seams → Tasks 6, 7 ✓

**Spec deviations (intentional, noted):**
- Offsets reconciled to `decode_sharemem.py` (spec's `+0x00/+0x04` were ttyAMA1
  frame offsets); all isolated in `shmem_offsets.h`, bench-verify-pending.
- Config split into `config.c` (spec said "may live in main.c") purely for unit
  testability — `main.c` still just loads + parses.

**Placeholder scan:** no TBD/TODO; `RESERVED_nn` fault names are real shippable
values, refined at M0/M1; all code blocks are complete and compile.

**Type consistency:** `struct shmem`, `struct charger_state`, `struct northbound`,
`struct mqtt_client`/`mqtt_config`, `struct mqtt_adapter_config`, `struct config`
and the `CS_DIRTY_*` / `EVSE_STATE_*` / `OFF_*` names are used identically across
every task that references them.
