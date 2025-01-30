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
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mkfs_exfat.h"
#include "exfat.h"

static void
usage(void)
{
    fprintf(stderr, "usage: mkfs_exfat [-n label] [-s sectors-per-cluster] [-v] device\n");
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
    uint64_t total_sectors = ctx->dev_size / EXFAT_SECTOR_SIZE;
    uint32_t sectors_per_cluster;
    uint32_t cluster_count;
    uint32_t fat_sectors;
    uint32_t bitmap_clusters;
    uint32_t upcase_clusters;
    uint32_t root_clusters = 1;  /* Start with 1 cluster for root dir */

    /* If cluster size not specified, auto-select based on volume size */
    if (ctx->cluster_size == 0) {
        if (total_sectors < 2097152)        /* < 1GB */
            ctx->cluster_size = 4096;       /* 4KB */
        else if (total_sectors < 16777216)  /* < 8GB */
            ctx->cluster_size = 32768;      /* 32KB */
        else
            ctx->cluster_size = 131072;     /* 128KB */
    }

    sectors_per_cluster = ctx->cluster_size / EXFAT_SECTOR_SIZE;

    /* Calculate number of clusters */
    cluster_count = (total_sectors - 24) / sectors_per_cluster;

    /* Calculate FAT size (4 bytes per cluster) */
    fat_sectors = ((uint64_t)cluster_count * 4 + EXFAT_SECTOR_SIZE - 1) / 
                 EXFAT_SECTOR_SIZE;

    /* Calculate bitmap size (1 bit per cluster) */
    bitmap_clusters = ((uint64_t)cluster_count + 7) / 8;
    bitmap_clusters = (bitmap_clusters + ctx->cluster_size - 1) / ctx->cluster_size;

    /* Calculate upcase table size (128KB) */
    upcase_clusters = (128 * 1024 + ctx->cluster_size - 1) / ctx->cluster_size;

    /* Store calculated values */
    ctx->boot.bytes_per_sector_shift = 9;  /* 512 bytes */
    ctx->boot.sectors_per_cluster_shift = ffs(sectors_per_cluster) - 1;
    ctx->boot.number_of_fats = 1;
    ctx->boot.volume_length = total_sectors;
    ctx->boot.fat_offset = 24;
    ctx->boot.fat_length = fat_sectors;
    ctx->boot.cluster_heap_offset = ctx->boot.fat_offset + fat_sectors;
    ctx->boot.cluster_count = cluster_count;
    ctx->boot.root_dir_cluster = bitmap_clusters + upcase_clusters + 2;
    ctx->boot.volume_serial = ctx->volume_serial;
    ctx->boot.fs_revision = 0x100;  /* Version 1.00 */
    ctx->boot.volume_flags = 0;
    ctx->boot.percent_in_use = 0;

    /* Store cluster locations */
    ctx->bitmap_cluster = 2;
    ctx->upcase_cluster = bitmap_clusters + 2;
    ctx->root_cluster = ctx->boot.root_dir_cluster;
    ctx->first_cluster = ctx->root_cluster + root_clusters;

    /* Verify layout */
    if (ctx->boot.cluster_heap_offset + 
        ((uint64_t)cluster_count * sectors_per_cluster) > total_sectors) {
        warnx("Invalid filesystem layout - device too small");
        return -1;
    }

    return 0;
}

static int
write_boot_sector(struct mkfs_exfat_ctx *ctx)
{
    uint8_t *sector;
    int error = 0;

    sector = calloc(1, EXFAT_SECTOR_SIZE);
    if (sector == NULL) {
        warn("Cannot allocate sector buffer");
        return -1;
    }

    /* Initialize boot sector */
    struct exfat_boot_record *boot = (struct exfat_boot_record *)sector;

    /* Jump instruction and filesystem name */
    boot->jump_boot[0] = 0xEB;
    boot->jump_boot[1] = 0x76;
    boot->jump_boot[2] = 0x90;
    memcpy(boot->fs_name, "EXFAT   ", 8);

    /* Copy calculated parameters */
    memcpy(&boot->partition_offset, &ctx->boot.partition_offset,
           sizeof(struct exfat_boot_record) - 
           offsetof(struct exfat_boot_record, partition_offset));

    /* Write main boot sector */
    if (write_sector(ctx, 0, sector) < 0) {
        error = -1;
        goto out;
    }

    /* Write backup boot sector */
    if (write_sector(ctx, 12, sector) < 0) {
        error = -1;
        goto out;
    }

    /* Write remaining boot sectors with zeros */
    memset(sector, 0, EXFAT_SECTOR_SIZE);
    for (int i = 1; i < 12; i++) {
        if (write_sector(ctx, i, sector) < 0) {
            error = -1;
            goto out;
        }
    }
    for (int i = 13; i < 24; i++) {
        if (write_sector(ctx, i, sector) < 0) {
            error = -1;
            goto out;
        }
    }

out:
    free(sector);
    return error;
}

int
write_fat(struct mkfs_exfat_ctx *ctx)
{
    uint32_t *fat_sector;
    uint32_t total_sectors, fat_sectors, cluster_count;
    uint32_t sector;

    /* Calculate layout */
    total_sectors = ctx->dev_size / EXFAT_SECTOR_SIZE;
    cluster_count = (total_sectors - 24) / (ctx->cluster_size / EXFAT_SECTOR_SIZE);
    fat_sectors = (cluster_count * 4 + EXFAT_SECTOR_SIZE - 1) / EXFAT_SECTOR_SIZE;

    /* Allocate buffer for one FAT sector */
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (fat_sector == NULL) {
        warn("Cannot allocate FAT sector buffer");
        return -1;
    }

    /* Initialize first FAT sector */
    memset(fat_sector, 0, EXFAT_SECTOR_SIZE);
    fat_sector[0] = htole32(0xFFFFFFF8);  /* Media descriptor */
    fat_sector[1] = htole32(0xFFFFFFFF);  /* EOC marker */
    fat_sector[2] = htole32(EXFAT_CLUSTER_END);  /* Bitmap */
    fat_sector[3] = htole32(EXFAT_CLUSTER_END);  /* Upcase table */
    fat_sector[4] = htole32(EXFAT_CLUSTER_END);  /* Root directory */

    /* Write first FAT sector */
    if (write_sector(ctx, 24, fat_sector) < 0) {
        free(fat_sector);
        return -1;
    }

    /* Initialize remaining FAT sectors to zero */
    memset(fat_sector, 0, EXFAT_SECTOR_SIZE);
    for (sector = 1; sector < fat_sectors; sector++) {
        if (write_sector(ctx, 24 + sector, fat_sector) < 0) {
            free(fat_sector);
            return -1;
        }
    }

    free(fat_sector);
    report_progress(ctx, "FAT written");
    return 0;
}

static int
write_cluster(struct mkfs_exfat_ctx *ctx, uint32_t cluster, const void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    /* Calculate cluster offset */
    offset = ((off_t)ctx->cluster_heap_offset +
             ((off_t)cluster - 2) * (ctx->cluster_size / EXFAT_SECTOR_SIZE)) *
             EXFAT_SECTOR_SIZE;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("seek error");
        return -1;
    }

    bytes = write(ctx->fd, buffer, ctx->cluster_size);
    if (bytes != ctx->cluster_size) {
        warn("write error");
        return -1;
    }

    return 0;
}

int
write_bitmap(struct mkfs_exfat_ctx *ctx)
{
    uint8_t *bitmap;
    uint32_t bitmap_size;
    uint32_t bitmap_clusters;
    uint32_t i;

    /* Calculate bitmap size (rounded up to cluster size) */
    bitmap_size = (ctx->cluster_count + 7) / 8;
    bitmap_clusters = (bitmap_size + ctx->cluster_size - 1) / ctx->cluster_size;
    bitmap_size = bitmap_clusters * ctx->cluster_size;

    /* Allocate bitmap buffer */
    bitmap = calloc(1, bitmap_size);
    if (bitmap == NULL) {
        warn("Cannot allocate bitmap buffer");
        return -1;
    }

    /* Mark system clusters as allocated */
    for (i = 0; i < ctx->first_cluster - 2; i++) {
        bitmap[i / 8] |= 1 << (i % 8);
    }

    /* Write bitmap clusters */
    for (i = 0; i < bitmap_clusters; i++) {
        if (write_cluster(ctx, ctx->bitmap_cluster + i,
                         bitmap + i * ctx->cluster_size) < 0) {
            free(bitmap);
            return -1;
        }
    }

    free(bitmap);
    report_progress(ctx, "Allocation bitmap written");
    return 0;
}

static int
write_upcase_table(struct mkfs_exfat_ctx *ctx)
{
    uint16_t *upcase;
    uint32_t table_size = 128 * 1024;  /* 128KB standard size */
    uint32_t checksum = 0;
    uint32_t i;
    int error = 0;

    /* Allocate upcase table buffer */
    upcase = calloc(1, table_size);
    if (upcase == NULL) {
        warn("Cannot allocate upcase table buffer");
        return -1;
    }

    /* Initialize Unicode uppercase mapping table */
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
    uint32_t clusters_needed = (table_size + ctx->cluster_size - 1) / ctx->cluster_size;
    for (i = 0; i < clusters_needed; i++) {
        uint32_t write_size = MIN(ctx->cluster_size, table_size - i * ctx->cluster_size);
        if (write_cluster(ctx, ctx->upcase_cluster + i, 
                         (uint8_t *)upcase + i * ctx->cluster_size) < 0) {
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
    char *root_cluster = calloc(1, ctx->cluster_size);
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
    cluster = calloc(1, ctx->cluster_size);
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
    es.stream.data_length = ctx->cluster_size;  /* One cluster for now */
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

int
main(int argc, char *argv[])
{
    struct mkfs_exfat_ctx ctx;
    struct stat sb;
    int ch;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.cluster_size = 32768;  /* Default: 32KB */

    /* Parse command line options */
    while ((ch = getopt(argc, argv, "n:s:v")) != -1) {
        switch (ch) {
        case 'n':
            ctx.volume_label = optarg;
            break;
        case 's':
            ctx.cluster_size = atoi(optarg) * EXFAT_SECTOR_SIZE;
            if (ctx.cluster_size < EXFAT_SECTOR_SIZE ||
                ctx.cluster_size > 32 * 1024 * 1024 ||
                (ctx.cluster_size & (ctx.cluster_size - 1)) != 0) {
                errx(1, "Invalid cluster size");
            }
            break;
        case 'v':
            ctx.verbose = 1;
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1)
        usage();

    /* Open the device */
    ctx.device = argv[0];
    ctx.fd = open(ctx.device, O_RDWR);
    if (ctx.fd < 0)
        err(1, "Cannot open %s", ctx.device);

    /* Get device size */
    if (fstat(ctx.fd, &sb) < 0)
        err(1, "Cannot stat device");
    ctx.dev_size = sb.st_size;

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