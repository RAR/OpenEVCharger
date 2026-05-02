# Recovery directory

Stock-firmware backups live here. Files are gitignored (large binaries, locally
reproducible by SWD-dumping the bench unit before any flash).

## Files

| File | Source | Restore script |
|---|---|---|
| `stock-mcu-V1.0.066.bin` | SWD dump of GD32F205VC main flash, 256 KB | `tools/flash_stock.sh` (added in Task 8) |

## Recovering the bench unit

If a new flash bricks the unit, restore stock with:

```sh
./tools/flash_stock.sh
```

This writes `recovery/stock-mcu-V1.0.066.bin` back to flash starting at
`0x08000000`. The unit will boot to stock V1.0.066 firmware afterwards.
