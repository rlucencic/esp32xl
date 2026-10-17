/*
 * Host build of the Atari core, for testing on a desktop:
 *
 *   build/xl_host <os.rom> [frames] [out.ppm]
 *
 * Environment:
 *   XL_BASIC=<basic.rom>     BASIC on (else off: OPTION held at start)
 *   XL_MODEL=130xe           (else 800XL)
 *   XL_NTSC=1                (else PAL)
 *   XL_DISK1=, XL_DISK2=     disk images
 *   XL_NOPATCH=1             no SIO patch: disks through POKEY only
 *   XL_SCRIPT=<file>         lines "<frame> <command> ...":
 *                              type <text> (\n is Return), key <hex> 0|1,
 *                              start|select|option|shift|ctrl|break 0|1,
 *                              joy <port> <bits>, shot <file.ppm>, text,
 *                              insert <drive 1-2> <path>, eject <drive>,
 *                              reset, warm, peek <hex>, dump <hex> <n>, flush
 *   XL_TEXT=1                print the screen text at the end
 *   XL_WAV=<file>            write the sound
 *   XL_TRACE=<hex pc>:<n>    print n instructions from when pc is reached
 *   XL_STATUS=1              machine state every 50 frames
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../xl_internal.h"

static uint8_t *load_file(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(1);
    }
    uint8_t *buf = calloc(1, want);
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    if (n != want) {
        fprintf(stderr, "%s: %zu bytes, expected %zu\n", path, n, want);
        exit(1);
    }
    return buf;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", XL_SCREEN_WIDTH, XL_SCREEN_HEIGHT);
    uint16_t line[XL_SCREEN_WIDTH];
    for (int y = 0; y < XL_SCREEN_HEIGHT; y++) {
        xl_render_line_rgb565(y, line);
        for (int x = 0; x < XL_SCREEN_WIDTH; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = {
                (uint8_t)(((c >> 11) & 31) * 255 / 31),
                (uint8_t)(((c >> 5) & 63) * 255 / 63),
                (uint8_t)((c & 31) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* The text screen at SAVMSC (40 x 24 internal codes) */
static void print_text(uint32_t frame)
{
    uint16_t sav = xl_peek(0x58) | xl_peek(0x59) << 8;
    printf("---- frame %u, screen at $%04X\n", frame, sav);
    for (int row = 0; row < 24; row++) {
        char s[41];
        for (int col = 0; col < 40; col++) {
            uint8_t c = xl_peek(sav + row * 40 + col) & 0x7F;
            /* internal code to ATASCII */
            uint8_t a = c < 64 ? c + 32 : c < 96 ? c - 64 : c;
            s[col] = isprint(a) ? a : '.';
        }
        s[40] = 0;
        int n = 40;
        while (n > 0 && s[n - 1] == ' ') {
            s[--n] = 0;
        }
        printf("|%s\n", s);
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[1024];
} line_t;

static line_t *s_script;
static int s_script_n, s_script_pos;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(1);
    }
    char buf[1200];
    s_script = calloc(8192, sizeof(line_t));
    while (fgets(buf, sizeof(buf), f) && s_script_n < 8192) {
        buf[strcspn(buf, "\r\n")] = 0;
        line_t *l = &s_script[s_script_n];
        int used = 0;
        if (sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &used) >= 2) {
            snprintf(l->arg, sizeof(l->arg), "%s", buf + used);
            s_script_n++;
        }
    }
    fclose(f);
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *o++ = '\n';
            s++;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void run_script(uint32_t frame)
{
    while (s_script_pos < s_script_n && s_script[s_script_pos].frame <= frame) {
        line_t *l = &s_script[s_script_pos++];
        if (!strcmp(l->cmd, "type")) {
            unescape(l->arg);
            xl_type(l->arg);
        } else if (!strcmp(l->cmd, "key")) {
            unsigned k = 0, d = 0;
            sscanf(l->arg, "%x %u", &k, &d);
            xl_key(k, d);
        } else if (!strcmp(l->cmd, "start") || !strcmp(l->cmd, "select") || !strcmp(l->cmd, "option") ||
                   !strcmp(l->cmd, "shift") || !strcmp(l->cmd, "ctrl") || !strcmp(l->cmd, "break")) {
            int key = !strcmp(l->cmd, "start") ? XL_KEY_START : !strcmp(l->cmd, "select") ? XL_KEY_SELECT :
                      !strcmp(l->cmd, "option") ? XL_KEY_OPTION : !strcmp(l->cmd, "shift") ? XL_KEY_SHIFT :
                      !strcmp(l->cmd, "ctrl") ? XL_KEY_CTRL : XL_KEY_BREAK;
            xl_key(key, atoi(l->arg));
        } else if (!strcmp(l->cmd, "joy")) {
            int port = 1;
            unsigned bits = 0;
            sscanf(l->arg, "%d %u", &port, &bits);
            xl_joystick(port, bits);
        } else if (!strcmp(l->cmd, "shot")) {
            write_ppm(l->arg);
        } else if (!strcmp(l->cmd, "text")) {
            print_text(frame);
        } else if (!strcmp(l->cmd, "insert")) {
            int d = 1;
            char path[200];
            if (sscanf(l->arg, "%d %199s", &d, path) == 2 && !xl_disk_insert(d - 1, path)) {
                printf("host: can't insert %s\n", path);
            }
        } else if (!strcmp(l->cmd, "eject")) {
            xl_disk_eject(atoi(l->arg) - 1);
        } else if (!strcmp(l->cmd, "reset")) {
            xl_reset();
        } else if (!strcmp(l->cmd, "warm")) {
            xl_warm_reset();
        } else if (!strcmp(l->cmd, "regs")) {
            printf("host: frame %u DMACTL %02X CHACTL %02X CHBASE %02X PMBASE %02X HSCROL %X VSCROL %X NMIEN %02X DL %04X\n",
                   frame, antic.dmactl, antic.chactl, antic.chbase, antic.pmbase, antic.hscrol, antic.vscrol, antic.nmien,
                   antic.dlist);
            printf("host: PRIOR %02X GRACTL %02X COL", gtia.prior, gtia.gractl);
            for (int i = 0; i < 9; i++) printf(" %02X", gtia.col[i]);
            printf(" HPOSP %02X %02X %02X %02X GRAFP %02X %02X %02X %02X GRAFM %02X\n", gtia.hposp[0], gtia.hposp[1],
                   gtia.hposp[2], gtia.hposp[3], gtia.grafp[0], gtia.grafp[1], gtia.grafp[2], gtia.grafp[3], gtia.grafm);
            uint16_t dl = xl_peek(0x230) | xl_peek(0x231) << 8;
            printf("host: display list at $%04X (SDLSTL):", dl);
            for (int i = 0; i < 48; i++) printf(" %02X", xl_peek(dl + i));
            printf("\n");
        } else if (!strcmp(l->cmd, "flush")) {
            xl_disk_flush();
        } else if (!strcmp(l->cmd, "peek")) {
            unsigned a = strtoul(l->arg, NULL, 16);
            printf("host: frame %u $%04X = $%02X\n", frame, a, xl_peek(a));
        } else if (!strcmp(l->cmd, "dump")) {
            unsigned a = 0, n = 16;
            sscanf(l->arg, "%x %u", &a, &n);
            printf("host: frame %u $%04X:", frame, a);
            for (unsigned i = 0; i < n; i++) {
                printf(" %02X", xl_peek(a + i));
            }
            printf("\n");
        } else {
            printf("host: unknown command %s\n", l->cmd);
        }
    }
}

/* ---- Tracing (m6502 host hook) ---- */

static int s_trace_pc = -1, s_trace_n;
static bool s_tracing;

void m6502_host_hook(m6502_t *c)
{
    if (s_trace_pc < 0) {
        return;
    }
    if (!s_tracing && c->pc == s_trace_pc) {
        s_tracing = true;
    }
    if (s_tracing && s_trace_n > 0) {
        char dis[32];
        m6502_disassemble(c, c->pc, dis, sizeof(dis));
        printf("%6u/%3d %04X  %-14s A=%02X X=%02X Y=%02X S=%02X P=%02X\n", xl.frames, xl.line, c->pc, dis,
               c->a, c->x, c->y, c->s, c->p);
        s_trace_n--;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: xl_host <os.rom> [frames] [out.ppm]\n");
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    const char *out = argc > 3 ? argv[3] : NULL;

    static xl_config_t cfg;
    cfg.model = getenv("XL_MODEL") && !strcmp(getenv("XL_MODEL"), "130xe") ? XL_MODEL_130XE : XL_MODEL_800XL;
    cfg.tv = getenv("XL_NTSC") ? XL_TV_NTSC : XL_TV_PAL;
    cfg.os = load_file(argv[1], 16384);
    if (getenv("XL_BASIC")) {
        cfg.basic_rom = load_file(getenv("XL_BASIC"), 8192);
        cfg.basic = true;
    }
    cfg.sio_patch = !getenv("XL_NOPATCH");
    cfg.ram = calloc(1, 128 * 1024);
    cfg.frame = calloc(XL_SCREEN_WIDTH, XL_SCREEN_HEIGHT);

    FILE *wav = NULL;
    if (getenv("XL_WAV")) {
        wav = fopen(getenv("XL_WAV"), "wb");
        xl_audio_enable(true);
        if (wav) {
            uint8_t hdr[44] = { 0 };
            fwrite(hdr, 1, 44, wav);
        }
    }
    if (getenv("XL_SIOLOG")) {
        extern bool xl_sio_log;
        xl_sio_log = true;
    }
    if (getenv("XL_TRACE")) {
        unsigned pc = 0;
        int n = 200;
        sscanf(getenv("XL_TRACE"), "%x:%d", &pc, &n);
        s_trace_pc = pc;
        s_trace_n = n;
    }
    if (!xl_init(&cfg)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    for (int d = 0; d < 2; d++) {
        char name[16];
        snprintf(name, sizeof(name), "XL_DISK%d", d + 1);
        if (getenv(name) && !xl_disk_insert(d, getenv(name))) {
            fprintf(stderr, "can't load %s\n", getenv(name));
        }
    }
    if (getenv("XL_SCRIPT")) {
        load_script(getenv("XL_SCRIPT"));
    }
    long samples = 0;
    for (int f = 0; f < frames; f++) {
        run_script(f);
        xl_run_frame();
        if (wav) {
            int16_t buf[2 * 1024];
            int n;
            while ((n = xl_audio_render(buf, 1024)) > 0) {
                fwrite(buf, 4, n, wav);
                samples += n;
            }
        }
        if (getenv("XL_STATUS") && f % 50 == 0) {
            xl_status_t s;
            xl_get_status(&s);
            printf("host: frame %d PC $%04X PORTB $%02X DMACTL $%02X NMIEN $%02X DL $%04X\n", f, s.pc, s.portb,
                   s.dmactl, s.nmien, s.dlist);
        }
    }
    run_script(frames);
    if (getenv("XL_TEXT")) {
        print_text(frames);
    }
    if (out) {
        write_ppm(out);
    }
    xl_disk_flush();
    if (wav) {
        uint32_t data = samples * 4;
        uint8_t hdr[44] = { 'R', 'I', 'F', 'F' };
        uint32_t v;
        v = 36 + data; memcpy(hdr + 4, &v, 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        v = 16; memcpy(hdr + 16, &v, 4);
        uint16_t w = 1; memcpy(hdr + 20, &w, 2);
        w = 2; memcpy(hdr + 22, &w, 2);
        v = XL_AUDIO_HZ; memcpy(hdr + 24, &v, 4);
        v = XL_AUDIO_HZ * 4; memcpy(hdr + 28, &v, 4);
        w = 4; memcpy(hdr + 32, &w, 2);
        w = 16; memcpy(hdr + 34, &w, 2);
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(hdr, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
