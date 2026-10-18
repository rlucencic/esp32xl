/*
 * The Atari 800XL/130XE: memory, the PIA, the bus, and the frame loop.
 *
 * The CPU runs a line at a time (114 cycles), stopping at cycle 8 of lines
 * with a DLI or VBI to raise the NMI and at POKEY and SIO events. ANTIC's
 * DMA cycles stall it: a read or write at or after cpu.stall_from calls
 * stall(), which moves the clock past the line's busy cycles (and, for
 * reads, to the end of a WSYNC wait). An instruction that runs over the
 * end of a line begins the next line from there.
 */
#include <stdio.h>
#include <string.h>
#include "emudbg.h"
#include "xl_internal.h"

xl_t xl;

static uint8_t s_junk[256];             /* writes to ROM */
static uint8_t s_ff[256];               /* ANTIC reading I/O */
static uint16_t s_palette[256];

/* ---- Memory ---- */

static void update_map(void)
{
    uint8_t pb = xl.portb;
    uint8_t *ram = xl.cfg.ram;
    const uint8_t *os = xl.cfg.os;
    bool os_on = pb & 0x01;
    bool basic = !(pb & 0x02) && xl.cfg.basic_rom;
    bool selftest = !(pb & 0x80) && os_on;
    uint8_t *cpu_bank = ram + 0x4000, *antic_bank = ram + 0x4000;
    if (xl.ext) {
        uint8_t *bank = xl.ext + ((pb >> 2) & 3) * 0x4000;
        if (!(pb & 0x10)) {
            cpu_bank = bank;
        }
        if (!(pb & 0x20)) {
            antic_bank = bank;
        }
    }
    m6502_t *c = &xl.cpu;
    for (int p = 0; p < 256; p++) {
        uint8_t *r = ram + p * 256, *w = r;
        const uint8_t *a = r;
        if (p >= 0x40 && p < 0x80) {
            r = w = cpu_bank + (p - 0x40) * 256;
            a = antic_bank + (p - 0x40) * 256;
        }
        if (selftest && p >= 0x50 && p < 0x58) {
            r = (uint8_t *)os + 0x1000 + (p - 0x50) * 256;
            w = s_junk;
            a = r;
        } else if (basic && p >= 0xA0 && p < 0xC0) {
            r = (uint8_t *)xl.cfg.basic_rom + (p - 0xA0) * 256;
            w = s_junk;
            a = r;
        } else if (os_on && ((p >= 0xC0 && p < 0xD0) || p >= 0xD8)) {
            r = (uint8_t *)os + (p - 0xC0) * 256;
            w = s_junk;
            a = r;
        }
        if (p >= 0xD0 && p < 0xD8) {
            r = w = NULL;                   /* I/O */
            a = s_ff;
        }
        c->rd[p] = r;
        c->wr[p] = w;
        xl.antic_rd[p] = a;
    }
}

/* Port B as the memory control sees it: inputs pulled up */
static void portb_changed(void)
{
    uint8_t pb = (xl.portb_out & xl.portb_ddr) | ~xl.portb_ddr;
    if (pb != xl.portb) {
        xl.portb = pb;
        update_map();
    }
}

uint8_t xl_peek(uint16_t a)
{
    return m6502_peek(&xl.cpu, a);
}

/* ---- PIA ---- */

static uint8_t s_joy[2];                /* XL_JOY_* bits of ports 1 and 2 */

static uint8_t pia_read(uint8_t reg)
{
    switch (reg & 3) {
    case 0:
        if (xl.pactl & 4) {
            uint8_t in = ~((s_joy[0] & 0x0F) | (s_joy[1] & 0x0F) << 4);
            return (xl.porta_out | ~xl.porta_ddr) & in;
        }
        return xl.porta_ddr;
    case 1:
        return (xl.pbctl & 4) ? xl.portb : xl.portb_ddr;
    case 2:
        return xl.pactl & 0x3F;
    default:
        return xl.pbctl & 0x3F;
    }
}

static void pia_write(uint8_t reg, uint8_t v)
{
    switch (reg & 3) {
    case 0:
        if (xl.pactl & 4) {
            xl.porta_out = v;
        } else {
            xl.porta_ddr = v;
        }
        break;
    case 1:
        if (xl.pbctl & 4) {
            xl.portb_out = v;
        } else {
            xl.portb_ddr = v;
        }
        portb_changed();
        break;
    case 2:
        xl.pactl = v & 0x3F;
        break;
    default:
        xl.pbctl = v & 0x3F;
        /* CB2 is the SIO command line: output low = asserted */
        sio_command_line((v & 0x38) == 0x30);
        break;
    }
}

static void pia_reset(void)
{
    xl.porta_out = xl.porta_ddr = xl.pactl = 0;
    xl.portb_out = xl.portb_ddr = xl.pbctl = 0;
    xl.portb = 0;
    portb_changed();
    sio_command_line(false);
}

void xl_joystick(int port, uint8_t bits)
{
    if (port >= 1 && port <= 2) {
        s_joy[port - 1] = bits;
        gtia.trig[port - 1] = !(bits & XL_JOY_FIRE);
    }
}

/* ---- The bus ---- */

void xl_update_irq(void)
{
    if (pokey_irq()) {
        xl.cpu.irq |= IRQ_POKEY;
    } else {
        xl.cpu.irq &= ~IRQ_POKEY;
    }
}

static uint8_t XL_HOT bus_read(void *ctx, uint16_t a)
{
    switch (a >> 8) {
    case 0xD0: return gtia_read(a);
    case 0xD2: return pokey_read(a);
    case 0xD3: return pia_read(a);
    case 0xD4: return antic_read(a);
    default: return 0xFF;                   /* PBI, cartridge control, nothing */
    }
}

static void XL_HOT bus_write(void *ctx, uint16_t a, uint8_t v)
{
    switch (a >> 8) {
    case 0xD0: gtia_write(a, v); break;
    case 0xD2: pokey_write(a, v); break;
    case 0xD3: pia_write(a, v); break;
    case 0xD4: antic_write(a, v); break;
    default: break;
    }
}

/* ---- Lines and DMA ---- */

static void begin_line(void)
{
    antic_begin_line();
    gtia_begin_line();
    /* the CPU skips this line's DMA cycles by itself */
    m6502_t *c = &xl.cpu;
    c->dma_free = antic.next_free;
    c->dma_busy = antic.next_busy;
    c->dma_base = xl.line_start;
    c->dma_len = XL_LINE_CYCLES;
}

/* End the current line and begin the next */
static void next_line(void)
{
    gtia_end_line();
    pokey_audio_time(xl.line_start + XL_LINE_CYCLES);
    xl.line_start += XL_LINE_CYCLES;
    if (++xl.line >= xl.lines) {
        xl.line = 0;
        xl.frames++;
        gtia_frame_end();
    }
    begin_line();
}

/* The first cycle from t on that DMA or WSYNC may stall (for stall_from) */
static uint64_t stall_point(uint64_t t)
{
    uint64_t c = t - xl.line_start;
    uint64_t s = xl.line_start + (c < XL_LINE_CYCLES ? antic.next_busy[c] : XL_LINE_CYCLES);
    if (s < t) {
        s = t;
    }
    if (xl.wsync_to > t) {
        uint64_t w = xl.wsync_from > t ? xl.wsync_from : t;
        if (w < s) {
            s = w;
        }
    }
    return s;
}

static void XL_HOT stall(void *ctx, m6502_t *cpu)
{
    uint32_t c = (uint32_t)(cpu->cycles - xl.line_start);
    for (;;) {
        if (c >= XL_LINE_CYCLES) {
            cpu->cycles = xl.line_start + c;
            next_line();
            c = (uint32_t)(cpu->cycles - xl.line_start);
            continue;
        }
        uint32_t f = antic.next_free[c];    /* (114: the next line) */
        if (f != c) {
            c = f;
            continue;
        }
        if (xl.wsync_to) {
            uint64_t t = xl.line_start + c;
            if (t >= xl.wsync_to) {
                xl.wsync_from = xl.wsync_to = 0;
                cpu->dma_stop = UINT64_MAX;
            } else if (!cpu->stall_is_write && t >= xl.wsync_from) {
                c = (uint32_t)(xl.wsync_to - xl.line_start);    /* WSYNC holds reads (RDY) only */
                continue;
            }
        }
        break;
    }
    cpu->cycles = xl.line_start + c;
    uint64_t next = xl.wsync_to ? stall_point(cpu->cycles + 1)
                                : xl.line_start + (c + 1 < XL_LINE_CYCLES ? antic.next_busy[c + 1] : XL_LINE_CYCLES);
    if (next != cpu->stall_from) {
        m6502_set_stall_from(cpu, next);
    }
}

/* A write to WSYNC (now): the next cycle runs, reads after it wait for
 * cycle 105 (of the next line if written at 104 or later)
 */
void xl_wsync(void)
{
    uint64_t w = xl_now();
    int c = (int)(w - xl.line_start);
    uint64_t target = xl.line_start + (c <= 103 ? 105 : XL_LINE_CYCLES + 105);
    if (!(xl.wsync_to > w && xl.wsync_to == target)) {
        xl.wsync_from = w + 2;
        xl.wsync_to = target;
    }
    xl.cpu.dma_stop = xl.wsync_from;           /* reads from then on: stall() */
    if (xl.cpu.stall_from > w + 1) {
        m6502_set_stall_from(&xl.cpu, w + 1);
    }
}

/* ---- Running ---- */

static void run_cpu(uint64_t until)
{
    m6502_t *c = &xl.cpu;
    c->until = until;
    c->stall_from = stall_point(c->cycles);
    m6502_run(c);
    if (c->pc == xl.sio_entry && xl.sio_patch) {
        sio_patch(c);
    }
}

static volatile bool s_warm_reset;
static void warm_reset_now(void);

void XL_HOT xl_run_frame(void)
{
    if (s_warm_reset) {
        s_warm_reset = false;
        warm_reset_now();
    }
    pokey_frame();
    gtia_triggers();
    if (xl.option_frames > 0) {
        gtia.consol_in |= 0x04;             /* OPTION: BASIC off */
        if (--xl.option_frames == 0) {
            gtia.consol_in &= ~0x04;
        }
    }
    uint32_t frame = xl.frames;
    while (xl.frames == frame) {
        uint64_t end = xl.line_start + XL_LINE_CYCLES;
        uint64_t until = end;
        if (antic.nmi && !antic.nmi_checked) {
            until = xl.line_start + 8;
        }
        uint64_t ev = pokey_next_event();
        uint64_t sv = sio_next_event();
        if (sv < ev) {
            ev = sv;
        }
        if (ev < until) {
            until = ev;
        }
        if (xl.cpu.cycles < until) {
            run_cpu(until);
        }
        uint64_t now = xl.cpu.cycles;
        if (antic.nmi && !antic.nmi_checked && now >= xl.line_start + 8) {
            antic_check_nmi();
        }
        pokey_events(now);
        sio_events(now);
        if (now >= xl.line_start + XL_LINE_CYCLES) {
            next_line();
        }
    }
}

/* ---- Reset ---- */

static void cpu_reset(void)
{
    xl.wsync_from = xl.wsync_to = 0;
    xl.cpu.dma_stop = UINT64_MAX;
    m6502_reset(&xl.cpu);
}

void xl_reset(void)
{
    memset(xl.cfg.ram, 0, xl.cfg.model == XL_MODEL_130XE ? 128 * 1024 : 64 * 1024);
    antic_reset();
    gtia_reset();
    pokey_reset();
    sio_reset();
    pia_reset();
    xl.option_frames = xl.cfg.basic ? 0 : 150;
    cpu_reset();
}

void xl_warm_reset(void)
{
    s_warm_reset = true;
}

static void warm_reset_now(void)
{
    antic_reset();
    pia_reset();
    cpu_reset();
}

/* ---- The debugger (emudbg.h) ---- */

static emudbg_cpu_t s_dbg_cpus[] = {
    { .name = "6502", .arch = &emudbg_arch_6502, .core = &xl.cpu },
};

static void dbg_where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %lu, line %d, cycle %d", (unsigned long)xl.frames, xl.line, xl_line_cycle());
}

static void dbg_screen(int *width, int *height)
{
    *width = XL_SCREEN_WIDTH;
    *height = XL_SCREEN_HEIGHT;
}

static emudbg_machine_t s_dbg = {
    .cpus = s_dbg_cpus,
    .cpu_count = 1,
    .where = dbg_where,
    .reset = xl_reset,
    .screen = dbg_screen,
    .render_line = xl_render_line_rgb565,
    .type = xl_type,
};

bool xl_init(const xl_config_t *config)
{
    xl.cfg = *config;
    if (!xl.cfg.ram || !xl.cfg.os || !xl.cfg.frame) {
        return false;
    }
    memset(s_ff, 0xFF, sizeof(s_ff));
    bool pal = xl.cfg.tv == XL_TV_PAL;
    xl.lines = pal ? 312 : 262;
    xl.clock_hz = pal ? 1773447 : 1789773;
    xl.ext = xl.cfg.model == XL_MODEL_130XE ? xl.cfg.ram + 65536 : NULL;
    xl.sio_patch = xl.cfg.sio_patch;
    /* SIOV ($E459) jumps to the SIO routine: the patch stops there */
    const uint8_t *siov = xl.cfg.os + 0x2459;
    xl.sio_entry = siov[0] == 0x4C ? (siov[1] | siov[2] << 8) : 0xE459;
    palette_build(xl.cfg.tv, s_palette);
    memset(xl.cfg.frame, 0, XL_SCREEN_WIDTH * XL_SCREEN_HEIGHT);

    static const m6502_bus_t bus = { .read = bus_read, .write = bus_write, .stall = stall };
    m6502_init(&xl.cpu, &bus);
    xl.cpu.stall_writes = true;             /* ANTIC's HALT stops writes too */
    xl.cpu.stop_pc = xl.sio_entry;
    xl.line_start = 0;
    xl.line = 0;
    xl.frames = 0;
    xl.portb = 0;
    xl_reset();
    begin_line();
    s_dbg.name = xl.cfg.model == XL_MODEL_130XE ? "Atari 130XE" : "Atari 800XL";
    emudbg_set_machine(&s_dbg);
    XL_LOG("%s %s, BASIC %s, SIO $%04X", xl.cfg.model == XL_MODEL_130XE ? "130XE" : "800XL",
           pal ? "PAL" : "NTSC", xl.cfg.basic && xl.cfg.basic_rom ? "on" : "off", xl.sio_entry);
    return true;
}

void xl_set_sio_patch(bool on)
{
    xl.sio_patch = on;
}

void xl_get_status(xl_status_t *s)
{
    s->frames = xl.frames;
    s->pc = xl.cpu.pc;
    s->portb = xl.portb;
    s->dmactl = antic.dmactl;
    s->nmien = antic.nmien;
    s->dlist = antic.dlist;
    s->sio_led = sio_busy_drive();
}

/* ---- Screen ---- */

int xl_screen_hz(void)
{
    return xl.cfg.tv == XL_TV_PAL ? 50 : 60;
}

const uint16_t *xl_palette(void)
{
    return s_palette;
}

void xl_render_line_rgb565(int y, uint16_t *dst)
{
    if (y < 0 || y >= XL_SCREEN_HEIGHT) {
        return;
    }
    const uint8_t *src = xl.cfg.frame + y * XL_SCREEN_WIDTH;
    for (int x = 0; x < XL_SCREEN_WIDTH; x++) {
        dst[x] = s_palette[src[x]];
    }
}

uint32_t xl_screen_signature(void)
{
    return gtia.signature;
}

/* ---- Disks ---- */

bool xl_disk_insert(int drive, const char *path)
{
    return drive >= 0 && drive < XL_DRIVES && disk_load(&xl_disks[drive], path);
}

void xl_disk_eject(int drive)
{
    if (drive >= 0 && drive < XL_DRIVES) {
        disk_flush(&xl_disks[drive]);
        disk_unload(&xl_disks[drive]);
    }
}

const char *xl_disk_path(int drive)
{
    return drive >= 0 && drive < XL_DRIVES && xl_disks[drive].data ? xl_disks[drive].path : NULL;
}

bool xl_disk_write_protected(int drive)
{
    return drive >= 0 && drive < XL_DRIVES && xl_disks[drive].read_only;
}

bool xl_disk_dirty(void)
{
    for (int d = 0; d < XL_DRIVES; d++) {
        if (xl_disks[d].dirty) {
            return true;
        }
    }
    return false;
}

void xl_disk_flush(void)
{
    for (int d = 0; d < XL_DRIVES; d++) {
        if (!disk_flush(&xl_disks[d])) {
            XL_LOG("can't write %s", xl_disks[d].path);
        }
    }
}

void xl_disk_status(xl_disk_status_t *s)
{
    s->light = sio_busy_drive();
    for (int d = 0; d < XL_DRIVES; d++) {
        s->reads[d] = xl_disks[d].reads;
        s->writes[d] = xl_disks[d].writes;
        s->sector[d] = xl_disks[d].last_sector;
    }
}
