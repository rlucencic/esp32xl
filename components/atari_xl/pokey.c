/*
 * POKEY: timers and their IRQs, keyboard, serial port, RANDOM; sound.
 *
 * Timer periods (Altirra Hardware Reference Manual 5.3): AUDF+4 cycles at
 * 1.79 MHz, AUDF+7 for a linked pair, (AUDF+1) x 28 or x 114 at 64 or 15
 * kHz. The serial port shifts a bit every two periods of timer 4: a byte
 * (start, 8 data, stop) takes 20 of them. Interrupt status bits are active
 * low in IRQST; bit 3 (serial output complete) is not latched and simply
 * shows that the output shift register is idle.
 *
 * Sound: writes to the audio registers are queued with their times and
 * played on the speaker's task, every channel's output integrated over each
 * sample period; noise from the shared 4, 5, 9 and 17-bit polynomial
 * counters, the high-pass filters, volume-only mode, the console speaker.
 */
#include <string.h>
#include "xl_internal.h"

/* ---- Polynomial counters (shared by the CPU side and the sound) ---- */

#define P4_LEN      15
#define P5_LEN      31
#define P9_LEN      511
#define P17_LEN     131071

static uint8_t s_p4[4], s_p5[4], s_p9[64], *s_p17;     /* bits, packed */

static inline int bit_at(const uint8_t *t, uint32_t n)
{
    return (t[n >> 3] >> (n & 7)) & 1;
}

static void set_bit(uint8_t *t, uint32_t n, int v)
{
    if (v) {
        t[n >> 3] |= 1 << (n & 7);
    } else {
        t[n >> 3] &= ~(1 << (n & 7));
    }
}

/* XNOR LFSR x^n + x^k + 1, shifting right */
static void lfsr(uint8_t *t, int n, int k, uint32_t len)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < len; i++) {
        set_bit(t, i, s & 1);
        uint32_t fb = ((s ^ (s >> (n - k))) & 1) ^ 1;
        s = (s >> 1) | fb << (n - 1);
    }
}

static void polys_init(void)
{
    static const char p4[] = "000011101100101";
    static const char p5[] = "0000011100100010101111011010011";
    for (int i = 0; i < P4_LEN; i++) {
        set_bit(s_p4, i, p4[i] == '1');
    }
    for (int i = 0; i < P5_LEN; i++) {
        set_bit(s_p5, i, p5[i] == '1');
    }
    lfsr(s_p9, 9, 4, P9_LEN);
    if (s_p17 == NULL) {
        s_p17 = xl_calloc((P17_LEN + 7) / 8, 1);
        lfsr(s_p17, 17, 12, P17_LEN);
    }
}

/* ---- CPU side ---- */

#define KEY_QUEUE   64

typedef struct {
    uint8_t audf[4], audc[4], audctl, skctl, irqen;
    uint8_t pending;                    /* latched interrupts (1 = pending) */
    uint8_t kbcode, serin;
    bool key_held, shift, ctrl;
    uint8_t held;                       /* the key code held */
    bool kb_overrun, ser_overrun;
    uint64_t poly_origin;               /* the polynomial counters started then */
    uint64_t timer_next[4];             /* next underflow of timers 1, 2 and 4 (0, 1, 3) */
    /* serial output: SEROUT loads into the shift register at the next bit
     * clock (load_at), then shifts out until shift_end
     */
    bool hold_full, shifting, load_pending;
    uint8_t hold, shift_byte;
    uint64_t load_at, shift_end;
    /* keyboard events from other tasks: key | 0x80 when down */
    volatile uint8_t queue[KEY_QUEUE];
    volatile uint32_t q_head, q_tail;
    /* typing */
    char text[1024];
    volatile int text_pos, text_len;
    int type_phase;                     /* frames into the current character */
    int type_key;
    bool type_shift;
} pokey_t;

static pokey_t pk;

/* The timer counts the 1.79 MHz machine clock (else 64 or 15 kHz) */
static bool fast_clock(uint8_t audctl, int ch)
{
    if (ch == 1 && (audctl & 0x10)) {
        return audctl & 0x40;
    }
    if (ch == 3 && (audctl & 0x08)) {
        return audctl & 0x20;
    }
    return (ch == 0 && (audctl & 0x40)) || (ch == 2 && (audctl & 0x20));
}

static uint32_t period(const uint8_t *audf, uint8_t audctl, int ch)
{
    uint32_t base = (audctl & 1) ? 114 : 28;
    if (ch == 1 && (audctl & 0x10)) {
        uint32_t n = audf[0] | audf[1] << 8;
        return (audctl & 0x40) ? n + 7 : (n + 1) * base;
    }
    if (ch == 3 && (audctl & 0x08)) {
        uint32_t n = audf[2] | audf[3] << 8;
        return (audctl & 0x20) ? n + 7 : (n + 1) * base;
    }
    bool fast = (ch == 0 && (audctl & 0x40)) || (ch == 2 && (audctl & 0x20));
    return fast ? audf[ch] + 4u : (audf[ch] + 1u) * base;
}

static inline uint32_t cpu_period(int ch)
{
    return period(pk.audf, pk.audctl, ch);
}

/* One byte on the serial port: 10 bits, two timer 4 periods each */
static uint32_t byte_time(void)
{
    uint32_t t = 20 * cpu_period(3);
    return t < 40 ? 40 : t;
}

static const uint8_t timer_bit[4] = { 0x01, 0x02, 0, 0x04 };

bool pokey_irq(void)
{
    return (pk.pending & pk.irqen) || ((pk.irqen & 0x08) && !pk.shifting);
}

/* Bring a timer's next underflow to now or later */
static void timer_catch_up(int ch, uint64_t now)
{
    uint64_t next = pk.timer_next[ch];
    if (next > now) {
        return;
    }
    uint32_t p = cpu_period(ch);
    uint64_t behind = now - next;
    pk.timer_next[ch] = next + (behind / p + 1) * p;
}

static uint64_t s_next_event = UINT64_MAX;

static void schedule(void)
{
    uint64_t next = UINT64_MAX;
    for (int ch = 0; ch < 4; ch++) {
        if (timer_bit[ch] && (pk.irqen & timer_bit[ch]) && pk.timer_next[ch] < next) {
            next = pk.timer_next[ch];
        }
    }
    if (pk.shifting && pk.shift_end < next) {
        next = pk.shift_end;
    }
    if (pk.load_pending && pk.load_at < next) {
        next = pk.load_at;
    }
    s_next_event = next;
    xl_event_at(next);
}

uint64_t pokey_next_event(void)
{
    return s_next_event;
}

static void start_shift(uint64_t at)
{
    pk.load_pending = false;
    pk.shift_byte = pk.hold;
    pk.hold_full = false;
    pk.shifting = true;
    pk.shift_end = at + byte_time();
    if (pk.irqen & 0x10) {
        pk.pending |= 0x10;             /* serial output ready */
    }
}

void pokey_events(uint64_t now)
{
    if (now < s_next_event) {
        return;
    }
    for (int ch = 0; ch < 4; ch++) {
        if (!timer_bit[ch] || !(pk.irqen & timer_bit[ch])) {
            continue;
        }
        if (pk.timer_next[ch] <= now) {
            pk.pending |= timer_bit[ch];
            timer_catch_up(ch, now);
        }
    }
    if (pk.load_pending && pk.load_at <= now) {
        start_shift(pk.load_at);
    }
    while (pk.shifting && pk.shift_end <= now) {
        uint64_t end = pk.shift_end;
        uint8_t b = pk.shift_byte;
        if (pk.hold_full) {
            start_shift(end);
        } else {
            pk.shifting = false;        /* output complete */
        }
        sio_from_computer(b);
    }
    xl_update_irq();
    schedule();
}

void pokey_serial_in(uint8_t byte)
{
    pk.serin = byte;
    if (pk.irqen & 0x20) {
        if (pk.pending & 0x20) {
            pk.ser_overrun = true;
        }
        pk.pending |= 0x20;
        xl_update_irq();
    }
}

static uint8_t random_byte(void)
{
    if (!(pk.skctl & 3)) {
        return 0xFF;                    /* initialisation mode */
    }
    uint32_t t = (uint32_t)(xl_now() - pk.poly_origin);
    uint8_t v = 0;
    if (pk.audctl & 0x80) {
        uint32_t n = t % P9_LEN;
        for (int i = 0; i < 8; i++) {
            v |= bit_at(s_p9, (n + i) % P9_LEN) << i;
        }
    } else {
        uint32_t n = t % P17_LEN;
        for (int i = 0; i < 8; i++) {
            v |= bit_at(s_p17, (n + i) % P17_LEN) << i;
        }
    }
    return ~v;
}

static void audio_write(uint8_t reg, uint8_t v);

void pokey_reset(void)
{
    polys_init();
    memset(pk.audf, 0, sizeof(pk.audf));
    memset(pk.audc, 0, sizeof(pk.audc));
    pk.audctl = 0;
    pk.skctl = 0;
    pk.irqen = 0;
    pk.pending = 0;
    pk.kbcode = 0xFF;
    pk.key_held = false;
    pk.kb_overrun = pk.ser_overrun = false;
    pk.hold_full = pk.shifting = false;
    pk.poly_origin = xl_now();
    for (int ch = 0; ch < 4; ch++) {
        pk.timer_next[ch] = xl_now() + cpu_period(ch);
    }
    pokey_audio_reset();
    schedule();
}

uint8_t pokey_read(uint8_t reg)
{
    switch (reg & 15) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
        return 228;                         /* POT0-7: no paddles */
    case 0x08: return 0;                    /* ALLPOT: scans done */
    case 0x09: return pk.kbcode;
    case 0x0A: return random_byte();
    case 0x0D: return pk.serin;
    case 0x0E: return ~(pk.pending | (pk.shifting ? 0 : 0x08));
    case 0x0F:
        return 0x80 | (pk.kb_overrun ? 0 : 0x40) | (pk.ser_overrun ? 0 : 0x20) | 0x10 |
               (pk.shift ? 0 : 0x08) | (pk.key_held ? 0 : 0x04) | 0x02 | 0x01;
    default: return 0xFF;
    }
}

void pokey_write(uint8_t reg, uint8_t v)
{
    reg &= 15;
    uint64_t now = xl_now();
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
        if (reg & 1) {
            pk.audc[reg >> 1] = v;
        } else {
            pk.audf[reg >> 1] = v;
        }
        audio_write(reg, v);
        break;
    case 0x08:
        pk.audctl = v;
        audio_write(reg, v);
        break;
    case 0x09:                              /* STIMER: the first IRQ 4 cycles after a period */
        for (int ch = 0; ch < 4; ch++) {
            uint32_t p = cpu_period(ch);
            pk.timer_next[ch] = now + p + (fast_clock(pk.audctl, ch) ? 4 : 0);
        }
        audio_write(reg, v);
        break;
    case 0x0A:                              /* SKRES */
        pk.kb_overrun = pk.ser_overrun = false;
        break;
    case 0x0B:                              /* POTGO */
        break;
    case 0x0D:                              /* SEROUT */
        pk.hold = v;
        pk.hold_full = true;
        if (!pk.shifting && !pk.load_pending) {
            pk.load_pending = true;
            pk.load_at = now + 2 * cpu_period(3);       /* the next bit clock */
        }
        break;
    case 0x0E:                              /* IRQEN */
        pk.irqen = v;
        pk.pending &= v;
        for (int ch = 0; ch < 4; ch++) {
            if (timer_bit[ch] && (v & timer_bit[ch])) {
                timer_catch_up(ch, now);
            }
        }
        break;
    case 0x0F:                              /* SKCTL */
        if (!(v & 3)) {
            /* initialisation mode: serial port and polynomial counters reset */
            pk.shifting = pk.hold_full = pk.load_pending = false;
        } else if (!(pk.skctl & 3)) {
            pk.poly_origin = now;
        }
        if (!(v & 2)) {
            pk.key_held = false;
        }
        pk.skctl = v;
        audio_write(reg, v);
        break;
    default:
        break;
    }
    xl_update_irq();
    schedule();
}

/* ---- Keyboard ---- */

void xl_key(uint8_t key, bool down)
{
    uint32_t head = pk.q_head;
    if (head - pk.q_tail >= KEY_QUEUE) {
        return;
    }
    pk.queue[head % KEY_QUEUE] = key | (down ? 0x80 : 0);
    __atomic_store_n(&pk.q_head, head + 1, __ATOMIC_RELEASE);
}

void xl_keys_up(void)
{
    for (int k = 0; k < XL_KEYS; k++) {
        xl_key(k, false);
    }
}

static void key_event(uint8_t key, bool down)
{
    switch (key) {
    case XL_KEY_SHIFT: pk.shift = down; return;
    case XL_KEY_CTRL: pk.ctrl = down; return;
    case XL_KEY_BREAK:
        if (down && (pk.skctl & 2) && (pk.irqen & 0x80)) {
            pk.pending |= 0x80;
        }
        return;
    case XL_KEY_START:
    case XL_KEY_SELECT:
    case XL_KEY_OPTION: {
        uint8_t bit = 1 << (key - XL_KEY_START);
        gtia.consol_in = down ? gtia.consol_in | bit : gtia.consol_in & ~bit;
        return;
    }
    default:
        break;
    }
    if (key >= 64 || !(pk.skctl & 2)) {
        return;
    }
    if (down) {
        pk.kbcode = key | (pk.shift ? 0x40 : 0) | (pk.ctrl ? 0x80 : 0);
        pk.held = key;
        pk.key_held = true;
        if (pk.irqen & 0x40) {
            if (pk.pending & 0x40) {
                pk.kb_overrun = true;
            }
            pk.pending |= 0x40;
        }
    } else if (pk.held == key) {
        pk.key_held = false;
    }
}

/* Typing: each character's key down for 3 frames, up for 3, and the next
 * only once the OS has taken the last from CH ($02FC back to $FF): a
 * program busy with a line (BASIC after Return) would lose one
 */
static void typing(void)
{
    if (pk.text_pos >= pk.text_len) {
        return;
    }
    if (pk.type_phase == 0 && xl_peek(0x02FC) != 0xFF) {
        return;
    }
    if (pk.type_phase == 0) {
        bool shift;
        int key = xl_ascii_key(pk.text[pk.text_pos], &shift);
        pk.type_key = key;
        pk.type_shift = shift;
        if (key >= 0) {
            if (shift) {
                key_event(XL_KEY_SHIFT, true);
            }
            key_event(key, true);
        }
    } else if (pk.type_phase == 3 && pk.type_key >= 0) {
        key_event(pk.type_key, false);
        if (pk.type_shift) {
            key_event(XL_KEY_SHIFT, false);
        }
    }
    if (++pk.type_phase == 6) {
        pk.type_phase = 0;
        pk.text_pos++;
    }
}

void pokey_frame(void)
{
    uint32_t head = __atomic_load_n(&pk.q_head, __ATOMIC_ACQUIRE);
    while (pk.q_tail != head) {
        uint8_t e = pk.queue[pk.q_tail % KEY_QUEUE];
        pk.q_tail++;
        key_event(e & 0x7F, e & 0x80);
    }
    typing();
    xl_update_irq();
}

void xl_type(const char *text)
{
    pk.text_len = 0;
    size_t n = strlen(text);
    if (n >= sizeof(pk.text)) {
        n = sizeof(pk.text) - 1;
    }
    memcpy(pk.text, text, n);
    pk.text_pos = 0;
    pk.type_phase = 0;
    pk.text_len = (int)n;
}

int xl_ascii_key(char c, bool *shift)
{
    static const char letters[26] = {
        0x3F, 0x15, 0x12, 0x3A, 0x2A, 0x38, 0x3D, 0x39, 0x0D, 0x01, 0x05, 0x00, 0x25,
        0x23, 0x08, 0x0A, 0x2F, 0x28, 0x3E, 0x2D, 0x0B, 0x10, 0x2E, 0x16, 0x2B, 0x17,
    };
    static const struct {
        char c;
        uint8_t key;
        bool shift;
    } others[] = {
        { '1', 0x1F, 0 }, { '2', 0x1E, 0 }, { '3', 0x1A, 0 }, { '4', 0x18, 0 }, { '5', 0x1D, 0 },
        { '6', 0x1B, 0 }, { '7', 0x33, 0 }, { '8', 0x35, 0 }, { '9', 0x30, 0 }, { '0', 0x32, 0 },
        { '!', 0x1F, 1 }, { '"', 0x1E, 1 }, { '#', 0x1A, 1 }, { '$', 0x18, 1 }, { '%', 0x1D, 1 },
        { '&', 0x1B, 1 }, { '\'', 0x33, 1 }, { '@', 0x35, 1 }, { '(', 0x30, 1 }, { ')', 0x32, 1 },
        { '<', 0x36, 0 }, { '>', 0x37, 0 }, { '-', 0x0E, 0 }, { '=', 0x0F, 0 }, { '_', 0x0E, 1 },
        { '|', 0x0F, 1 }, { ';', 0x02, 0 }, { ':', 0x02, 1 }, { '+', 0x06, 0 }, { '\\', 0x06, 1 },
        { '*', 0x07, 0 }, { '^', 0x07, 1 }, { ',', 0x20, 0 }, { '[', 0x20, 1 }, { '.', 0x22, 0 },
        { ']', 0x22, 1 }, { '/', 0x26, 0 }, { '?', 0x26, 1 }, { ' ', 0x21, 0 }, { '\n', 0x0C, 0 },
        { '\t', 0x2C, 0 }, { 27, 0x1C, 0 },
    };
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        return letters[c - 'a'];
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        if (others[i].c == c) {
            *shift = others[i].shift;
            return others[i].key;
        }
    }
    return -1;
}

/* ---- Sound ---- */

#define EVENT_QUEUE     4096
#define SPEAKER_LEVEL   6

typedef struct {
    uint64_t time;
    uint8_t reg, value;
} audio_event_t;

static struct {
    bool enabled;
    audio_event_t *queue;
    volatile uint32_t head, tail;
    volatile uint64_t now;              /* emulated time sound may be rendered up to */
} aud;

static struct {
    uint64_t pos_fp;                    /* the next sample's start, 1/65536 cycles */
    uint32_t cps_fp;                    /* cycles per sample, 16.16 */
    uint8_t audf[4], audc[4], audctl, skctl;
    uint32_t period[4];
    uint64_t next[4];                   /* each channel's next tick */
    uint8_t ff[4], hp[2];
    bool speaker;
    uint64_t poly_origin;
    int32_t dc;
} ren;

static void audio_write(uint8_t reg, uint8_t v)
{
    if (!aud.enabled || aud.queue == NULL) {
        return;
    }
    uint32_t head = aud.head;
    if (head - __atomic_load_n(&aud.tail, __ATOMIC_ACQUIRE) >= EVENT_QUEUE) {
        return;                             /* full: drop it */
    }
    audio_event_t *e = &aud.queue[head % EVENT_QUEUE];
    e->time = xl_now();
    e->reg = reg;
    e->value = v;
    __atomic_store_n(&aud.head, head + 1, __ATOMIC_RELEASE);
}

void pokey_consol_speaker(bool on)
{
    audio_write(0x10, on);
}

/* Emulated time up to which sound may be rendered (end of each line) */
void pokey_audio_time(uint64_t now)
{
    __atomic_store_n(&aud.now, now, __ATOMIC_RELEASE);
}

static void ren_periods(void)
{
    for (int ch = 0; ch < 4; ch++) {
        bool low_linked = (ch == 0 && (ren.audctl & 0x10)) || (ch == 2 && (ren.audctl & 0x08));
        ren.period[ch] = low_linked ? 0 : period(ren.audf, ren.audctl, ch);
    }
}

static void ren_write(uint8_t reg, uint8_t v, uint64_t t)
{
    switch (reg) {
    case 0x00: case 0x02: case 0x04: case 0x06:
        ren.audf[reg >> 1] = v;
        break;
    case 0x01: case 0x03: case 0x05: case 0x07:
        ren.audc[reg >> 1] = v;
        return;
    case 0x08:
        ren.audctl = v;
        if (!(v & 4)) {
            ren.hp[0] = 1;
        }
        if (!(v & 2)) {
            ren.hp[1] = 1;
        }
        break;
    case 0x09:
        ren_periods();
        for (int ch = 0; ch < 4; ch++) {
            ren.next[ch] = ren.period[ch] ? t + ren.period[ch] : UINT64_MAX;
            ren.ff[ch] = 1;
        }
        return;
    case 0x0F:
        if ((v & 3) && !(ren.skctl & 3)) {
            ren.poly_origin = t;
        }
        ren.skctl = v;
        return;
    case 0x10:
        ren.speaker = v;
        return;
    default:
        return;
    }
    ren_periods();
    for (int ch = 0; ch < 4; ch++) {
        if (!ren.period[ch]) {
            ren.next[ch] = UINT64_MAX;
        } else if (ren.next[ch] == UINT64_MAX) {
            ren.next[ch] = t + ren.period[ch];
        }
    }
}

static inline int poly_noise(int ch, uint64_t t)
{
    uint32_t n = (uint32_t)(t - ren.poly_origin) + ch;
    uint8_t c = ren.audc[ch];
    if (c & 0x40) {
        return bit_at(s_p4, n % P4_LEN);
    }
    return (ren.audctl & 0x80) ? bit_at(s_p9, n % P9_LEN) : bit_at(s_p17, n % P17_LEN);
}

static void ren_tick(int ch, uint64_t t)
{
    uint8_t c = ren.audc[ch];
    uint32_t n = (uint32_t)(t - ren.poly_origin) + ch;
    if ((c & 0x80) || bit_at(s_p5, n % P5_LEN)) {
        if (c & 0x20) {
            ren.ff[ch] ^= 1;
        } else {
            ren.ff[ch] = poly_noise(ch, t);
        }
    }
    if (ch == 2 && (ren.audctl & 4)) {
        ren.hp[0] = ren.ff[0];
    } else if (ch == 3 && (ren.audctl & 2)) {
        ren.hp[1] = ren.ff[1];
    }
}

static int ren_level(void)
{
    int level = ren.speaker ? SPEAKER_LEVEL : 0;
    for (int ch = 0; ch < 4; ch++) {
        uint8_t c = ren.audc[ch];
        int vol = c & 15;
        if (!vol) {
            continue;
        }
        if (c & 0x10) {
            level += vol;                   /* volume only */
            continue;
        }
        int out = ren.ff[ch];
        if (ch < 2) {
            out ^= ren.hp[ch];
        }
        level += out ? vol : 0;
    }
    return level;
}

void pokey_audio_reset(void)
{
    if (aud.queue == NULL) {
        aud.queue = xl_malloc(EVENT_QUEUE * sizeof(audio_event_t));
    }
    aud.head = aud.tail = 0;
    memset(&ren, 0, sizeof(ren));
    uint64_t now = xl_now();
    ren.pos_fp = now << 16;
    ren.cps_fp = (uint32_t)(((uint64_t)xl.clock_hz << 16) / XL_AUDIO_HZ);
    ren.hp[0] = ren.hp[1] = 1;
    ren.poly_origin = now;
    ren_periods();
    for (int ch = 0; ch < 4; ch++) {
        ren.next[ch] = ren.period[ch] ? now + ren.period[ch] : UINT64_MAX;
    }
    aud.now = now;
}

static int16_t ren_sample(void)
{
    uint64_t t0 = ren.pos_fp >> 16, t1 = (ren.pos_fp + ren.cps_fp) >> 16;
    uint64_t t = t0;
    int level = ren_level();
    int32_t acc = 0;
    for (;;) {
        int ch = -1;
        uint64_t tmin = t1;
        for (int i = 0; i < 4; i++) {
            if (ren.next[i] < tmin) {
                tmin = ren.next[i];
                ch = i;
            }
        }
        if (ch < 0) {
            break;
        }
        acc += level * (int32_t)(tmin - t);
        t = tmin;
        ren_tick(ch, tmin);
        ren.next[ch] += ren.period[ch];
        level = ren_level();
    }
    acc += level * (int32_t)(t1 - t);
    ren.pos_fp += ren.cps_fp;
    int32_t span = (int32_t)(t1 - t0);
    int32_t l = span ? (acc << 8) / span : level << 8;  /* 8.8 */
    /* POKEY's mixing compresses: nearly linear up to 15, about double at 60 */
    if (l > 15 << 8) {
        l = (15 << 8) + (l - (15 << 8)) / 3;
    }
    int32_t s = l * 4;                      /* one channel at 15: +-7680, four: ~+-15000 */
    ren.dc += ((s << 8) - ren.dc) >> 11;
    s -= ren.dc >> 8;
    return s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
}

int XL_HOT xl_audio_render(int16_t *dst, int frames)
{
    uint64_t limit = __atomic_load_n(&aud.now, __ATOMIC_ACQUIRE);
    uint32_t tail = aud.tail;
    int done = 0;
    while (done < frames && ((ren.pos_fp + ren.cps_fp) >> 16) <= limit) {
        uint64_t end = (ren.pos_fp + ren.cps_fp) >> 16;
        uint32_t head = __atomic_load_n(&aud.head, __ATOMIC_ACQUIRE);
        while (tail != head && aud.queue[tail % EVENT_QUEUE].time < end) {
            const audio_event_t *e = &aud.queue[tail % EVENT_QUEUE];
            ren_write(e->reg, e->value, e->time);
            tail++;
        }
        int16_t s = ren_sample();
        *dst++ = s;
        *dst++ = s;
        done++;
    }
    __atomic_store_n(&aud.tail, tail, __ATOMIC_RELEASE);
    return done;
}

int xl_audio_pending(void)
{
    uint64_t now = __atomic_load_n(&aud.now, __ATOMIC_ACQUIRE);
    uint64_t pos = ren.pos_fp >> 16;
    return now > pos ? (int)(((now - pos) << 16) / ren.cps_fp) : 0;
}

void xl_audio_enable(bool enable)
{
    aud.enabled = enable;
}
