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
report_progress(struct mkfs_exfat_ctx *ctx, int level, const char *fmt, ...)
{
    va_list ap;

    if (ctx->verbose < level)
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
write_sector(struct mkfs_exfat_ctx *ctx, uint32_t sector, const void *buffer)
{
    off_t offset = (off_t)sector * EXFAT_SECTOR_SIZE;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("seek error to sector %u", sector);
        return -1;
    }

    if (write(ctx->fd, buffer, EXFAT_SECTOR_SIZE) != EXFAT_SECTOR_SIZE) {
        warn("write error at sector %u", sector);
        return -1;
    }

    return 0;
}

static int
calculate_layout(struct mkfs_exfat_ctx *ctx)
{
    uint64_t min_size;

    /* Validate parameters */
    if (ctx->bytes_per_sector != EXFAT_SECTOR_SIZE) {
        warnx("Only 512-byte sectors are currently supported");
        return -1;
    }

    if (!EXFAT_VALID_CLUSTERSIZE(ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE)) {
        warnx("Invalid cluster size");
        return -1;
    }

    /* Check minimum volume size */
    min_size = (uint64_t)EXFAT_MIN_VOLUME_SIZE;
    if (ctx->total_sectors * EXFAT_SECTOR_SIZE < min_size) {
        warnx("Device too small (minimum size is %llu bytes)",
              (unsigned long long)min_size);
        return -1;
    }

    /* Calculate layout */
    ctx->fat_offset = 24;  /* After boot region */
    ctx->fat_length = (ctx->cluster_count * 4 + EXFAT_SECTOR_SIZE - 1) / 
                     EXFAT_SECTOR_SIZE;
    ctx->cluster_heap_offset = ctx->fat_offset + ctx->fat_length;

    /* Recalculate cluster count based on actual space available */
    uint64_t available_sectors = ctx->total_sectors - ctx->cluster_heap_offset;
    ctx->cluster_count = available_sectors / ctx->sectors_per_cluster;
    if (ctx->cluster_count > EXFAT_MAX_CLUSTERS)
        ctx->cluster_count = EXFAT_MAX_CLUSTERS;

    /* Assign special clusters */
    ctx->bitmap_cluster = 2;     /* First cluster of bitmap */
    ctx->upcase_cluster = 3;     /* First cluster of upcase table */
    ctx->root_cluster = 4;       /* First cluster of root directory */

    report_progress(ctx, DEBUG_DETAIL, "Filesystem layout:");
    report_progress(ctx, DEBUG_DETAIL, "  FAT offset: %u sectors", ctx->fat_offset);
    report_progress(ctx, DEBUG_DETAIL, "  FAT length: %u sectors", ctx->fat_length);
    report_progress(ctx, DEBUG_DETAIL, "  Cluster heap offset: %u sectors", 
                   ctx->cluster_heap_offset);
    report_progress(ctx, DEBUG_DETAIL, "  Cluster count: %u", ctx->cluster_count);

    return 0;
}

static int
write_cluster(struct mkfs_exfat_ctx *ctx, uint32_t cluster, const void *buffer)
{
    uint32_t first_sector = ctx->cluster_heap_offset + 
                           ((cluster - 2) * ctx->sectors_per_cluster);
    size_t cluster_size = ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE;
    
    report_progress(ctx, DEBUG_DUMP, "Writing cluster %u (sectors %u-%u)",
                   cluster, first_sector, 
                   first_sector + ctx->sectors_per_cluster - 1);

    if (lseek(ctx->fd, first_sector * EXFAT_SECTOR_SIZE, SEEK_SET) < 0) {
        warn("seek error to cluster %u", cluster);
        return -1;
    }

    if (write(ctx->fd, buffer, cluster_size) != cluster_size) {
        warn("write error at cluster %u", cluster);
        return -1;
    }

    return 0;
}

static uint32_t
calculate_boot_checksum(struct mkfs_exfat_ctx *ctx, const void *boot_region, size_t size)
{
    const uint8_t *p = boot_region;
    uint32_t checksum = 0;
    size_t i;

    /* Print boot region dump at highest verbosity */
    if (ctx->verbose >= DEBUG_DUMP) {
        printf("Boot region contents:\n");
        for (i = 0; i < size; i++) {
            if (i % 16 == 0)
                printf("%04zx: ", i);
            printf("%02x%s", p[i], (i + 1) % 16 ? " " : "\n");
        }
        printf("\n");
    }

    for (i = 0; i < size; i++) {
        /* Skip VolumeFlags and PercentInUse fields */
        if (i == 106 || i == 107 || i == 112)
            continue;
            
        checksum = ((checksum << 31) | (checksum >> 1)) + p[i];

        /* Print intermediate checksums at highest verbosity */
        if (ctx->verbose >= DEBUG_DUMP && i % 512 == 0)
            printf("Checksum after sector %zu: %08x\n", i/512, checksum);
    }

    return checksum;
}

static int
write_boot_sector(struct mkfs_exfat_ctx *ctx)
{
    uint8_t boot_region[EXFAT_BOOT_REGION_SIZE * EXFAT_SECTOR_SIZE];
    struct exfat_boot_record *boot;
    uint32_t checksum;
    int i;

    report_progress(ctx, DEBUG_DETAIL, "Writing boot sector and boot region");

    /* Initialize boot region to zeros */
    memset(boot_region, 0, sizeof(boot_region));
    boot = (struct exfat_boot_record *)boot_region;

    /* Initialize boot sector */
    boot->jump_boot[0] = 0xEB;
    boot->jump_boot[1] = 0x76;
    boot->jump_boot[2] = 0x90;
    memcpy(boot->fs_name, "EXFAT   ", 8);

    /* Zero the must_be_zero field */
    memset(boot->must_be_zero, 0, sizeof(boot->must_be_zero));

    /* Set partition offset (8 sectors) */
    boot->partition_offset[0] = htole16(0x0800);
    boot->partition_offset[1] = 0;
    boot->partition_offset[2] = 0;
    boot->partition_offset[3] = 0;

    /* Set volume parameters */
    boot->volume_length = htole64(ctx->total_sectors);
    boot->fat_offset = htole32(ctx->fat_offset);
    boot->fat_length = htole32(ctx->fat_length);
    boot->cluster_heap_offset = htole32(ctx->cluster_heap_offset);
    boot->cluster_count = htole32(ctx->cluster_count);
    boot->root_dir_cluster = htole32(ctx->root_cluster);
    boot->volume_serial = htole32(ctx->volume_serial);
    boot->fs_revision = htole16(EXFAT_DEFAULT_REVISION);
    boot->volume_flags = 0;
    boot->bytes_per_sector_shift = 9;  /* 512 bytes */
    boot->sectors_per_cluster_shift = 6;  /* 64 sectors = 32KB */
    boot->number_of_fats = ctx->number_of_fats;
    boot->drive_select = EXFAT_DEFAULT_DRIVE;
    boot->percent_in_use = 0;
    memset(boot->reserved, 0, sizeof(boot->reserved));

    /* Fill boot sector padding with 0xF4 */
    memset(boot_region + 0x78, 0xF4, EXFAT_SECTOR_SIZE - 0x78 - 2);

    /* Set boot signatures for all required sectors */
    for (i = 0; i < 9; i++) {
        boot_region[i * EXFAT_SECTOR_SIZE + 510] = 0x55;
        boot_region[i * EXFAT_SECTOR_SIZE + 511] = 0xAA;
    }

    /* Add validation patterns */
    for (i = 0; i < 24; i++) {
        if (i == EXFAT_VALIDATION_SECTOR1 || i == EXFAT_VALIDATION_SECTOR2) {
            uint32_t pattern = EXFAT_BOOT_VALIDATION_PATTERN;
            uint8_t *sector = boot_region + (i * EXFAT_SECTOR_SIZE);
            
            report_progress(ctx, DEBUG_DETAIL, "Writing validation pattern to sector %d", i);
            
            /* Fill sector with repeating pattern */
            for (size_t j = 0; j < EXFAT_SECTOR_SIZE - 2; j += 4) {
                *(uint32_t *)(sector + j) = pattern;
            }
            
            /* Add boot signature */
            sector[510] = 0x55;
            sector[511] = 0xAA;
        }
    }

    /* Calculate boot checksum */
    checksum = calculate_boot_checksum(ctx, boot_region, EXFAT_SECTOR_SIZE * 11);
    report_progress(ctx, DEBUG_DETAIL, "Boot region checksum: %08x", checksum);

    /* Write checksum to main boot sector */
    *(uint32_t *)(boot_region + 0x6A) = htole32(checksum);

    /* Fill sector 11 with repeated checksum */
    uint8_t *checksum_sector = boot_region + (11 * EXFAT_SECTOR_SIZE);
    for (i = 0; i < EXFAT_SECTOR_SIZE - 2; i += 4) {
        *(uint32_t *)(checksum_sector + i) = checksum;
    }
    checksum_sector[510] = 0x55;
    checksum_sector[511] = 0xAA;

    /* Write main boot region (sectors 0-11) */
    report_progress(ctx, DEBUG_DETAIL, "Writing main boot region");
    for (i = 0; i < 12; i++) {
        if (write_sector(ctx, i, boot_region + (i * EXFAT_SECTOR_SIZE)) < 0)
            return -1;
    }

    /* Write backup boot region (sectors 12-23) */
    report_progress(ctx, DEBUG_DETAIL, "Writing backup boot region");
    for (i = 0; i < 12; i++) {
        if (write_sector(ctx, i + 12, boot_region + (i * EXFAT_SECTOR_SIZE)) < 0)
            return -1;
    }

    report_progress(ctx, DEBUG_BASIC, "Boot region written successfully");
    return 0;
}

int
write_fat(struct mkfs_exfat_ctx *ctx)
{
    uint32_t *fat;
    size_t fat_size;
    int i;

    report_progress(ctx, DEBUG_DETAIL, "Writing FAT (offset=%u, length=%u sectors)", 
                   ctx->fat_offset, ctx->fat_length);

    /* Allocate FAT buffer */
    fat_size = ctx->fat_length * EXFAT_SECTOR_SIZE;
    fat = calloc(1, fat_size);
    if (fat == NULL) {
        warn("Failed to allocate FAT buffer");
        return -1;
    }

    /* Initialize FAT entries */
    fat[0] = htole32(0xFFFFFFF8);  /* Media type */
    fat[1] = htole32(0xFFFFFFFF);  /* EOC marker */

    /* Mark clusters 2-4 as used (bitmap, upcase, root dir) */
    for (i = 2; i <= 4; i++) {
        fat[i] = htole32(EXFAT_CLUSTER_END);
    }

    report_progress(ctx, DEBUG_DUMP, "First 16 FAT entries:");
    for (i = 0; i < 16 && ctx->verbose >= DEBUG_DUMP; i++) {
        printf("%08x%s", le32toh(fat[i]), (i + 1) % 4 ? " " : "\n");
    }

    /* Write FAT */
    if (lseek(ctx->fd, ctx->fat_offset * EXFAT_SECTOR_SIZE, SEEK_SET) < 0 ||
        write(ctx->fd, fat, fat_size) != fat_size) {
        warn("Failed to write FAT");
        free(fat);
        return -1;
    }

    free(fat);
    report_progress(ctx, DEBUG_BASIC, "FAT written successfully");
    return 0;
}

int
write_bitmap(struct mkfs_exfat_ctx *ctx)
{
    uint8_t *bitmap;
    size_t bitmap_size;

    report_progress(ctx, DEBUG_DETAIL, "Writing allocation bitmap (cluster %u)", 
                   ctx->bitmap_cluster);

    /* Calculate bitmap size (rounded up to cluster size) */
    bitmap_size = (ctx->cluster_count + 7) / 8;
    bitmap_size = roundup2(bitmap_size, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);

    bitmap = calloc(1, bitmap_size);
    if (bitmap == NULL) {
        warn("Failed to allocate bitmap buffer");
        return -1;
    }

    /* Mark first clusters as used (bitmap, upcase, root dir) */
    bitmap[0] = 0x07;  /* Clusters 2-4 */

    if (ctx->verbose >= DEBUG_DUMP) {
        printf("First 32 bytes of bitmap:\n");
        for (size_t i = 0; i < 32; i++) {
            printf("%02x%s", bitmap[i], (i + 1) % 16 ? " " : "\n");
        }
    }

    /* Write bitmap to its cluster */
    if (write_cluster(ctx, ctx->bitmap_cluster, bitmap) < 0) {
        warn("Failed to write bitmap");
        free(bitmap);
        return -1;
    }

    free(bitmap);
    report_progress(ctx, DEBUG_BASIC, "Allocation bitmap written successfully");
    return 0;
}

int
write_upcase_table(struct mkfs_exfat_ctx *ctx)
{
    uint16_t *upcase;
    size_t upcase_size = 5836;  /* Size of standard upcase table */
    uint32_t checksum = 0;

    report_progress(ctx, DEBUG_DETAIL, "Writing upcase table (cluster %u)", 
                   ctx->upcase_cluster);

    /* Allocate upcase table buffer */
    upcase = calloc(1, roundup2(upcase_size, 
                   ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE));
    if (upcase == NULL) {
        warn("Failed to allocate upcase table buffer");
        return -1;
    }

    /* Initialize standard upcase table */
    for (uint32_t i = 0; i < 128; i++) {
        upcase[i] = htole16(i);
    }
    for (uint32_t i = 'a'; i <= 'z'; i++) {
        upcase[i] = htole16(i - 0x20);
    }
    /* Add additional mappings as needed */

    /* Calculate checksum */
    for (size_t i = 0; i < upcase_size/2; i++) {
        checksum = ((checksum << 31) | (checksum >> 1)) + le16toh(upcase[i]);
    }
    ctx->upcase_checksum = checksum;

    report_progress(ctx, DEBUG_DETAIL, "Upcase table checksum: %08x", checksum);

    if (ctx->verbose >= DEBUG_DUMP) {
        printf("First 32 upcase entries:\n");
        for (size_t i = 0; i < 32; i++) {
            printf("%04x%s", le16toh(upcase[i]), (i + 1) % 8 ? " " : "\n");
        }
    }

    /* Write upcase table */
    if (write_cluster(ctx, ctx->upcase_cluster, upcase) < 0) {
        warn("Failed to write upcase table");
        free(upcase);
        return -1;
    }

    free(upcase);
    report_progress(ctx, DEBUG_BASIC, "Upcase table written successfully");
    return 0;
}

/* Add this helper function */
static void
set_timestamps(struct exfat_entry_file *entry, const struct timespec *ts)
{
    uint32_t date, time;
    
    unix_time_to_exfat(ts, &date, &time);
    
    entry->create_timestamp = date;
    entry->last_modified_timestamp = date;
    entry->last_access_timestamp = date;
    
    entry->create_time_ms = (ts->tv_nsec / 10000000) & 0xFF;  /* Convert ns to 10ms units */
    entry->last_modified_time_ms = entry->create_time_ms;
    
    /* Set timezone offset to UTC */
    entry->create_tz = 0;
    entry->last_modified_tz = 0;
    entry->last_access_tz = 0;
}

int
write_root_dir(struct mkfs_exfat_ctx *ctx)
{
    uint8_t *cluster;
    size_t cluster_size = ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE;

    report_progress(ctx, DEBUG_DETAIL, "Writing root directory (cluster %u)", 
                   ctx->root_cluster);

    /* Allocate root directory buffer */
    cluster = calloc(1, cluster_size);
    if (cluster == NULL) {
        warn("Failed to allocate root directory buffer");
        return -1;
    }

    /* Initialize directory entries */
    struct exfat_entry_bitmap *bitmap = (struct exfat_entry_bitmap *)cluster;
    bitmap->type = EXFAT_ENTRY_BITMAP;
    bitmap->first_cluster = htole32(ctx->bitmap_cluster);
    bitmap->data_length = htole64((ctx->cluster_count + 7) / 8);

    struct exfat_entry_upcase *upcase = 
        (struct exfat_entry_upcase *)(cluster + sizeof(struct exfat_entry_bitmap));
    upcase->type = EXFAT_ENTRY_UPCASE;
    upcase->checksum = htole32(ctx->upcase_checksum);
    upcase->first_cluster = htole32(ctx->upcase_cluster);
    upcase->data_length = htole64(5836);

    if (ctx->verbose >= DEBUG_DUMP) {
        printf("Root directory entries:\n");
        for (size_t i = 0; i < 64; i++) {
            printf("%02x%s", cluster[i], (i + 1) % 16 ? " " : "\n");
        }
    }

    /* Write root directory cluster */
    if (write_cluster(ctx, ctx->root_cluster, cluster) < 0) {
        warn("Failed to write root directory");
        free(cluster);
        return -1;
    }

    free(cluster);
    report_progress(ctx, DEBUG_BASIC, "Root directory written successfully");
    return 0;
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
            ctx.verbose++;  /* Each -v increases verbosity */
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

    /* Add progress reporting */
    report_progress(&ctx, DEBUG_BASIC, "Creating ExFAT filesystem on %s", ctx.device);
    report_progress(&ctx, DEBUG_DETAIL, "Parameters:");
    report_progress(&ctx, DEBUG_DETAIL, "  Bytes per sector: %u", ctx.bytes_per_sector);
    report_progress(&ctx, DEBUG_DETAIL, "  Sectors per cluster: %u", ctx.sectors_per_cluster);
    report_progress(&ctx, DEBUG_DETAIL, "  Number of FATs: %u", ctx.number_of_fats);
    report_progress(&ctx, DEBUG_DETAIL, "  Total sectors: %lu", (unsigned long)ctx.total_sectors);

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