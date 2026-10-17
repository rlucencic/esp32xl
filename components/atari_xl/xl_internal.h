/*
 * Between the Atari core's modules.
 *
 * Time is the 6502's cycle counter (cpu.cycles), the machine clock. A scan
 * line is 114 cycles; xl.line_start is when the current one began. ANTIC
 * sets up each line as it begins (its DMA cycles, the playfield, P/M data,
 * a DLI or VBI) and GTIA draws it as the beam passes: up to the point of a
 * register write before the write, the rest at the end of the line.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "m6502.h"
#include "xl.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define XL_LOG(fmt, ...)    ESP_LOGI("xl", fmt, ##__VA_ARGS__)
/* The core's buffers in PSRAM: plain malloc() serves small blocks from
 * internal RAM, which WiFi, Bluetooth and the LCD need
 */
#define xl_malloc(n)        heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define xl_calloc(n, s)     heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define XL_LOG(fmt, ...)    printf("xl: " fmt "\n", ##__VA_ARGS__)
#define xl_malloc(n)        malloc(n)
#define xl_calloc(n, s)     calloc((n), (s))
#endif

#define XL_HOT              M6502_HOT

/* IRQ sources on the CPU's irq line (bits) */
enum { IRQ_POKEY = 0x01, IRQ_PIA = 0x02 };

/* Colour clocks of the picture: 32-223, of which 34-221 are visible */
#define CC_FIRST            32
#define CC_END              224
#define CC_VIS_FIRST        34
#define CC_VIS_END          222

/* ---- xl.c: the machine ---- */

typedef struct {
    xl_config_t cfg;
    m6502_t cpu;
    int lines;                          /* 312 PAL, 262 NTSC */
    uint32_t clock_hz;
    uint64_t line_start;                /* cycle the current scan line began */
    int line;                           /* 0 .. lines - 1 */
    uint32_t frames;
    /* memory */
    uint8_t *ext;                       /* 130XE: the 64 KB above main memory, else NULL */
    const uint8_t *antic_rd[256];       /* ANTIC's view of memory (pages) */
    /* PIA */
    uint8_t porta_out, porta_ddr, pactl;
    uint8_t portb_out, portb_ddr, pbctl;
    uint8_t portb;                      /* PORTB as the memory control sees it */
    /* WSYNC: reads from wsync_from wait until wsync_to */
    uint64_t wsync_from, wsync_to;
    uint16_t sio_entry;                 /* the OS's SIO routine (the patch stops there) */
    bool sio_patch;
    int option_frames;                  /* OPTION held this many more frames (BASIC off) */
} xl_t;

extern xl_t xl;

static inline uint64_t xl_now(void)
{
    return xl.cpu.cycles;
}

/* Cycle within the current scan line (0-113; more if an instruction ran over) */
static inline int xl_line_cycle(void)
{
    return (int)(xl.cpu.cycles - xl.line_start);
}

/* From a bus callback: stop the CPU's run by cycle t (a device event) */
static inline void xl_event_at(uint64_t t)
{
    m6502_lower_until(&xl.cpu, t);
}

void xl_update_irq(void);               /* POKEY's or the PIA's IRQ changed */
void xl_wsync(void);                    /* a write to WSYNC */

/* ---- antic.c ---- */

typedef struct {
    uint8_t dmactl, chactl, hscrol, vscrol, pmbase, chbase, nmien, nmist;
    uint16_t dlist;                     /* display list counter */
    uint16_t msc;                       /* memory scan counter */
    uint8_t ir;                         /* instruction register */
    uint8_t row, end_row;               /* row counter, and its value on the mode line's last line */
    bool new_mode_line;                 /* the next display line fetches an instruction */
    bool vs_prev;                       /* the previous mode line was vertically scrolled */
    bool jvb;                           /* waiting for vertical blank */
    bool first;                         /* this line is its mode line's first */
    bool hires;                         /* this line is a hires mode (2, 3, F) */
    bool display;                       /* lines 8-247 */
    uint8_t buf[48];                    /* line buffer: character names or bitmap */
    /* This line's DMA (from a cache of patterns): cycles taken from the CPU,
     * the first busy and the first free cycle from c on (114: none)
     */
    const uint8_t *busy, *next_busy, *next_free;
    int nmi;                            /* this line's NMI: 0, 0x80 DLI, 0x40 VBI */
    bool nmi_checked;
    bool pf_blank;                      /* pf is all background */
    bool pf_built;                      /* pf holds this line's playfield (antic_build_pf()) */
    uint32_t key;                       /* what decides this line's playfield (0: unknown) */
    /* The playfield of this line as GTIA sees it, one byte per hires pixel
     * of the picture (colour clocks 32-223): PF_* codes
     */
    uint8_t pf[XL_SCREEN_WIDTH] __attribute__((aligned(4)));
} antic_t;

extern antic_t antic;

/* Playfield codes */
enum { PF_BAK, PF_0, PF_1, PF_2, PF_3, PF_HIRES_ON };

void antic_reset(void);
void antic_begin_line(void);            /* xl.line has just begun */
void antic_build_pf(void);              /* the line's pixels into pf (GTIA, before drawing) */
#define ANTIC_KEY_BLANK     0x8000000Bu /* a line of background only */
void antic_check_nmi(void);             /* at cycle 8 of a line with a DLI or VBI */
uint8_t antic_read(uint8_t reg);
void antic_write(uint8_t reg, uint8_t v);
uint8_t antic_mem(uint16_t addr);       /* a DMA read */

/* ---- gtia.c ---- */

typedef struct {
    uint8_t hposp[4], hposm[4], sizep[4], sizem;
    uint8_t grafp[4], grafm;
    uint8_t col[9];                     /* COLPM0-3, COLPF0-3, COLBK */
    uint8_t prior, vdelay, gractl, consol_out;
    uint8_t mpf[4], ppf[4], mpl[4], ppl[4];     /* collisions */
    uint16_t sel[256];                  /* priority: colours selected for (players | PF mask << 4) */
    int drawn;                          /* colour clocks of this line drawn (CC_FIRST ...) */
    bool objects;                       /* players or missiles may show on this line */
    uint8_t *row;                       /* the picture's row being drawn */
    uint8_t trig[4];                    /* trigger inputs: 1 up, 0 pressed */
    uint8_t trig_latch[4];
    volatile uint8_t consol_in;         /* console keys held: bit 0 Start, 1 Select, 2 Option */
    bool changed;                       /* a line was drawn this frame */
    uint32_t signature;
} gtia_t;

extern gtia_t gtia;

void gtia_reset(void);
void gtia_begin_line(void);
void gtia_end_line(void);               /* draws the rest and keeps the line */
void gtia_pm_dma(uint8_t missiles, const uint8_t players[4], bool do_missiles, bool do_players);
uint8_t gtia_read(uint8_t reg);
void gtia_write(uint8_t reg, uint8_t v);
void gtia_frame_end(void);
void gtia_forget_lines(void);           /* draw every line again */
void gtia_triggers(void);               /* latched triggers follow the inputs */
void palette_build(xl_tv_t tv, uint16_t *rgb565);

/* ---- pokey.c ---- */

void pokey_reset(void);
uint8_t pokey_read(uint8_t reg);
void pokey_write(uint8_t reg, uint8_t v);
uint64_t pokey_next_event(void);
void pokey_events(uint64_t now);
void pokey_frame(void);                 /* keyboard events from other tasks */
/* A byte from a device arrives at POKEY's serial input (its stop bit) */
void pokey_serial_in(uint8_t byte);
bool pokey_irq(void);
void pokey_audio_reset(void);
void pokey_audio_time(uint64_t now);    /* sound may be rendered up to now */
void pokey_consol_speaker(bool on);     /* GTIA CONSOL bit 3 */

/* ---- sio.c: the SIO bus and the disk drives ---- */

void sio_reset(void);
void sio_command_line(bool asserted);   /* PIA CB2 (asserted = low) */
void sio_from_computer(uint8_t byte);   /* POKEY sent a byte (its stop bit, now) */
uint64_t sio_next_event(void);
void sio_events(uint64_t now);
/* The OS's SIO routine was called: serve it if it's for a drive; true if done */
bool sio_patch(m6502_t *cpu);
int sio_busy_drive(void);

/* ---- disk.c: ATR/XFD images ---- */

typedef struct {
    char path[192];
    uint8_t *data;                      /* the sectors (no header) */
    uint32_t size;                      /* bytes of data */
    uint32_t capacity;                  /* bytes allocated */
    uint16_t sector_size;               /* 128 or 256 */
    uint32_t sectors;
    uint16_t boot_stride;               /* sectors 1-3 (128 bytes each) are this far apart */
    uint32_t data_start;                /* sector 4 begins here (384, or 768 in a padded image) */
    bool atr;
    bool read_only, dirty;
    uint32_t reads, writes;
    uint16_t last_sector;
} disk_t;

extern disk_t xl_disks[XL_DRIVES];

bool disk_load(disk_t *d, const char *path);
void disk_unload(disk_t *d);
/* A sector's bytes (1-based number), NULL if out of range */
uint8_t *disk_sector(disk_t *d, uint32_t sector, int *len);
/* Format to this geometry (data all zeros) */
bool disk_format(disk_t *d, uint16_t sector_size, uint32_t sectors);
bool disk_flush(disk_t *d);
