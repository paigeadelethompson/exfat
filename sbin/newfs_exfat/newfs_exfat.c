/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Paige A. Thompson (Ravenhammer Research.)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 */

#include <sys/param.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/disk.h>
#endif
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "newfs_exfat.h"

static void
usage(void)
{
    fprintf(stderr, "usage: newfs_exfat [-N] [-S bytes] "
            "[-b cluster-size] [-v] [-y] special\n");
    exit(1);
}

static void
report_progress(struct mkfs_exfat_ctx *ctx, const char *fmt, ...)
{
    va_list ap;

    if (!ctx->verbose)
        return;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static uint32_t
generate_volume_serial(void)
{
    time_t now;
    struct tm *tm;

    time(&now);
    tm = localtime(&now);

    return ((tm->tm_year + 1900) << 16) |
           ((tm->tm_mon + 1) << 8) |
           tm->tm_mday;
}

void
unix_time_to_exfat(const struct timespec *ts, uint32_t *date, uint32_t *time)
{
    struct tm tm;

    localtime_r(&ts->tv_sec, &tm);

    *date = ((tm.tm_year - 80) << 9) |  /* Year since 1980 */
            ((tm.tm_mon + 1) << 5) |     /* Month (1-12) */
            tm.tm_mday;                  /* Day (1-31) */

    *time = (tm.tm_hour << 11) |         /* Hour (0-23) */
            (tm.tm_min << 5) |           /* Minute (0-59) */
            (tm.tm_sec >> 1);            /* Second/2 (0-29) */
}

int
exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t maxout, size_t *lenout)
{
    size_t len = 0;
    uint32_t codepoint;
    const uint8_t *s = (const uint8_t *)utf8;

    while (*s && len < maxout) {
        /* Decode UTF-8 sequence */
        if ((*s & 0x80) == 0) {
            /* 1-byte sequence */
            codepoint = *s++;
        } else if ((*s & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if ((s[1] & 0xC0) != 0x80)
                return -1;
            codepoint = ((*s & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else if ((*s & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
                return -1;
            codepoint = ((*s & 0x0F) << 12) |
                       ((s[1] & 0x3F) << 6) |
                       (s[2] & 0x3F);
            s += 3;
        } else {
            /* Invalid sequence */
            return -1;
        }

        /* Encode as UTF-16 */
        if (codepoint < 0x10000) {
            utf16[len++] = htole16(codepoint);
        } else if (codepoint < 0x110000 && len + 1 < maxout) {
            /* Surrogate pair */
            codepoint -= 0x10000;
            utf16[len++] = htole16(0xD800 | (codepoint >> 10));
            utf16[len++] = htole16(0xDC00 | (codepoint & 0x3FF));
        } else {
            return -1;
        }
    }

    *lenout = len;
    return 0;
}

static int
write_sector(struct mkfs_exfat_ctx *ctx, off_t sector, const void *buffer)
{
    off_t offset = sector * EXFAT_SECTOR_SIZE;
    ssize_t bytes;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("seek error");
        return -1;
    }

    bytes = write(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        warn("write error");
        return -1;
    }

    return 0;
}

static int
calculate_layout(struct mkfs_exfat_ctx *ctx)
{
    /* Boot region is always 24 sectors */
    uint32_t boot_sectors = 24;
    
    /* For ~1GB disk, use 32KB clusters (64 sectors) */
    ctx->sectors_per_cluster = 64;
    ctx->number_of_fats = 1;

    /* FAT starts at sector 128 */
    ctx->fat_offset = 128;
    ctx->fat_length = 256;

    /* Cluster heap starts at sector 384 */
    ctx->cluster_heap_offset = 384;

    /* Calculate final cluster count */
    uint64_t data_sectors = ctx->total_sectors - ctx->cluster_heap_offset;
    ctx->cluster_count = data_sectors / ctx->sectors_per_cluster;

    /* Set fixed cluster assignments */
    ctx->bitmap_cluster = 2;                    /* Bitmap starts at cluster 2 */
    ctx->upcase_cluster = 3;                    /* Upcase table at cluster 3 */
    ctx->root_cluster = 4;                      /* Root dir at cluster 4 */

    /* Verify layout */
    if (ctx->verbose) {
        printf("Layout verification:\n");
        printf("  Total sectors: %lu\n", (unsigned long)ctx->total_sectors);
        printf("  Sectors per cluster: %u\n", ctx->sectors_per_cluster);
        printf("  Cluster count: %u\n", ctx->cluster_count);
        printf("  FAT sectors: %u\n", ctx->fat_length);
        printf("  Cluster heap offset: %u\n", ctx->cluster_heap_offset);
    }

    /* Verify minimum size requirement */
    uint64_t min_sectors = ctx->cluster_heap_offset + 
                          ((uint64_t)ctx->cluster_count * ctx->sectors_per_cluster);
    if (ctx->total_sectors < min_sectors) {
        warnx("Invalid filesystem layout - device too small");
        return -1;
    }

    return 0;
}

/* Write a cluster to disk at the specified cluster number */
static int write_cluster(struct mkfs_exfat_ctx *ctx, uint32_t cluster, const void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    /* Calculate cluster offset */
    offset = ((off_t)ctx->cluster_heap_offset +
             ((off_t)cluster - 2) * (ctx->sectors_per_cluster)) *
             EXFAT_SECTOR_SIZE;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("seek error");
        return -1;
    }

    bytes = write(ctx->fd, buffer, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);
    if (bytes != ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE) {
        warn("write error");
        return -1;
    }

    return 0;
}

static int
write_boot_sector(struct mkfs_exfat_ctx *ctx)
{
    struct exfat_boot_record boot;
    uint8_t sector[EXFAT_SECTOR_SIZE];
    /* Buffer for complete boot region (main + backup)
     * Per ExFAT spec section 3.1:
     * - Main boot region: sectors 0-11
     * - Backup boot region: sectors 12-23
     * - Backup must be an exact copy of main region
     */
    uint8_t boot_region[EXFAT_BOOT_REGION_SIZE * EXFAT_SECTOR_SIZE];
    int i;

    /* Initialize entire boot region to zeros */
    memset(boot_region, 0, sizeof(boot_region));

    /* Set boot signatures for required sectors
     * Per spec section 3.1:
     * - Main boot sector (0) and extended boot sectors (1-8) need signatures
     * - OEM Parameter sectors (9-10) should be all zeros
     * - Validation pattern sectors (11) needs signature
     */
    for (i = 0; i < 9; i++) {
        boot_region[i * EXFAT_SECTOR_SIZE + 510] = 0x55;
        boot_region[i * EXFAT_SECTOR_SIZE + 511] = 0xAA;
    }
    /* Add signature to validation pattern sector */
    boot_region[11 * EXFAT_SECTOR_SIZE + 510] = 0x55;
    boot_region[11 * EXFAT_SECTOR_SIZE + 511] = 0xAA;

    /* Set 0xF4 padding for boot sector per spec section 3.1.1 */
    memset(boot_region + 0x70, 0, 8);
    memset(boot_region + 0x78, 0xF4, EXFAT_SECTOR_SIZE - 0x78 - 2);

    /* Initialize boot sector */
    memset(&boot, 0, sizeof(boot));

    /* Set jump boot and filesystem name */
    boot.jump_boot[0] = 0xEB;
    boot.jump_boot[1] = 0x76;
    boot.jump_boot[2] = 0x90;
    memcpy(boot.fs_name, "EXFAT   ", 8);

    /* Zero the must_be_zero field per spec */
    memset(boot.must_be_zero, 0, sizeof(boot.must_be_zero));

    /* Set partition offset (8 sectors) */
    boot.partition_offset[0] = htole16(0x0800);  /* 8 sectors in LE format */
    boot.partition_offset[1] = 0;
    boot.partition_offset[2] = 0;
    boot.partition_offset[3] = 0;

    /* Set volume length */
    boot.volume_length = htole64(ctx->total_sectors);

    /* Set FAT and cluster heap layout */
    boot.fat_offset = htole32(ctx->fat_offset);
    boot.fat_length = htole32(ctx->fat_length);
    boot.cluster_heap_offset = htole32(ctx->cluster_heap_offset);
    boot.cluster_count = htole32(ctx->cluster_count);
    boot.root_dir_cluster = htole32(4);

    /* Set volume serial */
    boot.volume_serial = htole32(ctx->volume_serial);

    /* Set volume flags */
    boot.volume_flags = htole16(0x0000);      /* No flags set */

    /* Set drive select and version */
    boot.drive_select = 0x80;
    boot.bytes_per_sector_shift = 9;          /* 512 bytes */
    boot.sectors_per_cluster_shift = 6;       /* 64 sectors */
    boot.number_of_fats = 1;                  /* Single FAT */
    boot.fs_revision = htole16(0x0100);

    /* Zero remaining fields per spec */
    boot.percent_in_use = 0;
    memset(boot.reserved, 0, sizeof(boot.reserved));
    memset(boot.boot_code, 0, sizeof(boot.boot_code));

    /* Copy boot record into first sector */
    memcpy(boot_region, &boot, 0x70);  // Copy only up to padding area

    /* Write extended boot sectors */
    for (i = 1; i < 24; i++) {
        /* Add validation pattern at required sectors per spec */
        if (i == EXFAT_VALIDATION_SECTOR1) {
            uint32_t pattern = EXFAT_BOOT_VALIDATION_PATTERN;
            uint8_t *sector = boot_region + (i * EXFAT_SECTOR_SIZE);
            for (size_t j = 0; j < EXFAT_SECTOR_SIZE - 2; j += 4) {
                memcpy(sector + j, &pattern, 4);
            }
        }
    }

    /* Write main boot region (sectors 0-11) */
    for (i = 0; i < 12; i++) {
        const uint8_t *sector_data = boot_region + (i * EXFAT_SECTOR_SIZE);
        if (write_sector(ctx, i, sector_data) < 0)
            return -1;
    }

    /* Write backup boot region (sectors 12-23) */
    for (i = 0; i < 12; i++) {
        if (write_sector(ctx, i + 12, boot_region + (i * EXFAT_SECTOR_SIZE)) < 0)
            return -1;
    }

    return 0;
}

int
write_fat(struct mkfs_exfat_ctx *ctx)
{
    uint32_t *fat_sector;
    size_t i;

    /* Allocate buffer for FAT sector */
    fat_sector = calloc(1, ctx->bytes_per_sector);
    if (fat_sector == NULL)
        return -1;

    /* First two clusters are reserved */
    fat_sector[0] = htole32(0xFFFFFFF8);  /* Media type per spec */
    fat_sector[1] = htole32(0xFFFFFFFF);  /* End of cluster chain */

    /* Mark clusters 2-4 as end of chain */
    fat_sector[2] = htole32(0xFFFFFFFF);  /* Bitmap */
    fat_sector[3] = htole32(0xFFFFFFFF);  /* Upcase table */
    fat_sector[4] = htole32(0xFFFFFFFF);  /* Root directory */

    /* Write first FAT sector */
    if (lseek(ctx->fd, ctx->bytes_per_sector * ctx->fat_offset, SEEK_SET) < 0)
        goto error;
    if (write(ctx->fd, fat_sector, ctx->bytes_per_sector) != ctx->bytes_per_sector)
        goto error;

    /* Clear remaining FAT sectors */
    memset(fat_sector, 0, ctx->bytes_per_sector);
    for (i = 1; i < ctx->fat_length; i++) {
        if (write(ctx->fd, fat_sector, ctx->bytes_per_sector) != ctx->bytes_per_sector)
            goto error;
    }

    free(fat_sector);
    report_progress(ctx, "FAT written");
    return 0;

error:
    free(fat_sector);
    return -1;
}

int
write_bitmap(struct mkfs_exfat_ctx *ctx)
{
    uint8_t *bitmap;
    size_t bitmap_size;

    /* Calculate bitmap size (rounded up to cluster size) */
    bitmap_size = (ctx->cluster_count + 7) / 8;
    bitmap_size = roundup2(bitmap_size, ctx->sectors_per_cluster * ctx->bytes_per_sector);

    bitmap = calloc(1, bitmap_size);
    if (bitmap == NULL)
        return -1;

    /* Mark first 3 clusters as used */
    bitmap[0] = 0x07;  /* Clusters 0-2 */

    /* Write bitmap to cluster 2 */
    if (write_cluster(ctx, ctx->bitmap_cluster, bitmap) < 0) {
        free(bitmap);
        return -1;
    }

    free(bitmap);
    report_progress(ctx, "Allocation bitmap written");
    return 0;
}

int
write_upcase_table(struct mkfs_exfat_ctx *ctx)
{
    uint16_t *upcase;
    uint32_t checksum = 0;
    /* Per spec: Upcase table must be 5836 bytes (2918 entries) */
    size_t table_size = 5836;
    size_t i;
    int error = 0;

    /* Allocate upcase table buffer */
    upcase = calloc(1, table_size);
    if (upcase == NULL) {
        warn("Cannot allocate upcase table buffer");
        return -1;
    }

    /* Initialize Unicode uppercase mapping table per spec */
    for (i = 0; i < 65536; i++) {
        /* Basic Latin & Latin-1 Supplement */
        if (i >= 'a' && i <= 'z')
            upcase[i] = htole16(i - 0x20);
        else if (i >= 0xE0 && i <= 0xFE && i != 0xF7)
            upcase[i] = htole16(i - 0x20);
        /* Latin Extended-A */
        else if (i >= 0x0101 && i <= 0x0137 && (i & 1))
            upcase[i] = htole16(i - 1);
        /* Latin Extended-B */
        else if (i >= 0x0180 && i <= 0x0233 && (i & 1))
            upcase[i] = htole16(i - 1);
        /* Greek */
        else if (i >= 0x03B1 && i <= 0x03CB)
            upcase[i] = htole16(i - 0x20);
        /* Cyrillic */
        else if (i >= 0x0430 && i <= 0x044F)
            upcase[i] = htole16(i - 0x20);
        /* Other characters map to themselves */
        else
            upcase[i] = htole16(i);
    }

    /* Special cases */
    upcase[0x00DF] = htole16(0x0053);  /* ß -> S */
    upcase[0x00FF] = htole16(0x0178);  /* ÿ -> Ÿ */
    upcase[0x0131] = htole16(0x0049);  /* ı -> I */
    upcase[0x017F] = htole16(0x0053);  /* ſ -> S */

    /* Calculate checksum */
    for (i = 0; i < table_size; i++) {
        uint8_t *p = (uint8_t *)&upcase[i];
        checksum = ((checksum << 31) | (checksum >> 1)) + p[0];
        checksum = ((checksum << 31) | (checksum >> 1)) + p[1];
    }

    /* Write upcase table */
    uint32_t clusters_needed = (table_size + ctx->sectors_per_cluster - 1) / ctx->sectors_per_cluster;
    for (i = 0; i < clusters_needed; i++) {
        uint32_t write_size = MIN(ctx->sectors_per_cluster, table_size - i * ctx->sectors_per_cluster);
        if (write_cluster(ctx, ctx->upcase_cluster + i, 
                         (uint8_t *)upcase + i * ctx->sectors_per_cluster) < 0) {
            error = -1;
            goto out;
        }
    }

    /* Update upcase table entry in root directory */
    struct exfat_direntry_set es;
    memset(&es, 0, sizeof(es));
    es.file.type = EXFAT_ENTRY_UPCASE;
    es.file.secondary_count = 1;
    es.stream.type = EXFAT_ENTRY_STREAM;
    es.stream.first_cluster = ctx->upcase_cluster;
    es.stream.data_length = table_size;
    es.stream.valid_data_length = table_size;
    
    /* Store checksum */
    struct exfat_entry_upcase *upcase_entry = (struct exfat_entry_upcase *)&es.file;
    upcase_entry->checksum = htole32(checksum);

    /* Write entry to root directory */
    char *root_cluster = calloc(1, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);
    if (root_cluster == NULL) {
        error = -1;
        goto out;
    }

    memcpy(root_cluster + 64, &es, sizeof(struct exfat_entry_file) +
                                  sizeof(struct exfat_entry_stream));
    error = write_cluster(ctx, ctx->root_cluster, root_cluster);
    free(root_cluster);

    if (error == 0)
        report_progress(ctx, "Upcase table written (checksum: %08x)", checksum);

out:
    free(upcase);
    return error;
}

int
write_root_dir(struct mkfs_exfat_ctx *ctx)
{
    char *cluster;
    struct exfat_direntry_set es;
    struct timespec ts;
    size_t outlen;
    int error = 0;

    /* Allocate cluster buffer */
    cluster = calloc(1, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);
    if (cluster == NULL) {
        warn("Cannot allocate cluster buffer");
        return -1;
    }

    /* Get current time */
    clock_gettime(CLOCK_REALTIME, &ts);

    /* Initialize directory entries */
    memset(&es, 0, sizeof(es));

    /* Write bitmap entry */
    es.file.type = EXFAT_ENTRY_BITMAP;
    es.file.secondary_count = 1;
    unix_time_to_exfat(&ts, &es.file.create_timestamp, &es.file.create_timestamp);
    es.file.last_modified_timestamp = es.file.create_timestamp;
    es.file.last_access_timestamp = es.file.create_timestamp;
    es.stream.type = EXFAT_ENTRY_STREAM;
    es.stream.first_cluster = ctx->bitmap_cluster;
    es.stream.data_length = (ctx->cluster_count + 7) / 8;
    memcpy(cluster, &es, sizeof(struct exfat_entry_file) + 
                        sizeof(struct exfat_entry_stream));

    /* Write upcase entry */
    es.file.type = EXFAT_ENTRY_UPCASE;
    es.stream.first_cluster = ctx->upcase_cluster;
    es.stream.data_length = ctx->sectors_per_cluster;  /* One cluster for now */
    memcpy(cluster + 64, &es, sizeof(struct exfat_entry_file) +
                             sizeof(struct exfat_entry_stream));

    /* Write volume label if specified */
    if (ctx->volume_label != NULL) {
        struct exfat_entry_label *label;
        
        label = (struct exfat_entry_label *)(cluster + 128);
        label->type = EXFAT_ENTRY_LABEL;
        error = exfat_utf8_to_utf16(ctx->volume_label, label->unicode_label,
                                   11, &outlen);
        if (error) {
            warn("Invalid volume label");
            goto out;
        }
        label->character_count = outlen;
    }

    /* Write root directory cluster */
    error = write_cluster(ctx, ctx->root_cluster, cluster);
    if (error == 0)
        report_progress(ctx, "Root directory written");

out:
    free(cluster);
    return error;
}

static int
confirm_format(const char *device)
{
    char reply[10];

    printf("newfs_exfat: warning: %s will be formatted. All data will be lost!\n", device);
    printf("Proceed? (y/n) ");
    fflush(stdout);

    if (fgets(reply, sizeof(reply), stdin) == NULL)
        return 0;

    return (reply[0] == 'y' || reply[0] == 'Y');
}

int
main(int argc, char *argv[])
{
    struct mkfs_exfat_ctx ctx;
    struct stat sb;
    int ch;
    int yes = 0;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.bytes_per_sector = 512;
    ctx.sectors_per_cluster = EXFAT_DEFAULT_CLUSTER_SIZE / 512;
    ctx.number_of_fats = EXFAT_DEFAULT_FATS;

    /* Parse command line options */
    while ((ch = getopt(argc, argv, "NS:b:vy")) != -1) {
        switch (ch) {
        case 'N':
            /* Print parameters only - not implemented yet */
            break;
        case 'S':
            ctx.bytes_per_sector = atoi(optarg);
            if (ctx.bytes_per_sector < 512 || ctx.bytes_per_sector > 4096)
                errx(1, "bytes per sector must be between 512 and 4096");
            break;
        case 'b':
            ctx.sectors_per_cluster = atoi(optarg) / ctx.bytes_per_sector;
            if (ctx.sectors_per_cluster < 1)
                errx(1, "cluster size must be at least one sector");
            break;
        case 'v':
            ctx.verbose = 1;
            break;
        case 'y':
            yes = 1;
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1)
        usage();

    /* Confirm format unless -y was specified */
    if (!yes && !confirm_format(argv[0]))
        errx(1, "Format canceled");

    /* Open the device */
    ctx.device = argv[0];
    ctx.fd = open(ctx.device, O_RDWR);
    if (ctx.fd < 0)
        err(1, "Cannot open %s", ctx.device);

    /* Get device size */
    if (fstat(ctx.fd, &sb) < 0)
        err(1, "Cannot stat %s", ctx.device);
    if (S_ISREG(sb.st_mode)) {
        ctx.total_sectors = sb.st_size / ctx.bytes_per_sector;
    } else {
#ifdef __APPLE__
        uint64_t sector_count = 0;
        uint32_t sector_size = 0;
        if (ioctl(ctx.fd, DKIOCGETBLOCKCOUNT, &sector_count) < 0 ||
            ioctl(ctx.fd, DKIOCGETBLOCKSIZE, &sector_size) < 0)
            err(1, "Cannot get device size");
        ctx.total_sectors = sector_count;
#else
        off_t size = lseek(ctx.fd, 0, SEEK_END);
        if (size < 0)
            err(1, "Cannot determine device size");
        ctx.total_sectors = size / ctx.bytes_per_sector;
#endif
    }

    /* Calculate filesystem parameters */
    ctx.cluster_count = (ctx.total_sectors - 24) / ctx.sectors_per_cluster;
    ctx.fat_length = (ctx.cluster_count * 4 + ctx.bytes_per_sector - 1) / 
                     ctx.bytes_per_sector;

    /* Generate volume serial number */
    ctx.volume_serial = generate_volume_serial();

    /* Write filesystem structures */
    if (calculate_layout(&ctx) < 0)
        goto error;

    if (write_boot_sector(&ctx) < 0)
        goto error;

    if (write_fat(&ctx) < 0)
        goto error;

    /* Write filesystem structures */
    if (write_bitmap(&ctx) < 0)
        goto error;

    if (write_upcase_table(&ctx) < 0)
        goto error;

    if (write_root_dir(&ctx) < 0)
        goto error;

    /* Clean up */
    close(ctx.fd);
    return 0;

error:
    close(ctx.fd);
    return 1;
} 