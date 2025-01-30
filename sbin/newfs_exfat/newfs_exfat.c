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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/disk.h>
#endif

#include "newfs_exfat.h"

/* Debug levels */
#define DEBUG_NONE    0
#define DEBUG_BASIC   1
#define DEBUG_DETAIL  2
#define DEBUG_DUMP    3

static void
report_progress(struct mkfs_exfat_ctx *ctx, int level, const char *fmt, ...)
{
    va_list ap;

    if (level > ctx->verbose)
        return;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int
write_sector(struct mkfs_exfat_ctx *ctx, uint32_t sector, const void *data)
{
    if (lseek(ctx->fd, sector * EXFAT_SECTOR_SIZE, SEEK_SET) < 0) {
        warn("seek error to sector %u", sector);
        return -1;
    }

    if (write(ctx->fd, data, EXFAT_SECTOR_SIZE) != EXFAT_SECTOR_SIZE) {
        warn("write error at sector %u", sector);
        return -1;
    }

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

    /* Calculate checksum over first 11 sectors */
    for (i = 0; i < 11 * EXFAT_SECTOR_SIZE; i++) {
        /* Skip volume flags and checksum field */
        if (i == 106 || i == 107 || i == 112)
            continue;

        /* Right rotate by 1 bit and add byte */
        checksum = ((checksum >> 1) | (checksum << 31)) + p[i];
    }

    return checksum;
}

static int
write_boot_sector(struct mkfs_exfat_ctx *ctx)
{
    uint8_t boot_region[EXFAT_BOOT_REGION_SIZE * EXFAT_SECTOR_SIZE] = {0};
    struct exfat_boot_record *boot = (struct exfat_boot_record *)boot_region;
    uint32_t checksum;

    /* Initialize boot sector fields */
    boot->jump_boot[0] = 0xEB;
    boot->jump_boot[1] = 0x76;
    boot->jump_boot[2] = 0x90;
    memcpy(boot->fs_name, "EXFAT   ", 8);
    boot->volume_length = htole64(ctx->total_sectors);
    boot->fat_offset = htole32(ctx->fat_offset);
    boot->fat_length = htole32(ctx->fat_length);
    boot->cluster_heap_offset = htole32(ctx->cluster_heap_offset);
    boot->cluster_count = htole32(ctx->cluster_count);
    boot->root_dir_cluster = htole32(ctx->root_cluster);
    boot->volume_serial = htole32(0x67A30E5A);  /* Random serial */
    boot->fs_revision = htole16(0x0100);
    boot->bytes_per_sector_shift = 9;  /* 512 bytes */
    boot->sectors_per_cluster_shift = 6;  /* 64 sectors */
    boot->number_of_fats = 1;
    boot->drive_select = 0x80;

    /* Fill boot code area with 0xF4 */
    memset(boot->boot_code, 0xF4, sizeof(boot->boot_code));

    /* Add sector signatures */
    for (int i = 0; i < EXFAT_BOOT_REGION_SIZE; i++) {
        boot_region[(i * EXFAT_SECTOR_SIZE) + 510] = 0x55;
        boot_region[(i * EXFAT_SECTOR_SIZE) + 511] = 0xAA;
    }

    /* Calculate checksum of first 11 sectors */
    checksum = calculate_boot_checksum(ctx, boot_region, 11 * EXFAT_SECTOR_SIZE);

    /* Write checksum into boot sector */
    boot->volume_flags = htole16(checksum & 0xFFFF);
    *(uint8_t *)(boot_region + 112) = (checksum >> 16) & 0xFF;

    /* Fill sector 11 with repeating checksum value */
    uint32_t *checksum_sector = (uint32_t *)(boot_region + 11 * EXFAT_SECTOR_SIZE);
    for (size_t i = 0; i < EXFAT_SECTOR_SIZE / sizeof(uint32_t); i++) {
        checksum_sector[i] = checksum;
    }

    /* Write boot region */
    for (int i = 0; i < EXFAT_BOOT_REGION_SIZE; i++) {
        if (write_sector(ctx, i, boot_region + (i * EXFAT_SECTOR_SIZE)) < 0) {
            warn("Failed to write boot sector %d", i);
            return -1;
        }
    }

    report_progress(ctx, DEBUG_BASIC, "Boot sector written successfully");
    return 0;
}

int
write_fat(struct mkfs_exfat_ctx *ctx)
{
    uint32_t *fat;
    size_t fat_size;

    report_progress(ctx, DEBUG_DETAIL, "Writing FAT (offset=%u, length=%u sectors)",
                   ctx->fat_offset, ctx->fat_length);

    /* Allocate FAT buffer */
    fat_size = ctx->fat_length * EXFAT_SECTOR_SIZE;
    fat = calloc(1, fat_size);
    if (fat == NULL) {
        warn("Failed to allocate FAT buffer");
        return -1;
    }

    /* Initialize special clusters */
    fat[0] = htole32(0xFFFFFFF8);  /* Media type */
    fat[1] = htole32(0xFFFFFFFF);  /* EOC */
    fat[ctx->bitmap_cluster] = htole32(0xFFFFFFFF);  /* Bitmap EOC */
    fat[ctx->upcase_cluster] = htole32(0xFFFFFFFF);  /* Upcase EOC */
    fat[ctx->root_cluster] = htole32(0xFFFFFFFF);  /* Root EOC */

    /* Write FAT */
    if (lseek(ctx->fd, ctx->fat_offset * EXFAT_SECTOR_SIZE, SEEK_SET) < 0) {
        warn("Failed to seek to FAT");
        free(fat);
        return -1;
    }

    if (write(ctx->fd, fat, fat_size) != fat_size) {
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
    int error;

    /* Allocate and clear bitmap buffer */
    bitmap = calloc(1, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);
    if (!bitmap) {
        warn("Failed to allocate bitmap buffer");
        return -1;
    }

    /* Mark system clusters as allocated (clusters 2-4) */
    bitmap[0] = 0x07;  /* Bits 0-2 set (for clusters 2-4) */

    /* Write bitmap to its cluster */
    error = write_cluster(ctx, ctx->bitmap_cluster, bitmap);

    free(bitmap);
    if (error == 0) {
        report_progress(ctx, DEBUG_BASIC, "Bitmap written successfully");
    }
    return error;
}

static uint32_t
calculate_upcase_checksum(const uint16_t *upcase, size_t size)
{
    uint32_t checksum = 0;

    for (size_t i = 0; i < size; i++) {
        if (upcase[i] == 0xFFFF)
            break;
        checksum = ((checksum << 31) | (checksum >> 1)) + le16toh(upcase[i]);
    }

    return checksum;
}

int
write_upcase_table(struct mkfs_exfat_ctx *ctx)
{
    uint16_t *upcase;
    int error;

    /* Allocate cluster-sized buffer and zero it */
    uint8_t *cluster_buffer = calloc(1, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);
    if (!cluster_buffer) {
        warn("Failed to allocate cluster buffer");
        return -1;
    }

    /* Allocate and initialize upcase table */
    upcase = (uint16_t *)cluster_buffer;

    /* Initialize basic upcase table (0x0000 - 0x00C5) */
    for (int i = 0; i < EXFAT_UPCASE_SIZE; i++) {
        if (i >= 'a' && i <= 'z') {
            upcase[i] = htole16(i - 32);  /* Convert lowercase to uppercase */
        } else {
            upcase[i] = htole16(i);       /* Keep other characters as-is */
        }
    }

    /* Calculate checksum */
    ctx->upcase_checksum = calculate_upcase_checksum(upcase, EXFAT_UPCASE_SIZE);
    report_progress(ctx, DEBUG_BASIC, "Upcase table checksum: %08x", ctx->upcase_checksum);

    /* Write upcase table to cluster */
    error = write_cluster(ctx, ctx->upcase_cluster, cluster_buffer);

    free(cluster_buffer);
    return error;
}

int
write_root_dir(struct mkfs_exfat_ctx *ctx, const void *entry, size_t size)
{
    uint8_t *cluster_buffer = calloc(1, ctx->sectors_per_cluster * EXFAT_SECTOR_SIZE);
    struct exfat_entry_bitmap *bitmap;
    struct exfat_entry_upcase *upcase;
    struct exfat_entry_label *label;
    int ret = 0;

    if (!cluster_buffer) {
        warn("Failed to allocate cluster buffer");
        return -1;
    }

    /* First entry: volume label */
    label = (struct exfat_entry_label *)cluster_buffer;
    label->type = EXFAT_ENTRY_LABEL;
    label->character_count = 8;  /* Length of "Untitled" */
    memset(label->reserved, 0, sizeof(label->reserved));

    /* Convert ASCII to UTF-16LE with proper case */
    const char *name = "Untitled";
    for (int i = 0; i < 8; i++) {
        label->volume_label[i] = htole16((uint16_t)name[i]);
    }
    /* Zero out remaining label space */
    for (int i = 8; i < 11; i++) {
        label->volume_label[i] = 0;
    }

    /* Second entry: bitmap */
    bitmap = (struct exfat_entry_bitmap *)(cluster_buffer + EXFAT_ENTRY_SIZE);
    bitmap->type = EXFAT_ENTRY_BITMAP;
    bitmap->flags = 0;
    memset(bitmap->reserved, 0, sizeof(bitmap->reserved));
    bitmap->first_cluster = htole32(ctx->bitmap_cluster);
    bitmap->data_length = htole64((ctx->cluster_count + 7) / 8);

    /* Third entry: upcase table */
    upcase = (struct exfat_entry_upcase *)(cluster_buffer + 2 * EXFAT_ENTRY_SIZE);
    upcase->type = EXFAT_ENTRY_UPCASE;
    upcase->reserved1[0] = 0;
    memset(upcase->reserved2, 0, sizeof(upcase->reserved2));
    upcase->checksum = htole32(ctx->upcase_checksum);
    upcase->first_cluster = htole32(ctx->upcase_cluster);
    upcase->data_length = htole64(EXFAT_UPCASE_SIZE * sizeof(uint16_t));

    report_progress(ctx, DEBUG_DETAIL, "Writing root directory entries:");
    report_progress(ctx, DEBUG_DETAIL, "  Label: type=0x%02x", label->type);
    report_progress(ctx, DEBUG_DETAIL, "  Bitmap: type=0x%02x cluster=%u", 
                   bitmap->type, le32toh(bitmap->first_cluster));
    report_progress(ctx, DEBUG_DETAIL, "  Upcase: type=0x%02x cluster=%u checksum=0x%08x", 
                   upcase->type, le32toh(upcase->first_cluster), le32toh(upcase->checksum));

    /* Write the cluster */
    ret = write_cluster(ctx, ctx->root_cluster, cluster_buffer);

    free(cluster_buffer);
    if (ret == 0) {
        report_progress(ctx, DEBUG_BASIC, "Root directory written successfully");
    }
    return ret;
}

static int
init_filesystem(struct mkfs_exfat_ctx *ctx)
{
#ifndef __APPLE__
    struct stat st;  /* Only declare if not on Apple */
#endif
    off_t size;
    int ret;

    /* Get device size */
#ifdef __APPLE__
    uint32_t block_size;
    uint64_t blocks;

    ret = ioctl(ctx->fd, DKIOCGETBLOCKSIZE, &block_size);
    if (ret < 0) {
        warn("Failed to get block size");
        return -1;
    }

    ret = ioctl(ctx->fd, DKIOCGETBLOCKCOUNT, &blocks);
    if (ret < 0) {
        warn("Failed to get block count");
        return -1;
    }

    size = (off_t)blocks * block_size;
#else
    if (fstat(ctx->fd, &st) < 0) {
        warn("Failed to get device size");
        return -1;
    }
    size = st.st_size;
#endif

    if (size <= 0) {
        warnx("Invalid device size");
        return -1;
    }

    /* Calculate filesystem parameters */
    ctx->total_sectors = size / EXFAT_SECTOR_SIZE;
    ctx->fat_offset = 24;
    ctx->fat_length = 238;
    ctx->cluster_heap_offset = 262;
    ctx->cluster_count = 30459;
    ctx->sectors_per_cluster = 64;

    /* Assign special clusters - make sure bitmap_cluster is valid */
    ctx->bitmap_cluster = 2;     /* First cluster of bitmap */
    ctx->upcase_cluster = 3;     /* First cluster of upcase table */
    ctx->root_cluster = 4;       /* First cluster of root directory */

    /* Validate cluster numbers */
    if (ctx->bitmap_cluster < 2 || ctx->bitmap_cluster >= ctx->cluster_count + 2) {
        warnx("Invalid bitmap cluster number");
        return -1;
    }

    report_progress(ctx, DEBUG_DETAIL, "Filesystem layout:");
    report_progress(ctx, DEBUG_DETAIL, "  FAT offset: %u sectors", ctx->fat_offset);
    report_progress(ctx, DEBUG_DETAIL, "  FAT length: %u sectors", ctx->fat_length);
    report_progress(ctx, DEBUG_DETAIL, "  Cluster heap offset: %u sectors", 
                   ctx->cluster_heap_offset);
    report_progress(ctx, DEBUG_DETAIL, "  Cluster count: %u", ctx->cluster_count);

    return 0;
}

int
main(int argc, char *argv[])
{
    struct mkfs_exfat_ctx ctx = {0};
    int ch;
    int force = 0;

    /* Parse command line options */
    while ((ch = getopt(argc, argv, "fv")) != -1) {
        switch (ch) {
        case 'f':
            force = 1;
            break;
        case 'v':
            ctx.verbose++;
            break;
        default:
            fprintf(stderr, "usage: %s [-f] [-v] device\n", getprogname());
            return 1;
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1) {
        fprintf(stderr, "usage: %s [-f] [-v] device\n", getprogname());
        return 1;
    }

    /* Open device */
    ctx.fd = open(argv[0], O_RDWR);
    if (ctx.fd < 0) {
        err(1, "Failed to open %s", argv[0]);
    }

    /* Warn about data loss */
    if (!force) {
        printf("%s: warning: %s will be formatted. All data will be lost!\n",
               getprogname(), argv[0]);
        printf("Proceed? (y/n) ");
        fflush(stdout);

        if (getchar() != 'y') {
            close(ctx.fd);
            return 1;
        }
    }

    /* Initialize filesystem parameters */
    if (init_filesystem(&ctx) < 0)
        goto error;

    /* Write filesystem structures */
    if (write_boot_sector(&ctx) < 0 ||
        write_fat(&ctx) < 0 ||
        write_bitmap(&ctx) < 0 ||
        write_upcase_table(&ctx) < 0 ||
        write_root_dir(&ctx, NULL, 0) < 0)
        goto error;

    close(ctx.fd);
    return 0;

error:
    close(ctx.fd);
    return 1;
} 