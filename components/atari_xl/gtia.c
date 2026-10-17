/*
 * GTIA: players and missiles, priority, collisions, colours, triggers and
 * console keys; the palette.
 *
 * A line is drawn from ANTIC's playfield codes as the beam passes: before a
 * register write, up to where the write takes effect (colours 1 colour
 * clock after the beam, PRIOR 2, graphics 3, positions and sizes 5; the
 * beam is at colour clock 2 * cycle - 2), the rest at the end of the line.
 * Priority is GTIA's own logic (Altirra Hardware Reference Manual 6.7),
 * kept as a table of the colours selected for each combination of players
 * and playfields; overlapping colours are ORed.
 */
#include <math.h>
#include <string.h>
#include "xl_internal.h"

gtia_t gtia;

static const uint8_t size_cc[4] = { 1, 2, 1, 4 };   /* colour clocks per bit */
static uint8_t s_pm[CC_END - CC_FIRST];             /* objects at each colour clock */

static void build_priority(void)
{
    uint8_t pri = gtia.prior;
    int multi = (pri >> 5) & 1;
    int PRI0 = pri & 1, PRI1 = (pri >> 1) & 1, PRI2 = (pri >> 2) & 1, PRI3 = (pri >> 3) & 1;
    int PRI01 = PRI0 | PRI1, PRI12 = PRI1 | PRI2, PRI23 = PRI2 | PRI3, PRI03 = PRI0 | PRI3;
    for (int i = 0; i < 256; i++) {
        int P0 = i & 1, P1 = (i >> 1) & 1, P2 = (i >> 2) & 1, P3 = (i >> 3) & 1;
        int PF0 = (i >> 4) & 1, PF1 = (i >> 5) & 1, PF2 = (i >> 6) & 1, PF3 = (i >> 7) & 1;
        int P01 = P0 | P1, P23 = P2 | P3, PF01 = PF0 | PF1, PF23 = PF2 | PF3;
        int SP0 = P0 & !(PF01 & PRI23) & !(PRI2 & PF23);
        int SP1 = P1 & !(PF01 & PRI23) & !(PRI2 & PF23) & ((!P0) | multi);
        int SP2 = P2 & !P01 & !(PF23 & PRI12) & !(PF01 & !PRI0);
        int SP3 = P3 & !P01 & !(PF23 & PRI12) & !(PF01 & !PRI0) & ((!P2) | multi);
        int SF3 = PF3 & !(P23 & PRI03) & !(P01 & !PRI2);
        int SF0 = PF0 & !(P23 & PRI0) & !(P01 & PRI01) & !SF3;
        int SF1 = PF1 & !(P23 & PRI0) & !(P01 & PRI01) & !SF3;
        int SF2 = PF2 & !(P23 & PRI03) & !(P01 & !PRI2) & !SF3;
        int SB = !P01 & !P23 & !PF01 & !PF23;
        gtia.sel[i] = SP0 | SP1 << 1 | SP2 << 2 | SP3 << 3 | SF0 << 4 | SF1 << 5 | SF2 << 6 | SF3 << 7 | SB << 8;
    }
}

/* The OR of the colours a priority selection picks: players (bits 0-3)
 * and playfields with the background (bits 4-8), tables made per drawing
 */
static uint8_t s_or_p[16], s_or_f[32];

static void or_tables(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t c = 0;
        for (int k = 0; k < 4; k++) {
            if (i & (1 << k)) {
                c |= gtia.col[k];
            }
        }
        s_or_p[i] = c;
    }
    for (int i = 0; i < 32; i++) {
        uint8_t c = 0;
        for (int k = 0; k < 5; k++) {
            if (i & (1 << k)) {
                c |= gtia.col[4 + k];
            }
        }
        s_or_f[i] = c;
    }
}

static inline uint8_t selected(uint16_t s)
{
    return s_or_p[s & 15] | s_or_f[s >> 4];
}

static void an_tables(void);

static void update_objects(void)
{
    gtia.objects = (gtia.grafp[0] | gtia.grafp[1] | gtia.grafp[2] | gtia.grafp[3] | gtia.grafm) != 0;
}

void gtia_reset(void)
{
    uint8_t trig[4];
    memcpy(trig, gtia.trig, sizeof(trig));
    uint8_t consol = gtia.consol_in;
    memset(&gtia, 0, sizeof(gtia));
    memcpy(gtia.trig, trig, sizeof(trig));
    gtia.consol_in = consol;
    for (int i = 0; i < 4; i++) {
        gtia.trig_latch[i] = 1;
    }
    if (!xl.frames) {
        gtia.trig[0] = gtia.trig[1] = 1;    /* (at power-on: joysticks still) */
    }
    gtia.trig[2] = 1;                       /* XL/XE: TRIG2 inactive, TRIG3 cartridge (none) */
    gtia.trig[3] = 0;
    build_priority();
    an_tables();
    gtia_forget_lines();
}

/* Objects of [a, b) (colour clocks) into s_pm: players bits 0-3, missiles 4-7 */
static void build_objects(int a, int b)
{
    memset(s_pm + a - CC_FIRST, 0, b - a);
    for (int i = 0; i < 4; i++) {
        uint8_t g = gtia.grafp[i];
        if (!g) {
            continue;
        }
        int h = gtia.hposp[i], w = size_cc[gtia.sizep[i] & 3];
        for (int k = 0; k < 8; k++) {
            if (!(g & (0x80 >> k))) {
                continue;
            }
            int s = h + k * w, e = s + w;
            s = s < a ? a : s;
            e = e > b ? b : e;
            for (int c = s; c < e; c++) {
                s_pm[c - CC_FIRST] |= 1 << i;
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        uint8_t g = (gtia.grafm >> (i * 2)) & 3;
        if (!g) {
            continue;
        }
        int h = gtia.hposm[i], w = size_cc[(gtia.sizem >> (i * 2)) & 3];
        for (int k = 0; k < 2; k++) {
            if (!(g & (2 >> k))) {
                continue;
            }
            int s = h + k * w, e = s + w;
            s = s < a ? a : s;
            e = e > b ? b : e;
            for (int c = s; c < e; c++) {
                s_pm[c - CC_FIRST] |= 0x10 << i;
            }
        }
    }
}

static inline void collide(uint8_t players, uint8_t missiles, uint8_t pf)
{
    for (int i = 0; i < 4; i++) {
        if (players & (1 << i)) {
            gtia.ppf[i] |= pf;
            gtia.ppl[i] |= players & ~(1 << i);
        }
        if (missiles & (1 << i)) {
            gtia.mpf[i] |= pf;
            gtia.mpl[i] |= players;
        }
    }
}

/* The 2-bit value of a colour clock's pair of playfield codes as on the ANx
 * bus (GTIA modes), indexed like the drawing's pairs: hires (each pixel a
 * bit) and lores (the colour's number)
 */
static uint8_t s_an_hires[64], s_an_lores[64];

static void an_tables(void)
{
    for (int c0 = 0; c0 < 8; c0++) {
        for (int c1 = 0; c1 < 8; c1++) {
            s_an_hires[c0 | c1 << 3] = (c0 == PF_HIRES_ON) << 1 | (c1 == PF_HIRES_ON);
            s_an_lores[c0 | c1 << 3] = c0 <= PF_0 ? 0 : (c0 - PF_0) & 3;
        }
    }
}

static inline int pair_index(uint16_t v)
{
    return (v & 7) | ((v >> 5) & 0x38);
}

/* The GTIA modes (PRIOR bits 6-7): 4-bit pixels two colour clocks wide */
static void draw_gtia_mode(int a, int b)
{
    int mode = gtia.prior >> 6;
    bool fifth = gtia.prior & 0x10;
    uint8_t *row = gtia.row;
    const uint16_t *pf16 = (const uint16_t *)antic.pf;
    const uint8_t *an = antic.hires ? s_an_hires : s_an_lores;
    if (!gtia.objects) {
        /* No players: a table of the 16 colours. 9: 16 luminances, 11: 16
         * hues, 10: P0-P3, PF0-PF3, background, PF0-PF3 (a colour clock late)
         */
        uint8_t ctab[16];
        uint8_t bk = gtia.col[8];
        for (int n = 0; n < 16; n++) {
            if (mode == 2) {
                ctab[n] = gtia.col[(n & 0x0C) == 0x08 ? 8 : n < 4 ? n : 4 + (n & 3)];
            } else {
                ctab[n] = mode == 1 ? (bk | n) : (n ? bk | n << 4 : bk & 0xF0);
            }
        }
        int late = mode == 2;
        uint16_t *dst = (uint16_t *)row - CC_FIRST;
        int cc = a;
        while (cc < b) {
            /* the pixel's own bits (an even pair), shown a clock late in mode 10 */
            int g = ((cc - late) & ~1) - CC_FIRST;
            uint8_t nib = an[pair_index(pf16[g])] << 2 | an[pair_index(pf16[g + 1])];
            uint16_t c = ctab[nib] | ctab[nib] << 8;
            dst[cc++] = c;
            if (cc < b && ((cc - late) & 1)) {
                dst[cc++] = c;                  /* the pixel's second colour clock */
            }
        }
        return;
    }
    or_tables();
    for (int cc = a; cc < b; cc++) {
        int x = (cc - CC_FIRST) * 2;
        int g = ((cc - (mode == 2)) & ~1) - CC_FIRST;      /* mode 10 shows a colour clock late */
        uint8_t nib = an[pair_index(pf16[g])] << 2 | an[pair_index(pf16[g + 1])];
        uint8_t m = gtia.objects ? s_pm[cc - CC_FIRST] : 0;
        uint8_t players = m & 15, missiles = m >> 4;
        uint8_t color;
        if (mode == 2) {
            /* 9 colours: P0-P3, PF0-PF3, BAK through the priority logic */
            uint8_t p = players, pf = 0;
            if (nib < 4) {
                p |= 1 << nib;
            } else if ((nib & 0x0C) == 0x08) {
                pf = 0;
            } else {
                pf = 1 << (nib & 3);
            }
            if (fifth) {
                pf |= missiles ? 8 : 0;
            } else {
                p |= missiles;
            }
            color = selected(gtia.sel[p | pf << 4]);
            if (m) {
                collide(players, missiles, nib >= 4 && (nib & 0x0C) != 0x08 ? 1 << (nib & 3) : 0);
            }
        } else {
            uint8_t pfc = mode == 1 ? (gtia.col[8] | nib) : (nib ? gtia.col[8] | nib << 4 : gtia.col[8] & 0xF0);
            uint8_t p = players | (fifth ? 0 : missiles);
            if (p) {
                color = selected(gtia.sel[p]);      /* players over the playfield */
            } else if (fifth && missiles) {
                color = gtia.col[7] | pfc;
            } else {
                color = pfc;
            }
            if (m) {
                collide(players, missiles, 0);
            }
        }
        row[x] = row[x + 1] = color;
    }
}

/* Colour clocks [drawn, to) of the current line: into the picture (the
 * visible ones; the edges are black from xl_init())
 */
static void draw_to(int to)
{
    if (!antic.display) {
        return;
    }
    if (to > CC_VIS_END) {
        to = CC_VIS_END;
    }
    int a = gtia.drawn;
    if (to <= a) {
        return;
    }
    gtia.drawn = to;
    if (!antic.pf_built) {
        antic_build_pf();
    }
    if (gtia.objects) {
        build_objects(a, to);
    }
    if (gtia.prior & 0xC0) {
        draw_gtia_mode(a, to);
        return;
    }
    const uint8_t *col = gtia.col;
    uint8_t *row = gtia.row;
    int xa = (a - CC_FIRST) * 2, xb = (to - CC_FIRST) * 2;
    if (!gtia.objects && antic.pf_blank) {
        memset(row + xa, col[8], xb - xa);         /* background only */
        return;
    }
    uint8_t ctab[6] = {
        col[8], col[4], col[5], col[6], col[7], (uint8_t)((col[6] & 0xF0) | (col[5] & 0x0E)),
    };
    const uint8_t *pfp = antic.pf;
    if (!gtia.objects) {
        /* two pixels at a time: a colour clock's pair of codes is one index */
        uint16_t pair[64];
        for (int c0 = 0; c0 < 6; c0++) {
            for (int c1 = 0; c1 < 6; c1++) {
                pair[c0 | c1 << 3] = ctab[c0] | ctab[c1] << 8;
            }
        }
        const uint16_t *src = (const uint16_t *)pfp + xa / 2;
        uint16_t *dst = (uint16_t *)(row + xa);
        for (int n = (xb - xa) / 2; n > 0; n--) {
            uint16_t v = *src++;
            *dst++ = pair[(v & 7) | (v >> 5)];
        }
        return;
    }
    bool hires = antic.hires, fifth = gtia.prior & 0x10;
    or_tables();
    for (int cc = a; cc < to; cc++) {
        int x = (cc - CC_FIRST) * 2;
        uint8_t c0 = pfp[x], c1 = pfp[x + 1];
        uint8_t m = s_pm[cc - CC_FIRST];
        if (!m) {
            row[x] = ctab[c0];
            row[x + 1] = ctab[c1];
            continue;
        }
        uint8_t players = m & 15, missiles = m >> 4;
        uint8_t pf;
        if (hires) {
            pf = (c0 | c1) ? 0x04 : 0;      /* the priority logic sees PF2 */
        } else {
            pf = c0 ? 1 << (c0 - PF_0) : 0;
        }
        uint8_t p = players, pfx = pf;
        if (fifth) {
            pfx |= missiles ? 0x08 : 0;
        } else {
            p |= missiles;
        }
        uint8_t color = selected(gtia.sel[p | pfx << 4]);
        if (hires) {
            /* the hires bits put PF1's luminance on whatever shows */
            uint8_t lum = (color & 0xF0) | (col[5] & 0x0E);
            row[x] = c0 == PF_HIRES_ON ? lum : color;
            row[x + 1] = c1 == PF_HIRES_ON ? lum : color;
            collide(players, missiles, (c0 == PF_HIRES_ON || c1 == PF_HIRES_ON) ? 0x04 : 0);
        } else {
            row[x] = row[x + 1] = color;
            collide(players, missiles, pf);
        }
    }
}

void gtia_begin_line(void)
{
    gtia.drawn = CC_VIS_FIRST;
    if (antic.display) {
        /* (the edges, horizontal blank, stay black from xl_init()) */
        gtia.row = xl.cfg.frame + (xl.line - 8) * XL_SCREEN_WIDTH;
    }
}

/* Lines drawn whole, without players or missiles: the key of their inputs
 * (ANTIC's playfield, the colours, PRIOR). A line whose key is the same as
 * the frame before is already in the picture.
 */
static uint32_t s_line_key[XL_SCREEN_HEIGHT];

static uint32_t line_key(void)
{
    uint32_t h = antic.key;
    for (int i = 0; i < 9; i++) {
        h = (h ^ gtia.col[i]) * 16777619u;
    }
    h = (h ^ gtia.prior) * 16777619u;
    return h ? h : 1;
}

void gtia_end_line(void)
{
    if (!antic.display) {
        return;
    }
    int row = xl.line - 8;
    bool whole = gtia.drawn == CC_VIS_FIRST && !gtia.objects;   /* no write split it, no objects */
    uint32_t key = whole ? line_key() : 0;
    if (whole && key == s_line_key[row]) {
        return;                             /* unchanged */
    }
    draw_to(CC_VIS_END);
    s_line_key[row] = key;
    gtia.changed = true;
}

/* The picture changed: the presenter looks again (and compares rows) */
void gtia_frame_end(void)
{
    if (gtia.changed) {
        gtia.signature++;
        gtia.changed = false;
    }
}

void gtia_forget_lines(void)
{
    memset(s_line_key, 0, sizeof(s_line_key));
}

void gtia_pm_dma(uint8_t missiles, const uint8_t players[4], bool do_missiles, bool do_players)
{
    bool odd = xl.line & 1;
    if (do_missiles && (gtia.gractl & 1)) {
        for (int i = 0; i < 4; i++) {
            if (!(gtia.vdelay & (1 << i)) || odd) {
                uint8_t mask = 3 << (i * 2);
                gtia.grafm = (gtia.grafm & ~mask) | (missiles & mask);
            }
        }
    }
    if (do_players && (gtia.gractl & 2)) {
        for (int i = 0; i < 4; i++) {
            if (!(gtia.vdelay & (0x10 << i)) || odd) {
                gtia.grafp[i] = players[i];
            }
        }
    }
    update_objects();
}

/* Where the beam is, in colour clocks */
static inline int beam(void)
{
    return 2 * xl_line_cycle() - 2;
}

uint8_t gtia_read(uint8_t reg)
{
    reg &= 0x1F;
    if (reg < 0x10) {
        draw_to(beam());                    /* collisions up to now */
        switch (reg >> 2) {
        case 0: return gtia.mpf[reg & 3];
        case 1: return gtia.ppf[reg & 3];
        case 2: return gtia.mpl[reg & 3];
        default: return gtia.ppl[reg & 3];
        }
    }
    switch (reg) {
    case 0x10: case 0x11: case 0x12: case 0x13: {
        int i = reg & 3;
        return (gtia.gractl & 4) ? gtia.trig_latch[i] : gtia.trig[i];
    }
    case 0x14: return xl.cfg.tv == XL_TV_PAL ? 0x01 : 0x0F;
    case 0x1F: return ~(gtia.consol_out | gtia.consol_in) & 0x0F;
    default: return 0x0F;
    }
}

void gtia_write(uint8_t reg, uint8_t v)
{
    reg &= 0x1F;
    int b = beam();
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        draw_to(b + 5);
        gtia.hposp[reg] = v;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        draw_to(b + 5);
        gtia.hposm[reg & 3] = v;
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        draw_to(b + 5);
        gtia.sizep[reg & 3] = v & 3;
        break;
    case 0x0C:
        draw_to(b + 5);
        gtia.sizem = v;
        break;
    case 0x0D: case 0x0E: case 0x0F: case 0x10:
        draw_to(b + 3);
        gtia.grafp[reg - 0x0D] = v;
        update_objects();
        break;
    case 0x11:
        draw_to(b + 3);
        gtia.grafm = v;
        update_objects();
        break;
    case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A:
        draw_to(b + 1);
        gtia.col[reg - 0x12] = v & 0xFE;
        break;
    case 0x1B:
        draw_to(b + 2);
        gtia.prior = v;
        build_priority();
        break;
    case 0x1C:
        gtia.vdelay = v;
        break;
    case 0x1D:
        gtia.gractl = v & 7;
        if (!(v & 4)) {
            for (int i = 0; i < 4; i++) {
                gtia.trig_latch[i] = 1;
            }
        }
        break;
    case 0x1E:                              /* HITCLR */
        draw_to(b);
        memset(gtia.mpf, 0, sizeof(gtia.mpf));
        memset(gtia.ppf, 0, sizeof(gtia.ppf));
        memset(gtia.mpl, 0, sizeof(gtia.mpl));
        memset(gtia.ppl, 0, sizeof(gtia.ppl));
        break;
    case 0x1F:
        if ((v ^ gtia.consol_out) & 8) {
            pokey_consol_speaker(v & 8);
        }
        gtia.consol_out = v & 0x0F;
        break;
    default:
        break;
    }
}

/* Latched triggers follow presses */
void gtia_triggers(void)
{
    for (int i = 0; i < 4; i++) {
        gtia.trig_latch[i] &= gtia.trig[i];
    }
}

/* ---- Palette ----
 *
 * NTSC: hue n is the colour burst ("gold") delayed by n x 24 degrees;
 * PAL: the UV angles of the PAL GTIA's hues (Altirra manual, table 14).
 * Luminance in 16 even steps.
 */
static const float pal_angles[16] = {
    0, 135.0f, 112.5f, 90.0f, 67.5f, 45.0f, 22.5f, 337.5f,
    315.0f, 292.5f, 270.0f, 225.0f, 202.5f, 180.0f, 157.5f, 135.0f,
};

void palette_build(xl_tv_t tv, uint16_t *rgb565)
{
    const float sat = 0.21f;
    for (int c = 0; c < 256; c++) {
        int hue = c >> 4, lum = c & 15;
        float y = lum / 15.0f;
        float u = 0, v = 0;
        if (hue) {
            float deg = tv == XL_TV_PAL ? pal_angles[hue] : 180.0f - hue * 24.0f;
            float rad = deg * (float)M_PI / 180.0f;
            u = sat * cosf(rad);
            v = sat * sinf(rad);
        }
        float rgb[3] = { y + 1.140f * v, y - 0.395f * u - 0.581f * v, y + 2.032f * u };
        int q[3];
        for (int i = 0; i < 3; i++) {
            float f = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
            q[i] = (int)(f * 255.0f + 0.5f);
        }
        rgb565[c] = (uint16_t)((q[0] >> 3) << 11 | (q[1] >> 2) << 5 | (q[2] >> 3));
    }
}
