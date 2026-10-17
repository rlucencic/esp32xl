#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The Atari ROMs: from the xl_roms flash partition (roms/<name> in the
 * project, written by idf.py flash into 16 KB slots), else from the SD
 * card's "roms" folder under the same names.
 */
typedef enum {
    ROM_OS,                     /* atarixl.rom: XL/XE OS, 16 KB */
    ROM_BASIC,                  /* ataribas.rom: Atari BASIC, 8 KB */
    ROM_COUNT
} rom_id_t;

const char *rom_store_name(rom_id_t id);    /* "atarixl.rom" */
uint32_t rom_store_size(rom_id_t id);

/* The whole ROM into buf; false if there's none */
bool rom_store_load(rom_id_t id, uint8_t *buf);
