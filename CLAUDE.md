# esp32xl: Atari 800XL / 130XE on the Makerfabs MaTouch ESP32-S3 7" and the M5Stack Tab5

An Atari 800XL (64 KB) and 130XE (128 KB), PAL or NTSC, Atari BASIC on or
off: 6502C on the shared 6502 core with ANTIC's DMA cycles taken from it,
ANTIC (all modes, scrolling, DLIs), GTIA (P/M, priority logic, collisions,
GTIA modes 9/10/11), POKEY (sound, timers, keyboard, serial port), the PIA
(XL memory control, 130XE banks). Drives D1: and D2: on the SIO bus (1050
with double density), ATR/XFD images, an LVGL setup screen, a web page for
images and live status. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the 6502 core (`components/m6502`) are the shared library in
`../esp32emu`, also used by `../esp32ste`, `../esp32mac`, `../esp32cpc`,
`../esp32zx` and `../esp32cbm` (which shares the 6502 core). See its
CLAUDE.md for the runtime and the board-level lessons; changes there need
all six projects built, for both boards.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM), 1024x600 RGB565 panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `../esp32emu/components/emu/include/emu/board.h`.
The native USB port is the console, `/dev/cu.usbmodem1101` on the user's Mac.
The Tab5 is `/dev/cu.usbmodem3101` (its hardware: `../esp32emu/CLAUDE.md`).

## Build, flash, debug

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 app-flash'
```

- `app-flash` writes only the app; `idf.py flash` also writes the ROMs in
  `roms/` (not in git) into 16 KB slots of the `xl_roms` partition
  (0x810000, 64 KB): `atarixl.rom` 0, `ataribas.rom` 1 (top
  `CMakeLists.txt`, `main/rom_store.c`). The partition overlaps the other
  machines' ROM partitions: switching the board needs a full `idf.py flash`
  there (and here). NVS (WiFi, BLE pairings) is shared.
- Without flashed ROMs, `roms/` on the SD card (any case) works too.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset | profile ELF [N]`
  with the IDF venv's Python (`-I`); in zsh call it through a small wrapper
  script (a command in a variable isn't split into words). One touch per
  `send`.
- `curl http://atari-xl.local/api/status`; disks: see
  `../esp32emu/README.md` (drives 1 D1:, 2 D2:). Both boards are called
  `atari-xl`: one becomes `atari-xl-2` (or `-3`, ...; `/api/info` and the
  page's Board row say which).
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`s` `L` `r` `F` `t` `i` `p`, see
`emu/console.h`; `F 0 NAME` inserts into D1:, `F 0` ejects) and the
Atari's (`main/main.c`): `k <hex>` tap a key (POKEY code: 0C Return, 21
Space, 3F A), `K <hex> 0|1` (40 Shift, 41 Ctrl, 42 Break, 43 Start, 44
Select, 45 Option), `T <text>` type (`\n` is Return; one key every few
frames, each waiting for the OS to take the last: a long text takes ~10 s),
`j <port> <bits>` joystick (1 up 2 down 4 left 8 right 16 fire), `W` the
Reset key, `P <0|1>` the SIO patch, `R <model> [tv]` restart as 0 800XL, 1
130XE; tv 0 PAL, 1 NTSC. Touches: Start `t 850 532` (MaTouch), `t 1065 637`
(Tab5).

## M5Stack Tab5

The same code also builds for the M5Stack Tab5 (ESP32-P4, target esp32p4),
beside the MaTouch build, on `/dev/cu.usbmodem3101`:

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build && \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash'
```

- Its settings: this project's `sdkconfig.defaults`, then
  `../esp32emu/boards/m5stack_tab5/sdkconfig.defaults` (`CMakeLists.txt`),
  into `build-tab5/sdkconfig` (delete it and `reconfigure` after changing
  either). Board details and lessons: `../esp32emu/CLAUDE.md`, "The two
  boards".
- The Tab5 is shared by the projects like the MaTouch: after another
  project, a full `flash` (partition table and ROMs). The MaTouch build
  afterwards recompiles everything once (shared `managed_components/`).
- Measured on the Tab5 (2026-10-17): 100% (paced by the speaker), core 1
  36-44% busy (READY to the mode 10 robot demo); the picture 1152x720 (3x).
  Sound on by default (ES8388); the console stays on USB. The clock comes
  from the Tab5's RTC at boot (the Atari has no clock of its own).
- Not checked by eye yet: sound, touch, the Tab5 Keyboard's keys, Bluetooth
  pairing.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Atari as an `emu_machine_t`: memory, ROMs, start, choices, key map, joystick, drives, status, console |
| `main/rom_store.c` | ROMs from the `xl_roms` slots, else the SD card's `roms/` |
| `main/settings.c` | NVS `atarixl`: `model`, `tv`, `basic`, `joystick` |
| `components/atari_xl/` | The 800XL and 130XE, platform independent (`include/xl.h`) |
| `components/atari_xl/host/` | Host build: `xl_host` (see Testing) |
| `roms/` | `atarixl.rom` (XL/XE OS rev. 2), `ataribas.rom` (BASIC rev. C); see `roms/README.md`, not in git |
| `partitions.csv` | nvs, phy, factory 8 MB, xl_roms 64K (type 0x40), storage |
| `docs/images/` | README screenshots |

## The core (`components/atari_xl`)

The reference for all of it: the Altirra Hardware Reference Manual (Avery
Lee). Each file's header comment has the details.

- `xl.c`: memory map from PORTB (`update_map()`: OS ROM, BASIC, Self Test
  at $5000, the 130XE's banks at $4000 with separate CPU (bit 4) and ANTIC
  (bit 5) access; `xl.ext` = RAM + 64K), ANTIC's own page table
  (`antic_rd`), the PIA (PORTB, CB2 = the SIO command line: $34 asserted,
  $3C released), the bus, the frame loop. PAL 312 lines at 1,773,447 Hz,
  NTSC 262 at 1,789,773; 114 cycles a line. Per line the CPU runs to its
  end, stopping at cycle 8 of lines with a DLI or the VBI (line 248) for
  the NMI (a handler starts at cycle 10 or later), and at POKEY and SIO
  events; VCOUNT changes at cycle 111. BASIC off holds OPTION for the first
  150 frames, as a user would at power-on.
- **The CPU and ANTIC's DMA**: `begin_line()` gives the 6502 core the line's
  tables (`dma_free`, `dma_busy`, `dma_base`, `dma_len`); the core skips busy
  cycles by itself and calls `stall()` only past the table or at
  `dma_stop`. DMA halts the CPU for writes too (`stall_writes`: HALT);
  WSYNC holds reads only (RDY): a write by cycle 103 waits until 105,
  later ones until the next line's 105 (`xl_wsync()` sets `dma_stop`).
- `antic.c`: DMA after the manual: missiles at cycle 0, the instruction at 1,
  players 2-5, LMS/jump addresses 6-7, character names from 26/18/10
  (narrow/normal/wide) and their data 3 cycles after, bitmap data from
  28/20/12 at one fetch per 2, 4 or 8 cycles, HSCROL/2 cycles later, none
  from cycle 106; refresh from 25 every 4 (9), a blocked one at the next
  free cycle. Each combination (P/M DMA, instruction and address fetches,
  fetch rate, width, scroll, first line) is worked out once into a cache of
  64 patterns with next-busy and next-free tables. The line's data is
  fetched as it begins (`fetch_playfield()`, also an FNV key of everything
  that decides its picture); the per-pixel playfield is built only when
  GTIA draws (`antic_build_pf()`). Character rows (`char_row()`): mode 3's
  descenders, rows 8-9, CHACTL inverse/blank/reflection. Display list: LMS,
  jumps, JVB, vertical scroll row counters, DLI on a mode line's last line.
- `gtia.c`: a line is drawn up to the beam before each register write, the
  write taking effect where the chip has it (beam at colour clock 2·cycle −
  2; colours 2c−1, PRIOR 2c, GRAF 2c+1, HPOS/SIZE 2c+3), the rest at the
  line's end. Priority from GTIA's equations into a table of the colours
  selected for each player/playfield combination (`build_priority()`),
  overlaps ORed; the fifth player; multicolour players. Hi-res modes: PF1's
  luminance on PF2, lit pixels collide as PF2. GTIA modes: 9 (16
  luminances), 10 (9 colours: a pixel is an even pair of colour clocks
  shown one colour clock late: `g = ((cc - late) & ~1)`), 11 (16 hues;
  pixel 0 is forced to luminance 0: the black first bar is right). Fast
  paths: pair tables, GTIA modes, blank lines; a line whose key and objects
  are as last frame isn't drawn again (`s_line_key`), and the screen
  signature changes only when a line was drawn. Palette: NTSC hue angle
  180 − 24·hue degrees, PAL from the manual's angle table, saturation 0.21.
  Vblank lines are black; the picture is colour clocks 32-224 (384 pixels)
  of lines 8-247.
- `pokey.c`: timers (N+4, linked N+7, (N+1)·28 or ·114 cycles), IRQST active
  low (bit 3 shows the output register idle, not latched), the keyboard
  (KBCODE, SKSTAT, typing that waits for CH = $FF), the serial port (SEROUT
  loads into the shift register at the next bit clock; a byte is 20 timer-4
  periods), RANDOM from the 17-bit counter. Sound: register writes queued
  with their times, played on the speaker's task at 31,250 Hz, each
  channel integrated over the sample; polynomial counters 4/5/9/17,
  high-pass filters, volume-only mode, DC removal, the console speaker.
- `sio.c`: the bus as a state machine (command frame, ACK/NAK 1 ms later,
  data, Complete/Error; bytes at 19,040 baud into POKEY's serial input),
  drive commands S, R, W/P, !, " (format single/enhanced), N/O (PERCOM). The
  patch: the emulation stops at the OS's SIO routine (`cpu.stop_pc` = where
  SIOV $E459 jumps, read from the ROM) and serves D1:/D2: from the DCB at
  $0300 at once; other devices time out at once ($8A).
- `disk.c`: ATR (16-byte header) and XFD; 256-byte sector images keep the
  first three sectors at 128 bytes, stored packed (3·128 + n·256), padded
  (each in a 256-byte slot), or packed in the first 768 bytes with the data
  from 768 (`geometry()` decides by the size and the contents:
  `boot_stride`, `data_start`). Writes mark the image dirty; `main.c`
  flushes every 2 s. `xl_disk_create()`: zeroed images (90/130/180 KB) for
  DOS to format.

## Persistent state

- NVS `atarixl`: `model` (0 800XL, 1 130XE), `tv` (0 PAL, 1 NTSC), `basic`,
  `joystick` (port 1 or 2). NVS `xldisks`: D1: and D2:'s files.
- SD card: `atari/` (all images; made if missing), `roms/` (optional ROMs);
  new blank disks are `BLANKnnnn.atr`.

## Memory budget (measured on the board)

- PSRAM (6.6 MB heap on the MaTouch): RAM 64/128 KB, ROMs, the 384x240
  picture, the disks; 4.9 MB free while running.
- Internal RAM: ~43 KB free while running (min ~26 KB) with the 32 KB
  instruction cache (`sdkconfig.defaults`).
- Core 1 on the MaTouch, PAL: READY 61%, the robot demo (GTIA mode 10) 69%
  (core 0 40% drawing it); NTSC 65% / 74%. Started at 54% speed and fully
  busy; the DMA tables in the core, the pattern cache and the lazy
  playfield with the line keys brought it to 100%.

## Lessons

- **Typing too early loses keys**: the OS isn't reading the keyboard while
  it boots; and after Return keys were lost until typing waited for CH =
  $FF before each key. Host scripts: type from frame ~150.
- **SEROUT loads at the next bit clock**, not at once: loaded at once, the
  output-ready IRQ came too early and the OS lost it (SIO through POKEY
  failed).
- **256-byte sector ATRs come in three layouts** (packed, padded, packed in
  768): MyDOS's needed the third; a wrong guess read sector 4 from the
  wrong place (or counted 721 sectors).
- **GTIA mode 10 groups colour clocks in even pairs, shown one colour clock
  late**: grouping odd pairs (`((cc-1)&~1)+1`) striped the robot demo (the
  user compared it with a video). It looked like a sprite problem; it was
  the playfield.
- **ANTIC's halt stops writes too, WSYNC doesn't**: hence `stall_writes` in
  the core, and WSYNC through `dma_stop`.
- **Find the time with the profiler** (`emu_console.py profile`): a
  `bus.stall()` call per DMA cycle and drawing unchanged lines were what
  cost; both are gone.
- **Colours cycling after ~11 minutes (PAL) without a key** is the OS's
  attract mode (ATRACT, $4D, counts every 256 frames; at 128 colours are
  XORed with RTCLOK+1 and darkened), not a bug: any key resets it, the
  console keys and the joystick don't.
- **mDNS names drift** between the two boards (`atari-xl`, `-2`, `-3`, ...)
  and the Mac's cache keeps old ones: check `board.name` in the status.

## Testing

- **Host build**: `make -C components/atari_xl/host`, then
  `build/xl_host <os.rom> [frames] [out.ppm]`; options in `host_main.c`:
  `XL_BASIC`, `XL_MODEL=130xe`, `XL_NTSC`, `XL_DISK1/2`, `XL_NOPATCH` (no SIO
  patch: disks through POKEY; set even when empty, so unset it to get the
  patch back), `XL_SCRIPT` (type, key,
  start/select/option/shift/ctrl/break, joy, shot, text, insert, eject,
  reset, warm, peek, dump, flush; lines up to 1024 characters), `XL_TEXT`,
  `XL_WAV`, `XL_TRACE`, `XL_STATUS`, `XL_SIOLOG`.
- **Regression pictures**: BASIC READY, the Self Test, DOS 2.5 (with and
  without the patch), MyDOS, and demos, compared pixel for pixel after
  changes for speed.
- **The 6502 tests** (`../esp32emu/components/m6502/test`): the functional
  test passes, the opcode cycle table is unchanged with the DMA tables.
- Verified on the host: BASIC (PRINT, FOR loops), the Self Test and its
  memory test, DOS 2.5 booting with and without the patch, its directory,
  formatting a disk and copying to D2: both ways (identical images, 968
  free sectors), MyDOS 4.50 in double density, the 130XE's four banks and
  main memory (each written and read back), SOUND 0,121 at 259 Hz, the
  graphics demos on Xformer's demo disks (P/M graphics, GTIA modes, DLIs).
- On the board: both boards at 100%, BASIC typed through the console,
  GRAPHICS 11, the Self Test and its memory test, a demo disk booting from
  D1:, the setup screen, the web page and its API (status, disks, drives).
- Not verified yet: sound by ear, a BLE keyboard on the Atari, joysticks,
  the Tab5 Keyboard, games.

## Open items

- No cartridges, `.XEX`/`.CAS`, `.ATX`/`.PRO`; no cassette, printer, 850.
- ANTIC registers are read as a line begins (mid-line CHBASE/HSCROL
  changes show from the next line); no NTSC artefact colours.
- Disks have no rotational timing (the drive answers at once).
- Public on GitHub (`github.com/rlucencic/esp32xl`, pushed to `main`).
  Licence: MIT (`LICENSE`).

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing. Screenshots: a new file each time, never over an
  existing one.
