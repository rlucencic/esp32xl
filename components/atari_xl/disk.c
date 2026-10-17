/*
 * Disk images: .ATR (16-byte header: $96 $02, size in 16-byte paragraphs,
 * sector size) and .XFD (raw sectors, the geometry from the size).
 * 256-byte sector images keep sectors 1-3 as 128 bytes, either packed
 * (3 x 128 + n x 256) or each in a 256-byte slot.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "xl_internal.h"

#define MAX_IMAGE   (16u * 1024 * 1024)

disk_t xl_disks[XL_DRIVES];

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool zero(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

/* Geometry from the data size and sector size. A 256-byte image the size
 * of whole sectors gives the boot sectors three full slots (sector 4 at
 * 768): each in its own slot when the slots' second halves are empty,
 * else packed into the first 384 bytes.
 */
static bool geometry(disk_t *d, const uint8_t *data, uint32_t size, uint16_t sector_size)
{
    d->sector_size = sector_size;
    d->boot_stride = 128;
    d->data_start = 384;
    if (sector_size == 128) {
        d->sectors = size / 128;
    } else if (size >= 384 && (size - 384) % sector_size == 0) {
        d->sectors = 3 + (size - 384) / sector_size;      /* boot sectors packed */
    } else if (size % sector_size == 0 && size >= 3u * sector_size) {
        d->sectors = size / sector_size;
        d->data_start = 3 * sector_size;
        if (zero(data + 128, sector_size - 128) && zero(data + sector_size + 128, sector_size - 128) &&
            zero(data + 2 * sector_size + 128, sector_size - 128)) {
            d->boot_stride = sector_size;
        }
    } else {
        return false;
    }
    return d->sectors >= 3;
}

bool disk_load(disk_t *d, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (uint32_t)size > MAX_IMAGE) {
        fclose(f);
        return false;
    }
    uint8_t hdr[16];
    bool atr = false;
    uint32_t data_size = (uint32_t)size;
    uint16_t sector_size = 128;
    if (size >= 16 && fread(hdr, 1, 16, f) == 16 && hdr[0] == 0x96 && hdr[1] == 0x02) {
        atr = true;
        uint32_t paragraphs = hdr[2] | hdr[3] << 8 | (uint32_t)hdr[6] << 16;
        data_size = paragraphs * 16;
        sector_size = hdr[4] | hdr[5] << 8;
        if (data_size > (uint32_t)size - 16) {
            data_size = (uint32_t)size - 16;
        }
    } else {
        if (has_ext(path, ".atr")) {
            fclose(f);
            return false;               /* an .ATR without its header */
        }
        fseek(f, 0, SEEK_SET);
        /* .XFD: 256-byte sectors if it's the size of such a disk */
        if (size == 183936 || size == 184320) {
            sector_size = 256;
        }
    }
    if (sector_size != 128 && sector_size != 256 && sector_size != 512) {
        fclose(f);
        return false;
    }
    uint8_t *data = xl_malloc(data_size ? data_size : 1);
    if (!data) {
        fclose(f);
        return false;
    }
    if (fread(data, 1, data_size, f) != data_size) {
        free(data);
        fclose(f);
        return false;
    }
    fclose(f);
    disk_unload(d);
    if (!geometry(d, data, data_size, sector_size)) {
        free(data);
        return false;
    }
    d->data = data;
    d->size = data_size;
    d->capacity = data_size;
    d->atr = atr;
    d->dirty = false;
    d->reads = d->writes = 0;
    d->last_sector = 0;
    snprintf(d->path, sizeof(d->path), "%s", path);
    FILE *w = fopen(path, "r+b");
    d->read_only = w == NULL;
    if (w) {
        fclose(w);
    }
    XL_LOG("disk %s: %lu sectors of %u%s%s", path, (unsigned long)d->sectors, d->sector_size,
           d->data_start != 384 ? (d->boot_stride == 128 ? " (boot sectors packed in 768)" : " (boot sectors padded)") : "",
           d->read_only ? ", read-only" : "");
    return true;
}

void disk_unload(disk_t *d)
{
    free(d->data);
    d->data = NULL;
    d->size = d->capacity = 0;
    d->sectors = 0;
    d->path[0] = 0;
    d->dirty = false;
}

uint8_t *disk_sector(disk_t *d, uint32_t sector, int *len)
{
    if (!d->data || sector < 1 || sector > d->sectors) {
        return NULL;
    }
    uint32_t offset;
    if (sector <= 3) {
        offset = (sector - 1) * d->boot_stride;
        *len = 128;
    } else {
        offset = d->data_start + (sector - 4) * d->sector_size;
        *len = d->sector_size;
    }
    if (offset + *len > d->size) {
        return NULL;
    }
    return d->data + offset;
}

bool disk_format(disk_t *d, uint16_t sector_size, uint32_t sectors)
{
    uint32_t size = sector_size == 128 ? sectors * 128 : 384 + (sectors - 3) * sector_size;
    if (size > d->capacity) {
        uint8_t *data = xl_malloc(size);
        if (!data) {
            return false;
        }
        free(d->data);
        d->data = data;
        d->capacity = size;
    }
    memset(d->data, 0, size);
    d->size = size;
    d->sector_size = sector_size;
    d->sectors = sectors;
    d->boot_stride = 128;
    d->data_start = 384;
    d->dirty = true;
    return true;
}

static void atr_header(uint8_t *hdr, uint32_t size, uint16_t sector_size)
{
    uint32_t paragraphs = size / 16;
    memset(hdr, 0, 16);
    hdr[0] = 0x96;
    hdr[1] = 0x02;
    hdr[2] = paragraphs & 0xFF;
    hdr[3] = (paragraphs >> 8) & 0xFF;
    hdr[4] = sector_size & 0xFF;
    hdr[5] = sector_size >> 8;
    hdr[6] = (paragraphs >> 16) & 0xFF;
}

bool disk_flush(disk_t *d)
{
    if (!d->data || !d->dirty || d->read_only) {
        return true;
    }
    FILE *f = fopen(d->path, "wb");
    if (!f) {
        return false;
    }
    bool ok = true;
    if (d->atr) {
        uint8_t hdr[16];
        atr_header(hdr, d->size, d->sector_size);
        ok = fwrite(hdr, 1, 16, f) == 16;
    }
    ok = ok && fwrite(d->data, 1, d->size, f) == d->size;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        d->dirty = false;
    }
    return ok;
}

bool xl_disk_create(const char *path, uint32_t bytes)
{
    uint16_t sector_size;
    uint32_t sectors;
    if (bytes == XL_DISK_SD_BYTES) {
        sector_size = 128;
        sectors = 720;
    } else if (bytes == XL_DISK_ED_BYTES) {
        sector_size = 128;
        sectors = 1040;
    } else if (bytes == XL_DISK_DD_BYTES) {
        sector_size = 256;
        sectors = 720;
    } else {
        return false;
    }
    uint32_t size = sector_size == 128 ? sectors * 128 : 384 + (sectors - 3) * 256;
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    uint8_t hdr[16];
    atr_header(hdr, size, sector_size);
    bool ok = fwrite(hdr, 1, 16, f) == 16;
    static const uint8_t zero[256];
    for (uint32_t n = 0; ok && n < size; n += 256) {
        uint32_t k = size - n < 256 ? size - n : 256;
        ok = fwrite(zero, 1, k, f) == k;
    }
    return fclose(f) == 0 && ok;
}
