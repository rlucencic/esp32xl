/*
 * Atari 800XL and 130XE emulator on the Makerfabs MaTouch ESP32-S3 7" or
 * the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Atari into it: memory, ROMs, settings, keyboard map, joystick, drives
 * D1: and D2:, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/emu.h"
#include "rom_store.h"
#include "settings.h"
#include "xl.h"

static const char *TAG = "main";

static const char *const model_names[] = { "800XL", "130XE", "" };
static const char *const model_full[] = { "Atari 800XL", "Atari 130XE" };
static const char *const tv_names[] = { "PAL", "NTSC", "" };
static const char *const basic_names[] = { "Off", "On", "" };

static xl_config_t s_cfg;
static uint8_t *s_os, *s_basic;
static char s_rom_text[96];

/* ---- Memory ----
 *
 * Everything at power-on, before the screen, WiFi and Bluetooth fragment
 * PSRAM: 128 KB of RAM (the 130XE's; the 800XL uses 64), the ROMs, the
 * picture.
 */
static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    s_os = heap_caps_malloc(rom_store_size(ROM_OS), MALLOC_CAP_SPIRAM);
    s_basic = heap_caps_malloc(rom_store_size(ROM_BASIC), MALLOC_CAP_SPIRAM);
    s_cfg.frame = heap_caps_calloc(XL_SCREEN_WIDTH * XL_SCREEN_HEIGHT, 1, MALLOC_CAP_SPIRAM);
}

static void storage(bool sd_card)
{
    emu_set_drive_info(sd_card ? "Disk images on the SD card" : "No SD card: no disks");
}

/* ---- Starting ---- */

static bool start(void)
{
    if (s_cfg.ram == NULL || s_cfg.frame == NULL || s_os == NULL) {
        emu_set_message("Not enough memory: see the log.");
        return false;
    }
    if (!rom_store_load(ROM_OS, s_os)) {
        emu_set_message("No XL/XE OS ROM: put atarixl.rom into roms/ and flash with idf.py flash, "
                        "or copy it into the \"roms\" folder on the SD card.");
        return false;
    }
    bool basic = s_basic && rom_store_load(ROM_BASIC, s_basic);
    s_cfg.model = settings.model;
    s_cfg.tv = settings.tv;
    s_cfg.os = s_os;
    s_cfg.basic_rom = basic ? s_basic : NULL;
    s_cfg.basic = basic && settings.basic;
    s_cfg.sio_patch = true;
    snprintf(s_rom_text, sizeof(s_rom_text), "%s%s", rom_store_name(ROM_OS),
             basic ? ", ataribas.rom" : " (no ataribas.rom: no BASIC)");
    if (!xl_init(&s_cfg)) {
        emu_set_message("The Atari could not be set up: see the log.");
        return false;
    }
    ESP_LOGI(TAG, "%s %s, BASIC %s", model_full[settings.model], tv_names[settings.tv],
             s_cfg.basic ? "on" : "off");
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = XL_SCREEN_WIDTH;
    mode->height = XL_SCREEN_HEIGHT;
    mode->hz = xl_screen_hz();
}

/* ---- Keyboard and joystick ----
 *
 * Letters by position. Digits and symbols by the character they give on a
 * US keyboard, with SHIFT as the Atari needs it (Shift+' is ", which is
 * Shift+2 on the Atari). Return, Esc, Tab, Backspace (DELETE BACK S),
 * Caps Lock (CAPS) and Ctrl as themselves; Delete is Ctrl+DELETE, Insert
 * Ctrl+INSERT, Home CLEAR, ` and End the inverse (Atari) key; the cursor
 * keys are Ctrl with - = + *. F1 HELP, F2 START, F3 SELECT, F4 OPTION, F5
 * RESET, F7 or Pause BREAK. The numeric keypad is the joystick (8 4 6 2
 * and the diagonals, 0 or 5 fire) in port 1 (F11 switches to 2); F12 makes
 * the cursor keys the joystick too (Alt: fire).
 */
#define K_RETURN    0x0C
#define K_ESC       0x1C
#define K_TAB       0x2C
#define K_BACKSPACE 0x34
#define K_CAPS      0x3C
#define K_INVERSE   0x27
#define K_HELP      0x11
#define K_SPACE     0x21
#define K_LESS      0x36
#define K_GREATER   0x37
#define K_MINUS     0x0E
#define K_EQUALS    0x0F
#define K_PLUS      0x06
#define K_STAR      0x07

static const uint8_t letters[26] = {
    0x3F, 0x15, 0x12, 0x3A, 0x2A, 0x38, 0x3D, 0x39, 0x0D, 0x01, 0x05, 0x00, 0x25,
    0x23, 0x08, 0x0A, 0x2F, 0x28, 0x3E, 0x2D, 0x0B, 0x10, 0x2E, 0x16, 0x2B, 0x17,
};

/* US layout: the characters of the digit and symbol keys */
static const char us_plain[0x39] = {
    [0x1E] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    [0x2D] = '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
};
static const char us_shift[0x39] = {
    [0x1E] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    [0x2D] = '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?',
};

typedef struct {
    uint8_t key;                /* Atari key + 1, 0: none */
    int8_t shift, ctrl;         /* forced for it: -1 as held */
} held_t;

static held_t s_held[256];      /* per HID usage */
static bool s_pc_shift_l, s_pc_shift_r, s_pc_ctrl_l, s_pc_ctrl_r;
static bool s_shift, s_ctrl;    /* as the Atari has them now */
static uint8_t s_joy_pad, s_joy_arrows;
static bool s_arrows_joystick;

static void set_shift(bool on)
{
    if (on != s_shift) {
        s_shift = on;
        xl_key(XL_KEY_SHIFT, on);
    }
}

static void set_ctrl(bool on)
{
    if (on != s_ctrl) {
        s_ctrl = on;
        xl_key(XL_KEY_CTRL, on);
    }
}

/* Shift and Ctrl back to what the PC keyboard holds */
static void modifiers_as_held(void)
{
    set_shift(s_pc_shift_l || s_pc_shift_r);
    set_ctrl(s_pc_ctrl_l || s_pc_ctrl_r);
}

/* Press an Atari key, with Shift/Ctrl forced (0/1) or as held (-1) */
static void press(uint8_t usage, uint8_t key, int shift, int ctrl)
{
    if (shift >= 0) {
        set_shift(shift);
    }
    if (ctrl >= 0) {
        set_ctrl(ctrl);
    }
    xl_key(key, true);
    s_held[usage] = (held_t){ key + 1, shift, ctrl };
    /* KBCODE took the modifiers with the key: back as they are */
    modifiers_as_held();
}

static void release(uint8_t usage)
{
    if (s_held[usage].key) {
        xl_key(s_held[usage].key - 1, false);
        s_held[usage].key = 0;
    }
}

static void joystick_update(void)
{
    xl_joystick(settings.joystick, s_joy_pad | s_joy_arrows);
    xl_joystick(3 - settings.joystick, 0);
}

static bool keypad_joystick(uint8_t usage, bool pressed)
{
    static const uint8_t bits[] = {
        XL_JOY_DOWN | XL_JOY_LEFT, XL_JOY_DOWN, XL_JOY_DOWN | XL_JOY_RIGHT,
        XL_JOY_LEFT, XL_JOY_FIRE, XL_JOY_RIGHT,
        XL_JOY_UP | XL_JOY_LEFT, XL_JOY_UP, XL_JOY_UP | XL_JOY_RIGHT,
        XL_JOY_FIRE,                    /* 0 */
        XL_JOY_FIRE,                    /* . */
    };
    if (usage < 0x59 || usage > 0x63) {
        return false;
    }
    uint8_t b = bits[usage - 0x59];
    s_joy_pad = pressed ? s_joy_pad | b : s_joy_pad & ~b;
    joystick_update();
    return true;
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        /* LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
        switch (usage) {
        case 0xE0: s_pc_ctrl_l = pressed; break;
        case 0xE4: s_pc_ctrl_r = pressed; break;
        case 0xE1: s_pc_shift_l = pressed; break;
        case 0xE5: s_pc_shift_r = pressed; break;
        case 0xE2:
        case 0xE6:
            if (s_arrows_joystick) {
                s_joy_arrows = pressed ? s_joy_arrows | XL_JOY_FIRE : s_joy_arrows & ~XL_JOY_FIRE;
                joystick_update();
            }
            return;
        default:
            return;
        }
        modifiers_as_held();
        return;
    }
    if (!pressed) {
        release(usage);
        switch (usage) {
        case 0x3B: xl_key(XL_KEY_START, false); break;
        case 0x3C: xl_key(XL_KEY_SELECT, false); break;
        case 0x3D: xl_key(XL_KEY_OPTION, false); break;
        case 0x40: case 0x48: xl_key(XL_KEY_BREAK, false); break;
        default: break;
        }
        if (s_arrows_joystick && usage >= 0x4F && usage <= 0x52) {
            static const uint8_t dir[4] = { XL_JOY_RIGHT, XL_JOY_LEFT, XL_JOY_DOWN, XL_JOY_UP };
            s_joy_arrows &= ~dir[usage - 0x4F];
            joystick_update();
        }
        keypad_joystick(usage, false);
        return;
    }
    if (keypad_joystick(usage, true)) {
        return;
    }
    if (usage >= 0x04 && usage <= 0x1D) {
        press(usage, letters[usage - 0x04], -1, -1);
        return;
    }
    switch (usage) {
    case 0x28: case 0x58: press(usage, K_RETURN, -1, -1); return;
    case 0x29: press(usage, K_ESC, -1, -1); return;
    case 0x2A: press(usage, K_BACKSPACE, -1, -1); return;
    case 0x2B: press(usage, K_TAB, -1, -1); return;
    case 0x2C: press(usage, K_SPACE, -1, -1); return;
    case 0x39: press(usage, K_CAPS, -1, -1); return;
    case 0x35: case 0x4D: press(usage, K_INVERSE, -1, -1); return;    /* ` End */
    case 0x4C: press(usage, K_BACKSPACE, 0, 1); return;                /* Delete */
    case 0x49:                                                          /* Insert */
        press(usage, K_GREATER, (s_pc_shift_l || s_pc_shift_r) ? 1 : 0, (s_pc_shift_l || s_pc_shift_r) ? 0 : 1);
        return;
    case 0x4A: press(usage, K_LESS, 1, 0); return;                     /* Home: CLEAR */
    case 0x3A: press(usage, K_HELP, -1, -1); return;                   /* F1 */
    case 0x3B: xl_key(XL_KEY_START, true); return;
    case 0x3C: xl_key(XL_KEY_SELECT, true); return;
    case 0x3D: xl_key(XL_KEY_OPTION, true); return;
    case 0x3E: xl_warm_reset(); return;                                 /* F5: RESET */
    case 0x40: case 0x48: xl_key(XL_KEY_BREAK, true); return;          /* F7, Pause */
    case 0x44:                                                          /* F11: joystick port */
        xl_joystick(settings.joystick, 0);
        settings.joystick = 3 - settings.joystick;
        settings_save();
        joystick_update();
        ESP_LOGI(TAG, "joystick in port %d", settings.joystick);
        return;
    case 0x45:                                                          /* F12: cursor keys <-> joystick */
        s_arrows_joystick = !s_arrows_joystick;
        s_joy_arrows = 0;
        joystick_update();
        return;
    default:
        break;
    }
    if (usage >= 0x4F && usage <= 0x52) {
        if (s_arrows_joystick) {
            static const uint8_t dir[4] = { XL_JOY_RIGHT, XL_JOY_LEFT, XL_JOY_DOWN, XL_JOY_UP };
            s_joy_arrows |= dir[usage - 0x4F];
            joystick_update();
            return;
        }
        static const uint8_t arrow[4] = { K_STAR, K_PLUS, K_EQUALS, K_MINUS };    /* right left down up */
        press(usage, arrow[usage - 0x4F], 0, 1);
        return;
    }
    char c = 0;
    if (usage == 0x64) {
        c = (s_pc_shift_l || s_pc_shift_r) ? '|' : '\\';     /* the ISO key */
    } else if (usage < sizeof(us_plain)) {
        c = (s_pc_shift_l || s_pc_shift_r) ? us_shift[usage] : us_plain[usage];
    }
    if (c == '`') {
        press(usage, K_INVERSE, -1, -1);
        return;
    }
    bool shift;
    int key = c ? xl_ascii_key(c, &shift) : -1;
    if (key >= 0) {
        press(usage, key, shift, -1);
    }
}

/* ---- Setup screen ---- */

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= 0 && i < XL_MODEL_COUNT) {
        settings.model = i;
        settings_save();
    }
}

static int tv_get(void)
{
    return settings.tv;
}

static void tv_set(int i)
{
    if (i == XL_TV_PAL || i == XL_TV_NTSC) {
        settings.tv = i;
        settings_save();
    }
}

static int basic_get(void)
{
    return settings.basic;
}

static void basic_set(int i)
{
    if (i == 0 || i == 1) {
        settings.basic = i;
        settings_save();
    }
}

static const emu_choice_t choices[] = {
    { "Model", model_names, model_get, model_set, false },
    { "TV", tv_names, tv_get, tv_set, false },
    { "BASIC", basic_names, basic_get, basic_set, false },
};

/* ---- Drives D1: and D2: ---- */

static const char *const extensions[] = { ".atr", ".xfd", NULL };
static const char *const drive_names[] = { "D1:", "D2:" };
static const disks_size_t sizes[] = {
    { XL_DISK_SD_BYTES, "90 KB single density" },
    { XL_DISK_ED_BYTES, "130 KB enhanced density (1050)" },
    { XL_DISK_DD_BYTES, "180 KB double density" },
};

static const disks_config_t disks = {
    .folder = "atari",
    .extensions = extensions,
    .description = "Disk images (.ATR .XFD)",
    .title = "Disk drives",
    .button = "Disks",
    .nvs_namespace = "xldisks",
    .drives = XL_DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".atr",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 16 * 1024 * 1024,
    .insert = xl_disk_insert,
    .eject = xl_disk_eject,
    .path = xl_disk_path,
    .write_protected = xl_disk_write_protected,
    .create = xl_disk_create,
};

static void flush(void *arg)
{
    xl_disk_flush();
}

static void every_second(void)
{
    static int dirty_for;
    if (xl_disk_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        snprintf(text, sizeof(text), "%s, %s, BASIC %s", model_full[settings.model], tv_names[settings.tv],
                 s_cfg.basic ? "on" : "off");
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROMs", s_rom_text);
        snprintf(text, sizeof(text), "port %d on the numeric keypad%s (F11 switches)", settings.joystick,
                 s_arrows_joystick ? " and cursor keys" : "");
        emu_json_row(j, "Joystick", text);
        return;
    }
    xl_disk_status_t ds;
    xl_disk_status(&ds);
    for (int d = 0; d < XL_DRIVES; d++) {
        const char *name = disks_name(d);
        uint64_t free_bytes = 0, size = 0;
        xl_disk_space(d, &free_bytes, &size);
        snprintf(text, sizeof(text), "sector %u", ds.sector[d]);
        emu_json_drive_space(j, drive_names[d], name, xl_disk_write_protected(d), ds.light == d, ds.reads[d],
                             ds.writes[d], name && ds.sector[d] ? text : NULL, free_bytes, size);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    xl_status_t s;
    xl_get_status(&s);
    snprintf(buf, len, "frames %lu PC $%04X PORTB $%02X DMACTL $%02X DL $%04X", (unsigned long)s.frames, s.pc,
             s.portb, s.dmactl, s.dlist);
}

/*   k <hex>            tap a key (POKEY key code: 0C Return, 21 Space, 3F A)
 *   K <hex> <0|1>      key down (1) or up (0); 40 Shift, 41 Ctrl, 42 Break,
 *                      43 Start, 44 Select, 45 Option
 *   T <text>           type text (\n is Return)
 *   j <port> <bits>    joystick (1 up 2 down 4 left 8 right 16 fire)
 *   W                  Reset key (warm start)
 *   P <0|1>            serve the OS's SIO calls directly (fast disks)
 *   R <model> [tv]     restart as 0 800XL, 1 130XE; tv 0 PAL, 1 NTSC
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned key = strtoul(line + 1, NULL, 16);
        xl_key(key, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        xl_key(key, false);
        return true;
    }
    case 'K': {
        unsigned key = 0, down = 0;
        if (sscanf(line + 1, "%x %u", &key, &down) == 2) {
            xl_key(key, down);
        }
        return true;
    }
    case 'T': {
        char text[256];
        const char *s = line + 1;
        while (*s == ' ') {
            s++;
        }
        size_t n = 0;
        for (; *s && n < sizeof(text) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                text[n++] = '\n';
                s++;
            } else {
                text[n++] = *s;
            }
        }
        text[n] = 0;
        xl_type(text);
        return true;
    }
    case 'j': {
        int port = 1;
        unsigned bits = 0;
        sscanf(line + 1, "%d %u", &port, &bits);
        xl_joystick(port, bits);
        return true;
    }
    case 'W':
        xl_warm_reset();
        return true;
    case 'P':
        xl_set_sio_patch(atoi(line + 1) != 0);
        return true;
    case 'R': {
        int model = 0, tv = settings.tv;
        int n = sscanf(line + 1, "%d %d", &model, &tv);
        if (n >= 1 && model >= 0 && model < XL_MODEL_COUNT && (tv == XL_TV_PAL || tv == XL_TV_NTSC)) {
            settings.model = model;
            settings.tv = tv;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Atari 800XL/130XE",
    .short_name = "Atari",
    .view_width = XL_SCREEN_WIDTH * 5 / 2,  /* 384 x 240: 960 x 600, 3x on the Tab5 */
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = xl_reset,
    .run_frame = xl_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = xl_screen_signature,
    .render_line = xl_render_line_rgb565,
    .audio_hz = XL_AUDIO_HZ,
    .audio_enable = xl_audio_enable,
    .audio_render = xl_audio_render,
    .audio_pending = xl_audio_pending,
    .hid_key = hid_key,
    .choices = choices,
    .choice_count = sizeof(choices) / sizeof(choices[0]),
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
