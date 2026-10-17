# Atari ROM images

Put the ROM images here (not part of this repository). `idf.py flash` writes
them into 16 KB slots of the `xl_roms` flash partition.

| File | What | Size | MD5 |
|---|---|---|---|
| `atarixl.rom` | XL/XE OS, revision 2 (800XL, 65XE, 130XE) | 16 KB | `06daac977823773a3eea3422fd26a703` |
| `ataribas.rom` | Atari BASIC, revision C | 8 KB | `0bac0c6a50104045d902df4503a4c30b` |

BASIC is optional: without it the BASIC choice has no effect and the machine
starts like an XL with BASIC switched off.

Without flashing, the emulator also finds them in a `roms` folder on the SD
card under the same names (any case).

These are the files commonly named `ATARIXL.ROM` and `ATARIBAS.ROM`, for
instance in the Xformer and Atari800 emulator distributions.
