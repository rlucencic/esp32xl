/*
 * Free space on the disks in D1: and D2:, for the web page: the free
 * sectors their DOS counts (Atari DOS 2, MyDOS, SpartaDOS: esp32emu's
 * diskspace), read through disk_sector() whatever the image's layout.
 *
 * Worked out at insert and after writes are flushed (both with the
 * emulation stopped), kept for the web page's task.
 */
#include <string.h>
#include "xl_internal.h"
#include "diskspace.h"

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
} space_t;

static space_t s_space[XL_DRIVES];

/* Bytes from sector 1 on, the boot sectors packed (3 x 128) */
static bool disk_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    disk_t *d = ctx;
    uint8_t *out = buf;
    while (len > 0) {
        uint32_t sector, at;
        if (d->sector_size == 128 || offset < 384) {
            sector = (uint32_t)(offset / 128) + 1;
            at = (uint32_t)(offset % 128);
        } else {
            sector = (uint32_t)((offset - 384) / d->sector_size) + 4;
            at = (uint32_t)((offset - 384) % d->sector_size);
        }
        int n;
        const uint8_t *p = disk_sector(d, sector, &n);
        if (p == NULL || at >= (uint32_t)n) {
            return false;
        }
        uint32_t k = (uint32_t)n - at < len ? (uint32_t)n - at : len;
        memcpy(out, p + at, k);
        out += k;
        offset += k;
        len -= k;
    }
    return true;
}

void space_update(int drive)
{
    if (drive < 0 || drive >= XL_DRIVES) {
        return;
    }
    disk_t *disk = &xl_disks[drive];
    diskspace_t s = { 0 };
    bool known = false;
    if (disk->data != NULL && disk->sectors > 3) {
        uint64_t size = disk->sector_size == 128 ? (uint64_t)disk->sectors * 128
                                                 : 384 + (uint64_t)(disk->sectors - 3) * disk->sector_size;
        diskspace_disk_t d = { disk_read, disk, size };
        known = diskspace_atari(&d, disk->sector_size, &s);
    }
    s_space[drive].known = false;
    s_space[drive].free = s.free;
    s_space[drive].size = s.size;
    s_space[drive].known = known;
}

bool xl_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= XL_DRIVES || !s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}
