/*
 * The SIO bus and disk drives D1: and D2:, 1050-like with double density
 * (an XF551's PERCOM block), after the Altirra Hardware Reference Manual
 * chapters 9 and 10.
 *
 * On the bus: the computer lowers the command line, sends the 5-byte
 * command frame (device, command, aux1, aux2, checksum) and raises it; the
 * drive answers ACK ('A') or NAK ('N') a millisecond later, takes a data
 * frame from the computer for writes (and ACKs it), then sends Complete
 * ('C') or Error ('E') and, for reads, the data frame. Bytes from the drive
 * take ten bits at 19040 baud and arrive at POKEY's serial input when
 * their stop bit is in. Commands: status, read, write, put, format (single
 * and enhanced density), read and write PERCOM block.
 *
 * The patch: when the OS's SIO routine is called for a drive (the
 * emulation stops at its entry), the transfer is done at once from the
 * device control block at $0300 and the routine returns as it would.
 */
#include <string.h>
#include "xl_internal.h"

bool xl_sio_log;                        /* log every command (debugging) */

#define CHK_ACK     0x41
#define CHK_NAK     0x4E
#define CHK_COMPLETE 0x43
#define CHK_ERROR   0x45

typedef enum { BUS_IDLE, BUS_COMMAND, BUS_DATA } bus_state_t;

typedef struct {
    uint8_t byte;
    uint64_t at;                        /* when it has arrived (its stop bit) */
} out_byte_t;

static struct {
    bool command;                       /* command line asserted */
    bus_state_t state;
    uint8_t frame[5];
    int frame_n;
    int drive;                          /* of the command in progress */
    uint8_t cmd;
    uint16_t aux;
    uint8_t data[516];                  /* data frame from the computer */
    int data_n, data_need;
    out_byte_t out[600];
    int out_n, out_pos;
    uint64_t out_last;                  /* the last queued byte arrives then */
    uint32_t byte_cycles;               /* one byte from a drive */
    uint8_t status[XL_DRIVES];          /* drive status byte 0 (error bits) */
    uint8_t percom[XL_DRIVES][12];
    uint64_t busy_until;
    int busy_drive;
} sio;

static uint32_t us(uint32_t microseconds)
{
    return (uint32_t)((uint64_t)xl.clock_hz * microseconds / 1000000);
}

static uint8_t checksum(const uint8_t *p, int n)
{
    unsigned s = 0;
    for (int i = 0; i < n; i++) {
        s += p[i];
        s = (s & 0xFF) + (s >> 8);
    }
    return (uint8_t)s;
}

void sio_reset(void)
{
    sio.command = false;
    sio.state = BUS_IDLE;
    sio.frame_n = 0;
    sio.out_n = sio.out_pos = 0;
    sio.out_last = 0;
    sio.byte_cycles = xl.clock_hz * 10 / 19040;
    sio.busy_until = 0;
    sio.busy_drive = -1;
    memset(sio.status, 0, sizeof(sio.status));
}

/* ---- Bytes from the drive ---- */

static void send(uint8_t byte, uint32_t gap)
{
    if (sio.out_n >= (int)(sizeof(sio.out) / sizeof(sio.out[0]))) {
        return;
    }
    uint64_t start = sio.out_last > xl_now() ? sio.out_last : xl_now();
    start += gap;
    sio.out_last = start + sio.byte_cycles;
    sio.out[sio.out_n].byte = byte;
    sio.out[sio.out_n].at = sio.out_last;
    sio.out_n++;
    xl_event_at(sio.out[sio.out_pos].at);
    sio.busy_until = sio.out_last;
}

static void send_frame(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        send(p[i], 0);
    }
    send(checksum(p, n), 0);
}

uint64_t sio_next_event(void)
{
    return sio.out_pos < sio.out_n ? sio.out[sio.out_pos].at : UINT64_MAX;
}

void sio_events(uint64_t now)
{
    while (sio.out_pos < sio.out_n && sio.out[sio.out_pos].at <= now) {
        pokey_serial_in(sio.out[sio.out_pos].byte);
        sio.out_pos++;
    }
    if (sio.out_pos == sio.out_n) {
        sio.out_pos = sio.out_n = 0;
    }
}

/* ---- The drives' commands ---- */

static disk_t *drive_disk(int drive)
{
    return drive >= 0 && drive < XL_DRIVES && xl_disks[drive].data ? &xl_disks[drive] : NULL;
}

/* Status frame: drive status, inverted FDC status, format timeout, 0 */
static void status_frame(int drive, uint8_t out[4])
{
    disk_t *d = drive_disk(drive);
    uint8_t s = drive >= 0 && drive < XL_DRIVES ? sio.status[drive] : 0;
    if (d) {
        if (d->sector_size == 256) {
            s |= 0x20;
        } else if (d->sectors == 1040) {
            s |= 0x80;
        }
    }
    out[0] = s;
    out[1] = d && d->read_only ? 0xBF : 0xFF;
    out[2] = 0xE0;
    out[3] = 0x00;
}

static void percom_frame(int drive, uint8_t out[12])
{
    disk_t *d = drive_disk(drive);
    uint16_t ss = d ? d->sector_size : 128;
    uint32_t sectors = d ? d->sectors : 720;
    uint16_t spt = sectors == 1040 ? 26 : 18;
    uint32_t tracks = sectors / spt;
    out[0] = tracks > 255 ? 255 : (uint8_t)(tracks ? tracks : 40);
    out[1] = 0;
    out[2] = spt >> 8;
    out[3] = spt & 0xFF;
    out[4] = 0;
    out[5] = (ss == 256 || sectors == 1040) ? 4 : 0;
    out[6] = ss >> 8;
    out[7] = ss & 0xFF;
    out[8] = 0xFF;
    out[9] = out[10] = out[11] = 0;
    if (sectors != 720 && sectors != 1040) {
        /* hard disk images: one "track" of all sectors */
        out[0] = 1;
        out[2] = sectors >> 8;
        out[3] = sectors & 0xFF;
    }
}

/* The PERCOM geometry chosen for the next format */
static void percom_geometry(int drive, uint16_t *ss, uint32_t *sectors)
{
    const uint8_t *p = sio.percom[drive];
    uint32_t spt = p[2] << 8 | p[3];
    uint32_t tracks = p[0] ? p[0] : 40;
    uint32_t sides = p[4] + 1;
    *ss = (p[6] << 8 | p[7]) == 256 ? 256 : 128;
    *sectors = tracks * spt * sides;
    if (*sectors < 3 || *sectors > 65535) {
        *sectors = 720;
    }
}

static int sector_len(disk_t *d, uint16_t sector)
{
    int len = 0;
    return disk_sector(d, sector, &len) ? len : 0;
}

static void busy(int drive)
{
    sio.busy_drive = drive;
}

/* A command frame for a drive, the command line just raised */
static void command(void)
{
    int drive = sio.drive;
    disk_t *d = drive_disk(drive);
    uint8_t cmd = sio.cmd & 0x7F;
    uint32_t ack = us(1000), complete = us(500);
    if (xl_sio_log) {
        XL_LOG("sio D%d: command $%02X aux %u", drive + 1, sio.cmd, sio.aux);
    }
    busy(drive);
    switch (cmd) {
    case 0x53: {                            /* status */
        uint8_t st[4];
        status_frame(drive, st);
        send(CHK_ACK, ack);
        send(CHK_COMPLETE, complete);
        send_frame(st, 4);
        sio.status[drive] = 0;
        break;
    }
    case 0x52: {                            /* read */
        int len = 0;
        uint8_t *p = disk_sector(d, sio.aux, &len);
        if (!p) {
            sio.status[drive] |= 0x01;
            send(CHK_NAK, ack);
            break;
        }
        d->reads++;
        d->last_sector = sio.aux;
        send(CHK_ACK, ack);
        send(CHK_COMPLETE, complete);
        send_frame(p, len);
        break;
    }
    case 0x50:                              /* put */
    case 0x57: {                            /* write */
        int len = sector_len(d, sio.aux);
        if (!len) {
            sio.status[drive] |= 0x01;
            send(CHK_NAK, ack);
            break;
        }
        send(CHK_ACK, ack);
        sio.state = BUS_DATA;
        sio.data_n = 0;
        sio.data_need = len + 1;
        return;
    }
    case 0x21:                              /* format (PERCOM geometry) */
    case 0x22: {                            /* format enhanced density */
        uint16_t ss = 128;
        uint32_t sectors = 1040;
        if (cmd == 0x21) {
            percom_geometry(drive, &ss, &sectors);
        }
        send(CHK_ACK, ack);
        if (d->read_only || !disk_format(d, ss, sectors)) {
            sio.status[drive] |= 0x08;
            uint8_t bad[256];
            memset(bad, 0xFF, sizeof(bad));
            send(CHK_ERROR, us(2000));
            send_frame(bad, ss);
            break;
        }
        uint8_t bad[256];
        memset(bad, 0xFF, sizeof(bad));
        send(CHK_COMPLETE, us(20000));
        send_frame(bad, ss);
        break;
    }
    case 0x4E: {                            /* read PERCOM block */
        uint8_t p[12];
        percom_frame(drive, p);
        send(CHK_ACK, ack);
        send(CHK_COMPLETE, complete);
        send_frame(p, 12);
        break;
    }
    case 0x4F:                              /* write PERCOM block */
        send(CHK_ACK, ack);
        sio.state = BUS_DATA;
        sio.data_n = 0;
        sio.data_need = 13;
        return;
    default:
        sio.status[drive] |= 0x01;
        send(CHK_NAK, ack);
        break;
    }
    sio.state = BUS_IDLE;
}

/* The data frame of a write is in */
static void data_frame(void)
{
    int drive = sio.drive;
    disk_t *d = drive_disk(drive);
    int n = sio.data_need - 1;
    sio.state = BUS_IDLE;
    if (checksum(sio.data, n) != sio.data[n]) {
        sio.status[drive] |= 0x02;
        send(CHK_NAK, us(1000));
        return;
    }
    send(CHK_ACK, us(1000));
    uint8_t cmd = sio.cmd & 0x7F;
    if (cmd == 0x4F) {
        memcpy(sio.percom[drive], sio.data, 12);
        send(CHK_COMPLETE, us(500));
        return;
    }
    int len = 0;
    uint8_t *p = d ? disk_sector(d, sio.aux, &len) : NULL;
    if (!p || d->read_only) {
        sio.status[drive] |= 0x04 | (d && d->read_only ? 0x08 : 0);
        send(CHK_ERROR, us(500));
        return;
    }
    memcpy(p, sio.data, len);
    d->dirty = true;
    d->writes++;
    d->last_sector = sio.aux;
    send(CHK_COMPLETE, us(cmd == 0x57 ? 2000 : 1000));
}

void sio_command_line(bool asserted)
{
    if (asserted && !sio.command) {
        sio.state = BUS_COMMAND;
        sio.frame_n = 0;
    } else if (!asserted && sio.command && sio.state == BUS_COMMAND) {
        sio.state = BUS_IDLE;
        if (sio.frame_n == 5 && checksum(sio.frame, 4) == sio.frame[4]) {
            int drive = sio.frame[0] - 0x31;
            if (drive_disk(drive)) {
                sio.drive = drive;
                sio.cmd = sio.frame[1];
                sio.aux = sio.frame[2] | sio.frame[3] << 8;
                sio.out_n = sio.out_pos = 0;
                command();
            }
        }
    }
    sio.command = asserted;
}

void sio_from_computer(uint8_t byte)
{
    if (sio.state == BUS_COMMAND && sio.command) {
        if (sio.frame_n < 5) {
            sio.frame[sio.frame_n++] = byte;
        }
    } else if (sio.state == BUS_DATA) {
        if (sio.data_n < (int)sizeof(sio.data)) {
            sio.data[sio.data_n++] = byte;
        }
        if (sio.data_n == sio.data_need) {
            data_frame();
        }
    }
}

int sio_busy_drive(void)
{
    return xl_now() < sio.busy_until + us(100000) ? sio.busy_drive : -1;
}

/* ---- The patch ---- */

static void put_byte(uint16_t a, uint8_t v)
{
    uint8_t *p = xl.cpu.wr[a >> 8];
    if (p) {
        p[a & 0xFF] = v;
    }
}

bool sio_patch(m6502_t *cpu)
{
    uint8_t dev = xl_peek(0x300) + xl_peek(0x301) - 1;
    int drive = dev - 0x31;
    /* Nothing else answers on this bus: other devices (printers, the 850,
     * the OS's handler polls) time out at once instead of after retries
     */
    disk_t *d = drive >= 0 && drive < XL_DRIVES ? drive_disk(drive) : NULL;
    uint8_t cmd = xl_peek(0x302) & 0x7F;
    uint16_t buf = xl_peek(0x304) | xl_peek(0x305) << 8;
    uint16_t bytes = xl_peek(0x308) | xl_peek(0x309) << 8;
    uint16_t aux = xl_peek(0x30A) | xl_peek(0x30B) << 8;
    uint8_t result = 0x01;
    if (xl_sio_log) {
        XL_LOG("sio patch: device $%02X command $%02X aux %u bytes %u buffer $%04X", dev, cmd, aux, bytes, buf);
    }
    if (!d) {
        result = 0x8A;                      /* timeout: no drive */
    } else {
        busy(drive);
        sio.busy_until = xl_now() + us(20000);
        switch (cmd) {
        case 0x52: {                        /* read */
            int len = 0;
            uint8_t *p = disk_sector(d, aux, &len);
            if (!p) {
                result = 0x8B;              /* NAK */
                break;
            }
            for (int i = 0; i < bytes && i < len; i++) {
                put_byte(buf + i, p[i]);
            }
            d->reads++;
            d->last_sector = aux;
            break;
        }
        case 0x50:
        case 0x57: {                        /* put, write */
            int len = 0;
            uint8_t *p = disk_sector(d, aux, &len);
            if (!p) {
                result = 0x8B;
                break;
            }
            if (d->read_only) {
                sio.status[drive] |= 0x0C;
                result = 0x90;              /* device error */
                break;
            }
            for (int i = 0; i < bytes && i < len; i++) {
                p[i] = xl_peek(buf + i);
            }
            d->dirty = true;
            d->writes++;
            d->last_sector = aux;
            break;
        }
        case 0x53: {                        /* status */
            uint8_t st[4];
            status_frame(drive, st);
            for (int i = 0; i < 4 && i < bytes; i++) {
                put_byte(buf + i, st[i]);
            }
            sio.status[drive] = 0;
            break;
        }
        case 0x21:
        case 0x22: {                        /* format */
            uint16_t ss = 128;
            uint32_t sectors = 1040;
            if (cmd == 0x21) {
                percom_geometry(drive, &ss, &sectors);
            }
            if (d->read_only || !disk_format(d, ss, sectors)) {
                result = 0x90;
                break;
            }
            for (int i = 0; i < bytes; i++) {
                put_byte(buf + i, 0xFF);
            }
            break;
        }
        case 0x4E: {
            uint8_t p[12];
            percom_frame(drive, p);
            for (int i = 0; i < 12 && i < bytes; i++) {
                put_byte(buf + i, p[i]);
            }
            break;
        }
        case 0x4F:
            for (int i = 0; i < 12; i++) {
                sio.percom[drive][i] = xl_peek(buf + i);
            }
            break;
        default:
            result = 0x8B;
            break;
        }
    }
    /* back to the caller as the OS routine would: status in DSTATS and Y, N
     * flag set on an error
     */
    put_byte(0x0303, result);
    put_byte(0x0030, result);
    put_byte(0x0042, 0);                    /* CRITIC */
    cpu->y = result;
    cpu->p = (cpu->p & ~(M6502_N | M6502_Z)) | (result & 0x80 ? M6502_N : 0);
    uint8_t s = cpu->s;
    uint16_t ret = xl_peek(0x100 + (uint8_t)(s + 1)) | xl_peek(0x100 + (uint8_t)(s + 2)) << 8;
    cpu->s = s + 2;
    cpu->pc = ret + 1;
    return true;
}
