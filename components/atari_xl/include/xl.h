/*
 * Atari 800XL and 130XE, PAL or NTSC.
 *
 * The 6502C (esp32emu's 6502 core: every bus cycle an access, ANTIC's
 * HALT stopping reads and writes alike), ANTIC with its display list and
 * cycle-exact DMA (playfield, character data, player/missile, refresh,
 * WSYNC, DLIs and VBIs at cycle 8), GTIA (players and missiles, the
 * priority logic, collisions, the hi-res and GTIA modes, colour changes in
 * the middle of a line), POKEY (sound, timers, keyboard, serial port), the
 * PIA (joysticks, the XL/XE memory control in PORTB, the SIO command line)
 * and the 130XE's 64 KB of extended memory with separate ANTIC access.
 *
 * Disk drives D1: and D2: answer on the SIO bus at POKEY level, so custom
 * loaders work; calls to the OS's SIO routine are served directly (fast
 * loading) unless turned off. Images: .ATR (128 or 256 byte sectors) and
 * .XFD.
 *
 * Platform independent: the caller provides the ROMs and the big buffers
 * and drives the emulation one video frame at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    XL_MODEL_800XL,             /* 64 KB */
    XL_MODEL_130XE,             /* 128 KB: 64 KB more in 16 KB banks at $4000 */
    XL_MODEL_COUNT
} xl_model_t;

typedef enum {
    XL_TV_PAL,                  /* 312 lines, 49.86 Hz, 1.773 MHz */
    XL_TV_NTSC,                 /* 262 lines, 59.92 Hz, 1.790 MHz */
} xl_tv_t;

/* The picture: colour clocks 32-223 as hires pixels (two per colour
 * clock; 34-221 are visible, the rest is black) and scan lines 8-247, the
 * most ANTIC ever displays. Each pixel is an Atari colour number (hue in
 * the high nibble, luminance in the low).
 */
#define XL_SCREEN_WIDTH         384
#define XL_SCREEN_HEIGHT        240
#define XL_LINE_CYCLES          114

typedef struct {
    xl_model_t model;
    xl_tv_t tv;
    bool basic;                 /* BASIC on (else OPTION is held while the OS starts) */
    bool sio_patch;             /* serve the OS's SIO calls for the drives directly */
    uint8_t *ram;               /* 64 KB (800XL) or 128 KB (130XE) */
    const uint8_t *os;          /* XL/XE OS, 16 KB */
    const uint8_t *basic_rom;   /* Atari BASIC, 8 KB (NULL: none) */
    uint8_t *frame;             /* XL_SCREEN_WIDTH x XL_SCREEN_HEIGHT colour numbers */
} xl_config_t;

typedef struct {
    uint32_t frames;
    uint16_t pc;
    uint8_t portb;              /* memory control as the MMU sees it */
    uint8_t dmactl, nmien;
    uint16_t dlist;
    int sio_led;                /* drive busy, -1 none */
} xl_status_t;

bool xl_init(const xl_config_t *config);
void xl_reset(void);            /* power off and on */
void xl_warm_reset(void);       /* the Reset key (the next frame) */
void xl_run_frame(void);
void xl_get_status(xl_status_t *status);
uint8_t xl_peek(uint16_t address);  /* as the CPU sees it, no side effects */
void xl_set_sio_patch(bool on);

/* ---- Screen ---- */

int xl_screen_hz(void);         /* 50 or 60 */
void xl_render_line_rgb565(int y, uint16_t *dst);
uint32_t xl_screen_signature(void);
/* The palette in use, RGB565 (256 entries) */
const uint16_t *xl_palette(void);

/* ---- Sound: 16-bit stereo, POKEY in both channels (and the console
 * speaker)
 */
#define XL_AUDIO_HZ             31250

void xl_audio_enable(bool enable);
int xl_audio_render(int16_t *dst, int frames);
int xl_audio_pending(void);

/* ---- Disk drives D1: and D2: (0, 1) ----
 *
 * .ATR (16-byte header; 128 or 256 byte sectors, the first three always
 * 128, stored packed or padded) and .XFD (raw). Read into memory; writes
 * go back to the file with xl_disk_flush(). Not while xl_run_frame() runs.
 */
#define XL_DRIVES               2

bool xl_disk_insert(int drive, const char *path);
void xl_disk_eject(int drive);
const char *xl_disk_path(int drive);        /* NULL when empty */
bool xl_disk_write_protected(int drive);
bool xl_disk_dirty(void);
void xl_disk_flush(void);

/* Blank (unformatted) disks: single density (720 x 128), enhanced density
 * (1040 x 128, the 1050's), double density (720 x 256)
 */
#define XL_DISK_SD_BYTES        (16 + 720 * 128)
#define XL_DISK_ED_BYTES        (16 + 1040 * 128)
#define XL_DISK_DD_BYTES        (16 + 3 * 128 + 717 * 256)
bool xl_disk_create(const char *path, uint32_t bytes);

typedef struct {
    int light;                  /* drive busy, -1 none */
    uint32_t reads[XL_DRIVES];  /* sectors */
    uint32_t writes[XL_DRIVES];
    uint16_t sector[XL_DRIVES]; /* last one accessed */
} xl_disk_status_t;

void xl_disk_status(xl_disk_status_t *status);

/* ---- Keyboard, console keys and joysticks: safe from another task ----
 *
 * Keys are POKEY key codes 0-63 (KBCODE without Shift and Control: $0C
 * Return, $21 Space, $3F A, ...) and the keys below.
 */
enum {
    XL_KEY_SHIFT = 64, XL_KEY_CTRL, XL_KEY_BREAK,
    XL_KEY_START, XL_KEY_SELECT, XL_KEY_OPTION,
    XL_KEYS
};
void xl_key(uint8_t key, bool down);
void xl_keys_up(void);

/* Type text: one key every few frames. ASCII, '\n' is Return. Replaces
 * what's still queued.
 */
void xl_type(const char *text);

/* The key code for an ASCII character (*shift: with Shift); -1 if none */
int xl_ascii_key(char c, bool *shift);

enum {
    XL_JOY_UP = 0x01, XL_JOY_DOWN = 0x02, XL_JOY_LEFT = 0x04, XL_JOY_RIGHT = 0x08,
    XL_JOY_FIRE = 0x10,
};
/* Port 1 or 2 */
void xl_joystick(int port, uint8_t bits);
