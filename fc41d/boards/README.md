# Custom LibreTiny board: generic-bk7231n-qfn32-quectel

Same hardware as LibreTiny's `generic-bk7231n-qfn32-tuya` but with the OTA
`download` partition shifted from `0x12A000` to `0x132000` to match Beken's
canonical layout (see `middleware/driver/flash/flash_partition.c` in
`bekencorp/armino`). The stock Quectel bootloader on FC41D modules reads
the staged OTA image from `0x132000`; LibreTiny's default Tuya layout
writes it to `0x12A000`, which the bootloader never looks at — so OTA
silently rolls back.

## Setup

LibreTiny only resolves boards from `~/.platformio/platforms/libretiny/boards/`,
so symlink this directory's JSON into place once after cloning:

```sh
ln -sfn "$(pwd)/boards/generic-bk7231n-qfn32-quectel.json" \
        ~/.platformio/platforms/libretiny/boards/generic-bk7231n-qfn32-quectel.json
```

The symlink survives LibreTiny upgrades because we never touch LibreTiny's
own files — we only add one new entry to its boards directory.

## Layout diff vs upstream `beken-7231n.json`

|              | upstream Tuya         | this variant          |
|--------------|-----------------------|-----------------------|
| `bootloader` | 0x000000 + 0x11000    | 0x000000 + 0x11000    |
| `app`        | 0x011000 + 0x119000   | 0x011000 + 0x119000   |
| `download`   | **0x12A000** + 0xA6000 | **0x132000** + 0xAE000 |
| `calibration`| 0x1D0000 + 0x1000     | 0x1D0000 + 0x1000     |
| `net`        | 0x1D1000 + 0x1000     | 0x1D1000 + 0x1000     |
| `tlv`        | 0x1D2000 + 0x1000     | 0x1D2000 + 0x1000     |
| `kvs`        | 0x1D3000 + 0x8000     | 0x1D3000 + 0x8000     |
| `userdata`   | 0x1DB000 + 0x25000    | 0x1DB000 + 0x25000    |

Only `download` moves. The 8 KB gap between the end of `app` (`0x129FFF`)
and the new start of `download` (`0x132000`) is intentionally unused.

## Upstream PR

[libretiny-eu/libretiny#379](https://github.com/libretiny-eu/libretiny/pull/379)
adds a `generic-bk7231n-qfn32` board (no Tuya overlay) that inherits the
canonical `download: 0x132000` from `beken-72xx.json`. Once that lands,
this local variant becomes redundant — switch the YAML to the upstream
board name and drop the symlink.
