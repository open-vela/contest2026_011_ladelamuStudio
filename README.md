# openvela D13x Hengshan-Pi port

This repository contains the contest-owned sources required to port openvela
to the ArtInChip D13x Hengshan-Pi board (D133EBS, Xuantie E907).

## Hardware-verified baseline

The tracked source is the frozen `step20b` baseline tested on 2026-07-13.

| Function | Status | Verification |
| --- | --- | --- |
| SPI NOR boot | Verified | PBP and tinySPL load and enter NuttX |
| UART0 console | Verified | 115200 8N1, interactive NSH input and output |
| NuttX VFS | Verified | repeated `ls /dev` completes without an exception |
| I2C2 | Verified at registration | `/dev/i2c2` is present |
| LVDS display | Verified | `/dev/fb0`, 1024x600 color bars, PE13 backlight |
| GT911 touch | Source present, disabled | not enabled until runtime validation is complete |
| System timer | IRQ masked | the unverified timer experiment is intentionally excluded |

The display image validated on hardware has SHA-256:

```text
64e102b1d68e4b8148ac30f4c57941149c851348b1f32fe2db97b25fbf871855
```

## Repository layout

```text
board/d13x-hengshan-pi/   board code, NSH defconfig and image inputs
chip/d13x/                D13x startup, IRQ, UART, I2C and display drivers
nuttx-overlay/            required changes to the upstream NuttX tree
vendor-overlay/           D13x ArtInChip packer inputs and pack script
scripts/integrate.sh      install contest sources into an openvela workspace
scripts/build.sh          integrate, configure, build and pack
logs/                     official AI coding logs
```

Generated object files, temporary ROMFS headers and historical test images are
not source artifacts and are not part of this port.

## Build

The workspace must contain sibling `nuttx`, `apps`, and `vendor/artinchip`
trees from the official `dev-ai-contest-2026` branches. The contest manifest
provides the new board and chip paths; the integration script applies the
tracked NuttX and vendor overlays.

Prerequisites include Python 3, CMake, Ninja, Kconfig tools and an RV32 GNU
bare-metal toolchain available as `riscv32-unknown-elf-*` or
`riscv-none-elf-*`.

```bash
cd contest2026_011_ladelamuStudio
./scripts/build.sh /path/to/openvela-workspace
```

The resulting image is:

```text
vendor/artinchip/pack/prebuilt/d13x_hengshan-pi_v1.0.0.img
```

The 16 MiB SPI NOR layout totals 15 MiB:

| Partition | Size |
| --- | ---: |
| spl | 512 KiB |
| env + env_r | 256 KiB |
| userid | 256 KiB |
| os | 3 MiB |
| rodata | 10 MiB |
| data | 1 MiB |

## Flash and smoke test

1. Hold BOOT while connecting the board, then flash the generated `.img` with
   AiBurn.
2. Connect UART0 TX, RX and GND to a 3.3 V USB-TTL adapter.
3. Open the console at 115200 8N1 with hardware and software flow control off.
4. Run `echo RX_OK` and repeat `ls /dev`.
5. Confirm `/dev/i2c2` and `/dev/fb0`, backlight, and the 1024x600 color bars.

## Important implementation notes

- `d13x_head.S` enters C with `jal x1, __start_c`; the linked target and C
  prologue are retained from the hardware-verified image.
- NuttX executes from PSRAM at `0x30040000`; the image entry and linker script
  use the same address.
- The frozen CORET/GTC path remains disabled because enabling IRQ 7 was not
  part of the verified display baseline.
- Touch code is retained for the next feature commit but its Kconfig option is
  off in `configs/nsh/defconfig`.

## License

Apache-2.0
