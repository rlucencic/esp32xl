/*
 * ANTIC: the display list, playfield and player/missile DMA, NMIs.
 *
 * Each scan line is set up as it begins (antic_begin_line()): the display
 * list instruction on a mode line's first line, the rows of its vertical
 * scroll, the line's playfield as GTIA sees it (one code per hires pixel),
 * P/M data for GTIA, and which of the line's 114 cycles DMA takes from the
 * CPU, after the Altirra Hardware Reference Manual: missiles at 0, the
 * instruction at 1, players 2-5, LMS/jump addresses 6-7, character names
 * from 26/18/10 (narrow/normal/wide) and their data three cycles after,
 * bitmap data from 28/20/12, one fetch per 2, 4 or 8 cycles, HSCROL/2
 * cycles later when scrolled, none from cycle 106 on; nine refresh cycles
 * from 25, every 4, a blocked one taken at the next free cycle.
 *
 * A line's DMA cycles depend on a handful of things (P/M DMA, the
 * instruction and address fetches, the mode's fetch rate, fetch width,
 * scroll delay, first line or not): each combination is worked out once,
 * with tables of the next busy and the next free cycle for the stalls.
 */
#include <string.h>
#include "xl_internal.h"

antic_t antic;

static const uint8_t mode_height[16] = { 1, 1, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1 };

/* Playfield start (colour clock) of each fetch width, and the window shown
 * for each width: narrow, normal, wide (clipped by ANTIC to start at $2C)
 */
static const int pf_left[4] = { 0, 64, 48, 32 };
static const int win_left[4] = { 0, 64, 48, 44 };
static const int win_right[4] = { 0, 192, 208, 224 };

/* ---- DMA patterns ---- */

/* Fetch classes: what a mode fetches, how often */
enum { CLASS_NONE, CLASS_CHAR2, CLASS_CHAR4, CLASS_MAP8, CLASS_MAP4, CLASS_MAP2 };

typedef struct {
    uint32_t key;
    uint8_t busy[XL_LINE_CYCLES + 1];
    uint8_t next_busy[XL_LINE_CYCLES + 1];
    uint8_t next_free[XL_LINE_CYCLES + 1];
} pattern_t;

#define PATTERNS    64

static pattern_t *s_patterns;

static void build_pattern(pattern_t *p, uint32_t key)
{
    int pm = key & 3, dl = (key >> 2) & 1, addr = (key >> 3) & 1, cls = (key >> 4) & 7;
    int fw = (key >> 7) & 3, shift = (key >> 9) & 7, first = (key >> 12) & 1;
    uint8_t *busy = p->busy;
    memset(busy, 0, sizeof(p->busy));
    if (pm) {
        busy[0] = 1;
    }
    if (pm == 2) {
        busy[2] = busy[3] = busy[4] = busy[5] = 1;
    }
    busy[1] = dl;
    if (addr) {
        busy[6] = busy[7] = 1;
    }
    if (cls != CLASS_NONE && fw) {
        int n, interval, s;
        bool chars = cls == CLASS_CHAR2 || cls == CLASS_CHAR4;
        switch (cls) {
        case CLASS_CHAR2: n = (int[]){ 0, 32, 40, 48 }[fw]; interval = 2; break;
        case CLASS_CHAR4: n = (int[]){ 0, 16, 20, 24 }[fw]; interval = 4; break;
        case CLASS_MAP8: n = (int[]){ 0, 8, 10, 12 }[fw]; interval = 8; break;
        case CLASS_MAP4: n = (int[]){ 0, 16, 20, 24 }[fw]; interval = 4; break;
        default: n = (int[]){ 0, 32, 40, 48 }[fw]; interval = 2; break;
        }
        s = (chars ? (int[]){ 0, 26, 18, 10 }[fw] : (int[]){ 0, 28, 20, 12 }[fw]) + shift;
        for (int k = 0; k < n; k++) {
            int c = s + k * interval;
            if (first && c < 106) {
                busy[c] = 1;            /* names or bitmap */
            }
            if (chars && c + 3 < 106) {
                busy[c + 3] = 1;        /* character data */
            }
        }
    }
    /* refresh: requests at 25, 29, ... 57; a blocked one at the next free cycle */
    bool pending = false;
    for (int c = 25; c < XL_LINE_CYCLES; c++) {
        bool request = c <= 57 && ((c - 25) & 3) == 0;
        if (busy[c]) {
            pending |= request;
            continue;
        }
        if (request || pending) {
            busy[c] = 1;
            pending = request && pending;
        }
    }
    busy[XL_LINE_CYCLES] = 0;
    p->next_busy[XL_LINE_CYCLES] = XL_LINE_CYCLES;
    p->next_free[XL_LINE_CYCLES] = XL_LINE_CYCLES;
    for (int c = XL_LINE_CYCLES - 1; c >= 0; c--) {
        p->next_busy[c] = busy[c] ? c : p->next_busy[c + 1];
        p->next_free[c] = busy[c] ? p->next_free[c + 1] : c;
    }
    p->key = key;
}

static void use_pattern(uint32_t key)
{
    key |= 1u << 31;
    pattern_t *p = &s_patterns[(key * 2654435761u) >> 26];
    if (p->key != key) {
        build_pattern(p, key);
    }
    antic.busy = p->busy;
    antic.next_busy = p->next_busy;
    antic.next_free = p->next_free;
}

/* ---- Memory ---- */

void antic_reset(void)
{
    if (s_patterns == NULL) {
        s_patterns = xl_calloc(PATTERNS, sizeof(pattern_t));
    }
    antic.nmien = 0;
    antic.dmactl = 0;
    antic.nmist = 0x1F;
    antic.new_mode_line = true;
    antic.jvb = false;
    antic.pf_blank = false;
    use_pattern(0);
}

uint8_t XL_HOT antic_mem(uint16_t addr)
{
    return xl.antic_rd[addr >> 8][addr & 0xFF];
}

static inline uint16_t inc10(uint16_t a)
{
    return (a & 0xFC00) | ((a + 1) & 0x03FF);
}

static inline uint16_t inc12(uint16_t a)
{
    return (a & 0xF000) | ((a + 1) & 0x0FFF);
}

/* ---- The playfield as GTIA sees it ----
 *
 * Built a colour clock (two hires pixels, one uint16_t) at a time into
 * s_wide, which covers every colour clock a fetch can reach, then the
 * window of the playfield width is copied into antic.pf.
 */
#define WIDE_CC     288

static uint16_t s_wide[WIDE_CC + 16];

static inline uint16_t dup(uint8_t code)
{
    return code | code << 8;
}

/* Two hires pixels from a bit pair (the left one is the high bit) */
static const uint16_t hires_pair[4] = {
    PF_2 | PF_2 << 8, PF_2 | PF_HIRES_ON << 8, PF_HIRES_ON | PF_2 << 8, PF_HIRES_ON | PF_HIRES_ON << 8,
};
static const uint16_t lores2[4] = {
    PF_BAK | PF_BAK << 8, PF_0 | PF_0 << 8, PF_1 | PF_1 << 8, PF_2 | PF_2 << 8,
};

/* A character mode's row, the same for every character of the line */
typedef struct {
    uint16_t base;                      /* character set + row */
    uint8_t mask;                       /* name bits indexing the set */
    bool blank, blank_desc;             /* rows of no data: characters $00-5F, $60-7F */
    bool attr;                          /* name bit 7: inverse / blink (modes 2-3) */
} char_row_t;

static void char_row(int mode, char_row_t *cr)
{
    int r = antic.row;
    cr->blank = cr->blank_desc = false;
    cr->attr = mode <= 3;
    switch (mode) {
    case 2:
    case 3:
        if (r >= 10) {
            r -= 8;                         /* rows 10-15 repeat 2-7 */
        }
        cr->blank = r >= 8;                 /* rows 8-9 of $00-5F, $80-DF */
        cr->blank_desc = mode == 3 && r < 2;    /* descenders: rows 0-1 at the bottom */
        r &= 7;
        break;
    case 4: case 6: r &= 7; break;
    default: r = (r >> 1) & 7; break;       /* 5, 7: double height */
    }
    if (antic.chactl & 4) {
        r = 7 - r;                          /* vertical reflection */
    }
    bool half = mode >= 6;                  /* 64 characters in 512 bytes */
    cr->base = ((antic.chbase & (half ? 0xFE : 0xFC)) << 8) + r;
    cr->mask = half ? 0x3F : 0x7F;
}

static inline uint8_t char_data(const char_row_t *cr, uint8_t name)
{
    bool blank = (name & 0x60) == 0x60 ? cr->blank_desc : cr->blank;
    uint8_t data = blank ? 0 : antic_mem(cr->base + (name & cr->mask) * 8);
    if (cr->attr && (name & 0x80)) {
        if (antic.chactl & 1) {
            data = 0;                       /* blink */
        }
        if (antic.chactl & 2) {
            data ^= 0xFF;                   /* inverse */
        }
    }
    return data;
}

static inline void hires_byte(uint16_t *w, uint8_t d)
{
    w[0] = hires_pair[d >> 6];
    w[1] = hires_pair[(d >> 4) & 3];
    w[2] = hires_pair[(d >> 2) & 3];
    w[3] = hires_pair[d & 3];
}

/* 4 pixels of 2 bits, each n colour clocks */
static inline uint16_t *lores2_byte(uint16_t *w, uint8_t d, int n)
{
    for (int p = 6; p >= 0; p -= 2) {
        uint16_t v = lores2[(d >> p) & 3];
        for (int i = 0; i < n; i++) {
            *w++ = v;
        }
    }
    return w;
}

/* 8 pixels of 1 bit, each n colour clocks */
static inline uint16_t *lores1_byte(uint16_t *w, uint8_t d, int n, uint16_t on)
{
    for (int b = 7; b >= 0; b--) {
        uint16_t v = (d >> b) & 1 ? on : 0;
        for (int i = 0; i < n; i++) {
            *w++ = v;
        }
    }
    return w;
}

/* This line's playfield as data, fetched as the line begins; the pixels
 * are made from it only when GTIA draws (antic_build_pf())
 */
static struct {
    int mode, fw, width, hs, n;
    uint8_t data[48];                   /* character data bytes, or the bitmap */
} s_line;

static inline uint32_t fnv(uint32_t h, uint8_t b)
{
    return (h ^ b) * 16777619u;
}

/* Fetch the playfield's data and make the line's key from what decides its
 * pixels
 */
static void fetch_playfield(int mode, int fw, int width, int hs)
{
    int n;
    if (mode <= 5) {
        n = (int[]){ 0, 32, 40, 48 }[fw];
    } else if (mode <= 7) {
        n = (int[]){ 0, 16, 20, 24 }[fw];
    } else if (mode <= 9) {
        n = (int[]){ 0, 8, 10, 12 }[fw];
    } else if (mode <= 0xC) {
        n = (int[]){ 0, 16, 20, 24 }[fw];
    } else {
        n = (int[]){ 0, 32, 40, 48 }[fw];
    }
    if (antic.first) {
        for (int k = 0; k < n; k++) {
            antic.buf[k] = antic_mem(antic.msc);
            antic.msc = inc12(antic.msc);
        }
    }
    s_line.mode = mode;
    s_line.fw = fw;
    s_line.width = width;
    s_line.hs = hs;
    s_line.n = n;
    uint32_t h = 2166136261u;
    h = fnv(h, mode);
    h = fnv(h, fw << 4 | width);
    h = fnv(h, hs);
    if (mode <= 7) {
        char_row_t cr;
        char_row(mode, &cr);
        /* modes 4-7 take colours from the names too */
        uint8_t colour_bits = mode <= 3 ? 0 : mode <= 5 ? 0x80 : 0xC0;
        for (int k = 0; k < n; k++) {
            uint8_t name = antic.buf[k];
            uint8_t d = char_data(&cr, name);
            s_line.data[k] = d;
            h = fnv(fnv(h, d), name & colour_bits);
        }
    } else {
        for (int k = 0; k < n; k++) {
            uint8_t d = antic.buf[k];
            s_line.data[k] = d;
            h = fnv(h, d);
        }
    }
    antic.key = h ? h : 1;
}

void antic_build_pf(void)
{
    antic.pf_built = true;
    if (antic.pf_blank) {
        return;
    }
    if (antic.key == ANTIC_KEY_BLANK) {
        memset(antic.pf, PF_BAK, sizeof(antic.pf));
        antic.pf_blank = true;
        return;
    }
    int mode = s_line.mode, fw = s_line.fw, width = s_line.width, n = s_line.n;
    int origin = pf_left[fw] + s_line.hs;   /* colour clock of the first byte */
    uint16_t *w = s_wide + origin;
    const uint8_t *data = s_line.data;
    for (int k = 0; k < n; k++) {
        uint8_t d = data[k];
        switch (mode) {
        case 2:
        case 3:
        case 0xF:
            hires_byte(w, d);
            w += 4;
            break;
        case 4:
        case 5: {
            uint8_t name = antic.buf[k];
            for (int p = 6; p >= 0; p -= 2) {
                int v = (d >> p) & 3;
                *w++ = v == 3 && (name & 0x80) ? dup(PF_3) : lores2[v];
            }
            break;
        }
        case 6:
        case 7:
            w = lores1_byte(w, d, 1, dup(PF_0 + (antic.buf[k] >> 6)));
            break;
        case 8: w = lores2_byte(w, d, 4); break;
        case 9: w = lores1_byte(w, d, 2, dup(PF_0)); break;
        case 0xA: w = lores2_byte(w, d, 2); break;
        case 0xB:
        case 0xC: w = lores1_byte(w, d, 1, dup(PF_0)); break;
        default: w = lores2_byte(w, d, 1); break;   /* D, E */
        }
    }

    /* the window of the playfield width, background around it */
    uint16_t *pf = (uint16_t *)antic.pf;
    int lo = win_left[width], hi = win_right[width];
    int a = origin > lo ? origin : lo;
    int b = (int)(w - s_wide) < hi ? (int)(w - s_wide) : hi;
    if (a >= b) {
        memset(pf, 0, sizeof(antic.pf));
        return;
    }
    memset(pf, 0, (a - CC_FIRST) * sizeof(uint16_t));
    memcpy(pf + (a - CC_FIRST), s_wide + a, (b - a) * sizeof(uint16_t));
    memset(pf + (b - CC_FIRST), 0, (CC_END - b) * sizeof(uint16_t));
}

/* ---- Lines ---- */

/* Player/missile DMA for this line, to GTIA */
static int pm_dma(int line)
{
    bool missiles = antic.dmactl & 0x0C, players = antic.dmactl & 0x08;
    if (!missiles) {
        return 0;
    }
    uint8_t m, p[4] = { 0 };
    if (antic.dmactl & 0x10) {              /* one-line resolution */
        uint16_t base = (antic.pmbase & 0xF8) << 8;
        m = antic_mem(base + 0x300 + line);
        if (players) {
            for (int i = 0; i < 4; i++) {
                p[i] = antic_mem(base + 0x400 + i * 0x100 + line);
            }
        }
    } else {
        uint16_t base = (antic.pmbase & 0xFC) << 8;
        m = antic_mem(base + 0x180 + (line >> 1));
        if (players) {
            for (int i = 0; i < 4; i++) {
                p[i] = antic_mem(base + 0x200 + i * 0x80 + (line >> 1));
            }
        }
    }
    gtia_pm_dma(m, p, true, players);
    return players ? 2 : 1;
}

static int fetch_class(int mode)
{
    if (mode < 2) {
        return CLASS_NONE;
    }
    if (mode <= 5) {
        return CLASS_CHAR2;
    }
    if (mode <= 7) {
        return CLASS_CHAR4;
    }
    if (mode <= 9) {
        return CLASS_MAP8;
    }
    return mode <= 0xC ? CLASS_MAP4 : CLASS_MAP2;
}

void antic_begin_line(void)
{
    int line = xl.line;
    antic.nmi = 0;
    antic.nmi_checked = false;
    antic.display = line >= 8 && line < 248;
    antic.hires = false;

    if (line == 248) {
        /* vertical blank: the display list waits for line 8 */
        antic.nmi = 0x40;
        antic.jvb = false;
        antic.new_mode_line = true;
        antic.ir &= ~0x40;
    }
    if (!antic.display) {
        use_pattern(0);
        return;
    }

    int pm = pm_dma(line);
    bool dl_dma = (antic.dmactl & 0x20) && !antic.jvb;
    bool first = antic.new_mode_line;
    bool dl_fetch = false, addr_fetch = false;
    if (first) {
        antic.new_mode_line = false;
        if (dl_dma) {
            dl_fetch = true;
            antic.ir = antic_mem(antic.dlist);
            antic.dlist = inc10(antic.dlist);
        }
        int mode = antic.ir & 15;
        int height;
        if (mode == 1) {
            if (dl_dma) {                   /* jump (and wait for vertical blank) */
                addr_fetch = true;
                uint8_t lo = antic_mem(antic.dlist);
                uint8_t hi = antic_mem(inc10(antic.dlist));
                antic.dlist = lo | hi << 8;
                if (antic.ir & 0x40) {
                    antic.jvb = true;
                }
            }
            height = 1;
        } else if (mode == 0) {
            height = ((antic.ir >> 4) & 7) + 1;
        } else {
            if ((antic.ir & 0x40) && dl_dma) {     /* LMS */
                addr_fetch = true;
                uint8_t lo = antic_mem(antic.dlist);
                antic.dlist = inc10(antic.dlist);
                uint8_t hi = antic_mem(antic.dlist);
                antic.dlist = inc10(antic.dlist);
                antic.msc = lo | hi << 8;
            }
            height = mode_height[mode];
        }
        bool vs = mode >= 2 && (antic.ir & 0x20);
        antic.row = vs && !antic.vs_prev ? antic.vscrol & 15 : 0;
        antic.end_row = !vs && antic.vs_prev ? antic.vscrol & 15 : (height - 1) & 15;
        antic.vs_prev = vs;
    } else {
        antic.row = (antic.row + 1) & 15;
    }
    antic.first = first;
    if (antic.row == antic.end_row) {
        antic.new_mode_line = true;
        if (antic.ir & 0x80) {
            antic.nmi = 0x80;               /* DLI on the mode line's last line */
        }
    }

    int mode = antic.ir & 15;
    int width = antic.dmactl & 3;
    int cls = CLASS_NONE, fw = 0, shift = 0;
    if (mode >= 2 && width && !antic.jvb) {
        bool hscroll = antic.ir & 0x10;
        fw = width + (hscroll && width < 3);
        int hs = hscroll ? antic.hscrol & 15 : 0;
        shift = hs >> 1;
        cls = fetch_class(mode);
        antic.hires = mode == 2 || mode == 3 || mode == 15;
        fetch_playfield(mode, fw, width, hs);
        antic.pf_blank = false;
        antic.pf_built = false;
    } else {
        antic.key = ANTIC_KEY_BLANK;
        antic.pf_built = antic.pf_blank;
    }
    use_pattern(pm | dl_fetch << 2 | addr_fetch << 3 | cls << 4 | fw << 7 | shift << 9 |
                (first && cls != CLASS_NONE) << 12);
}

/* Cycle 8: DLI or VBI */
void antic_check_nmi(void)
{
    antic.nmi_checked = true;
    if (antic.nmi == 0x80) {
        antic.nmist = (antic.nmist | 0x80) & ~0x40;
    } else if (antic.nmi == 0x40) {
        antic.nmist = (antic.nmist | 0x40) & ~0x80;
    } else {
        return;
    }
    if (antic.nmien & antic.nmi) {
        m6502_nmi_at(&xl.cpu, xl.line_start + 8);
    }
}

/* VCOUNT shows the next line from cycle 111 (the last line's: one cycle of
 * lines / 2, then 0)
 */
static uint8_t vcount(void)
{
    int c = xl_line_cycle();
    int v = xl.line;
    if (c >= 111) {
        if (v == xl.lines - 1) {
            v = c == 111 ? xl.lines : 0;
        } else {
            v++;
        }
    }
    return v >> 1;
}

uint8_t antic_read(uint8_t reg)
{
    switch (reg & 15) {
    case 0x0B: return vcount();
    case 0x0C: return 0;                    /* PENH */
    case 0x0D: return 0xFF;                 /* PENV */
    case 0x0F: return antic.nmist | 0x1F;
    default: return 0xFF;
    }
}

void antic_write(uint8_t reg, uint8_t v)
{
    switch (reg & 15) {
    case 0x00: antic.dmactl = v & 0x3F; break;
    case 0x01: antic.chactl = v & 7; break;
    case 0x02: antic.dlist = (antic.dlist & 0xFF00) | v; break;
    case 0x03: antic.dlist = (antic.dlist & 0x00FF) | v << 8; break;
    case 0x04: antic.hscrol = v & 15; break;
    case 0x05: antic.vscrol = v & 15; break;
    case 0x07: antic.pmbase = v; break;
    case 0x09: antic.chbase = v; break;
    case 0x0A: xl_wsync(); break;
    case 0x0E: antic.nmien = v & 0xC0; break;
    case 0x0F: antic.nmist &= 0x1F; break;  /* NMIRES */
    default: break;
    }
}
