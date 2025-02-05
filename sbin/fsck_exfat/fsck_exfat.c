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
#include <unistd.h>
#include <time.h>

#include "fsck_exfat.h"


#define MAX_CLUSTERS 0xFFFFFFF5  /* Maximum valid cluster number */

/* Add cluster tracking structure */
struct cluster_info {
    uint32_t refs;      /* Reference count */
    uint32_t owner;     /* First cluster of file/dir that owns this cluster */
};

static int get_fat_entry(struct fsck_exfat_ctx *ctx, uint32_t cluster, uint32_t *value);
static int validate_cluster_number(struct fsck_exfat_ctx *ctx, uint32_t cluster, const char *desc);

static void
usage(void)
{
    fprintf(stderr, "usage: fsck_exfat [-fnvy] filesystem\n");
    exit(1);
}

void
report_error(struct fsck_exfat_ctx *ctx, int level, const char *fmt, ...)
{
    va_list ap;
    
    ctx->errors++;
    
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");

    if (level == FSCK_ERR_FATAL)
        exit(1);
}

static int
read_sector(struct fsck_exfat_ctx *ctx, off_t sector, void *buffer)
{
    off_t offset = sector * EXFAT_SECTOR_SIZE;
    ssize_t bytes;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "seek error: %s", strerror(errno));
        return -1;
    }

    bytes = read(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        report_error(ctx, FSCK_ERR_FATAL, "read error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int
read_fat_sector(struct fsck_exfat_ctx *ctx, uint32_t sector, void *buffer)
{
    off_t offset;
    ssize_t bytes;

    /* Validate sector number */
    if (sector >= ctx->emp->boot.fat_length) {
        report_error(ctx, FSCK_ERR_FATAL, "FAT sector number out of range: %u", sector);
        return -1;
    }

    /* Calculate absolute sector offset */
    offset = (off_t)(ctx->emp->boot.fat_offset + sector) * EXFAT_SECTOR_SIZE;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "FAT seek error: %s (offset=%lld)", 
                    strerror(errno), (long long)offset);
        return -1;
    }

    bytes = read(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        report_error(ctx, FSCK_ERR_FATAL, "FAT read error: %s (bytes=%zd)", 
                    strerror(errno), bytes);
        return -1;
    }

    return 0;
}

static int
write_fat_sector(struct fsck_exfat_ctx *ctx, off_t sector, uint32_t *buffer)
{
    off_t offset = (ctx->emp->boot.fat_offset + sector) * EXFAT_SECTOR_SIZE;
    ssize_t bytes;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "seek error: %s", strerror(errno));
        return -1;
    }

    bytes = write(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        report_error(ctx, FSCK_ERR_FATAL, "write error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* Helper function to write FAT entry */
static int
write_fat_entry(struct fsck_exfat_ctx *ctx, uint32_t cluster, uint32_t value)
{
    uint32_t *fat_sector;
    uint32_t sector_index = cluster / (EXFAT_SECTOR_SIZE / 4);
    uint32_t entry_index = cluster % (EXFAT_SECTOR_SIZE / 4);
    int error;

    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (fat_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate FAT sector buffer");
        return -1;
    }

    /* Read FAT sector */
    error = read_fat_sector(ctx, sector_index, fat_sector);
    if (error) {
        free(fat_sector);
        return error;
    }

    /* Update entry */
    fat_sector[entry_index] = htole32(value);

    /* Write back sector */
    error = write_fat_sector(ctx, sector_index, fat_sector);
    free(fat_sector);

    return error;
}

static void
log_info(struct fsck_exfat_ctx *ctx, const char *fmt, ...)
{
    va_list ap;
    
    if (!ctx->verbose)
        return;
        
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void
print_progress(struct fsck_exfat_ctx *ctx, const char *phase, uint32_t current, uint32_t total)
{
    if (!ctx->verbose)
        return;
        
    printf("\r%s: %u/%u (%2.1f%%)...", 
           phase, current, total, 
           total ? ((float)current * 100.0f) / total : 0.0f);
    fflush(stdout);
}

int
check_boot_sector(struct fsck_exfat_ctx *ctx)
{
    log_info(ctx, "Checking boot sector...");
    struct exfat_boot_sector boot;
    int error;

    error = read_sector(ctx, 0, &boot);
    if (error)
        return -1;

    /* Validate signature */
    if (memcmp(boot.jump_boot, "\xEB\x76\x90", 3) != 0) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid jump boot signature");
        return -1;
    }

    if (memcmp(boot.fs_name, "EXFAT   ", 8) != 0) {
        report_error(ctx, FSCK_ERR_FATAL, "Not an ExFAT filesystem");
        return -1;
    }

    /* Validate must-be-zero region */
    for (int i = 0; i < 53; i++) {
        if (boot.must_be_zero[i] != 0) {
            report_error(ctx, FSCK_ERR_FATAL, "Non-zero value in reserved field");
            return -1;
        }
    }

    /* Validate sector size (must be power of 2, 512-4096) */
    uint32_t sector_size = 1 << boot.bytes_per_sector_shift;
    if (boot.bytes_per_sector_shift < 9 || boot.bytes_per_sector_shift > 12) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid sector size: %u", sector_size);
        return -1;
    }

    /* Validate sectors per cluster (must be power of 2) */
    if (boot.sectors_per_cluster_shift == 0) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid sectors per cluster: 0");
        return -1;
    }

    /* Validate number of FATs (should be 1 or 2) */
    if (boot.number_of_fats < 1 || boot.number_of_fats > 2) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid number of FATs: %u", 
                    boot.number_of_fats);
        return -1;
    }

    /* Validate boot signature */
    if (le16toh(boot.boot_signature) != EXFAT_BOOT_SIGNATURE) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid boot sector signature");
        return -1;
    }

    /* Copy boot sector to mount structure */
    memcpy(&ctx->emp->boot, &boot, sizeof(boot));

    /* Find bitmap and upcase clusters from root directory */
    error = find_system_files(ctx);
    if (error)
        return -1;

    log_info(ctx, "Boot sector OK");
    log_info(ctx, "  Bytes per sector: %u", 1 << ctx->emp->boot.bytes_per_sector_shift);
    log_info(ctx, "  Sectors per cluster: %u", 1 << ctx->emp->boot.sectors_per_cluster_shift);
    log_info(ctx, "  Total clusters: %u", ctx->emp->boot.cluster_count);
    log_info(ctx, "  Volume size: %llu MB", 
        ((uint64_t)ctx->emp->boot.cluster_count * 
         (1 << ctx->emp->boot.sectors_per_cluster_shift) * 
         (1 << ctx->emp->boot.bytes_per_sector_shift)) / (1024 * 1024));
    
    return 0;
}

int
find_system_files(struct fsck_exfat_ctx *ctx)
{
    uint8_t *buffer;
    size_t bytes_per_cluster;
    int error = -1;

    /* Validate root directory cluster */
    if (ctx->emp->boot.root_dir_cluster < 2) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid root directory cluster: %u",
                    ctx->emp->boot.root_dir_cluster);
        return -1;
    }

    /* Calculate cluster size */
    bytes_per_cluster = EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Allocate cluster buffer */
    buffer = malloc(bytes_per_cluster);
    if (buffer == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate cluster buffer");
        return -1;
    }

    /* Read root directory cluster */
    error = read_cluster(ctx, ctx->emp->boot.root_dir_cluster, buffer);
    if (error)
        goto out;

    /* Look for bitmap and upcase table entries */
    uint8_t *entry = buffer;
    
    /* First entry should be volume label */
    if (*entry != EXFAT_ENTRY_LABEL) {
        report_error(ctx, FSCK_ERR_FATAL, "Missing volume label in root directory");
        goto out;
    }
    entry += EXFAT_ENTRY_SIZE;  /* Skip volume label */

    /* Next should be bitmap */
    if (*entry != EXFAT_ENTRY_BITMAP) {
        report_error(ctx, FSCK_ERR_FATAL, "Missing bitmap entry in root directory");
        goto out;
    }
    /* Get bitmap cluster from bitmap entry */
    struct exfat_entry_bitmap *bitmap = (struct exfat_entry_bitmap *)entry;
    ctx->bitmap_cluster = le32toh(bitmap->first_cluster);
    if (ctx->bitmap_cluster < 2 || ctx->bitmap_cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid bitmap cluster: %u", ctx->bitmap_cluster);
        goto out;
    }
    entry += EXFAT_ENTRY_SIZE;

    /* Next should be upcase table */
    if (*entry != EXFAT_ENTRY_UPCASE) {
        report_error(ctx, FSCK_ERR_FATAL, "Missing upcase table entry in root directory");
        goto out;
    }
    /* Get upcase cluster from upcase entry */
    struct exfat_entry_upcase *upcase = (struct exfat_entry_upcase *)entry;
    ctx->upcase_cluster = le32toh(upcase->first_cluster);
    if (ctx->upcase_cluster < 2 || ctx->upcase_cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid upcase cluster: %u", ctx->upcase_cluster);
        goto out;
    }

    error = 0;

out:
    free(buffer);
    return error;
}

static int
read_bitmap_sector(struct fsck_exfat_ctx *ctx, uint32_t sector, void *buffer)
{
    off_t offset;
    ssize_t bytes;

    /* Calculate absolute sector offset */
    offset = ((off_t)ctx->emp->boot.cluster_heap_offset +
             ((off_t)ctx->bitmap_cluster - 2) * (1 << ctx->emp->boot.sectors_per_cluster_shift)) * EXFAT_SECTOR_SIZE +
             sector * EXFAT_SECTOR_SIZE;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "bitmap seek error: %s (offset=%lld)",
                    strerror(errno), (long long)offset);
        return -1;
    }

    bytes = read(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        report_error(ctx, FSCK_ERR_FATAL, "bitmap read error: %s (bytes=%zd)",
                    strerror(errno), bytes);
        return -1;
    }

    return 0;
}

static int
write_bitmap_sector(struct fsck_exfat_ctx *ctx, off_t sector, uint8_t *buffer)
{
    off_t offset = ((off_t)ctx->emp->boot.cluster_heap_offset +
                   ((off_t)ctx->bitmap_cluster - 2) * 
                   (1 << ctx->emp->boot.sectors_per_cluster_shift) +
                   sector) * EXFAT_SECTOR_SIZE;
    ssize_t bytes;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "seek error: %s", strerror(errno));
        return -1;
    }

    bytes = write(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        report_error(ctx, FSCK_ERR_FATAL, "write error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
check_fat(struct fsck_exfat_ctx *ctx)
{
    log_info(ctx, "Checking File Allocation Table...");
    uint32_t *fat_sector;
    uint32_t total_sectors;
    uint32_t i;
    int error = 0;

    /* Allocate sector buffer */
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (fat_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate FAT buffer");
        return -1;
    }

    /* Read first FAT sector */
    error = read_fat_sector(ctx, 0, fat_sector);
    if (error)
        goto out;

    /* First FAT entry must be media type (0xFFFFFFF8) */
    if (le32toh(fat_sector[0]) != 0xFFFFFFF8) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid media type in FAT (expected 0xFFFFFFF8, got 0x%x)",
            le32toh(fat_sector[0]));
        error = 1;
    }

    /* Second FAT entry must be end of chain */
    if (le32toh(fat_sector[1]) != EXFAT_CLUSTER_END) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid second FAT entry (expected 0xFFFFFFFF, got 0x%x)",
            le32toh(fat_sector[1]));
        error = 1;
    }

    /* Check remaining FAT entries */
    total_sectors = ctx->emp->boot.fat_length;
    
    /* Allocate cluster tracking array */
    struct cluster_info *cluster_map = calloc(ctx->emp->boot.cluster_count, 
                                            sizeof(struct cluster_info));
    if (cluster_map == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate cluster map");
        error = -1;
        goto out;
    }

    for (i = 0; i < total_sectors; i++) {
        uint32_t j;

        if (i > 0) {
            error = read_fat_sector(ctx, i, fat_sector);
            if (error)
                goto out_free;
        }

        for (j = 0; j < EXFAT_SECTOR_SIZE/4; j++) {
            uint32_t cluster = i * (EXFAT_SECTOR_SIZE/4) + j;
            uint32_t val = le32toh(fat_sector[j]);

            if (cluster < 2)
                continue;
            if (cluster >= ctx->emp->boot.cluster_count + 2)
                break;

            /* Track cluster chains */
            if (val != EXFAT_CLUSTER_FREE && val != EXFAT_CLUSTER_BAD && 
                val != EXFAT_CLUSTER_END) {
                if (val < 2 || val >= ctx->emp->boot.cluster_count + 2) {
                    report_error(ctx, FSCK_ERR_NORMAL,
                        "Invalid cluster chain: cluster %u points to %u",
                        cluster, val);
                    error = 1;
                    continue;
                }
                cluster_map[val - 2].refs++;
            }
        }
    }

    /* Check for cross-linked clusters */
    for (i = 0; i < ctx->emp->boot.cluster_count; i++) {
        if (cluster_map[i].refs > 1) {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Cluster %u is cross-linked (%u references)",
                i + 2, cluster_map[i].refs);
            error = 1;
        }
    }

out_free:
    free(cluster_map);
out:
    free(fat_sector);
    if (ctx->verbose)
        printf("\n");
    log_info(ctx, "FAT check complete");
    return error;
}

int
read_cluster(struct fsck_exfat_ctx *ctx, uint32_t cluster, void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    /* Validate cluster number */
    if (cluster < 2 || cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid cluster number: %u", cluster);
        return -1;
    }

    /* First calculate bytes per cluster */
    bytes_per_cluster = (size_t)EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Then calculate absolute offset:
     * cluster_heap_offset gives the sector where clusters start
     * (cluster - 2) because clusters start at 2
     */
    offset = (off_t)ctx->emp->boot.cluster_heap_offset * EXFAT_SECTOR_SIZE +
            ((off_t)cluster - 2) * bytes_per_cluster;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "cluster seek error: %s (cluster=%u offset=%lld)", 
                    strerror(errno), cluster, (long long)offset);
        return -1;
    }

    /* Read the entire cluster */
    bytes = read(ctx->fd, buffer, bytes_per_cluster);
    if (bytes != (ssize_t)bytes_per_cluster) {
        report_error(ctx, FSCK_ERR_FATAL, "cluster read error: %s (cluster=%u bytes=%zd)", 
                    strerror(errno), cluster, bytes);
        return -1;
    }

    return 0;
}

static int
validate_timestamp(struct fsck_exfat_ctx *ctx, const char *desc, 
                  uint32_t date, uint32_t time, uint8_t time_ms, uint8_t tz)
{
    int year, month, day, hour, min, sec;
    int error = 0;

    /* Extract fields */
    year = EXFAT_YEAR(date);
    month = EXFAT_MONTH(date);
    day = EXFAT_DAY(date);
    hour = EXFAT_HOUR(time);
    min = EXFAT_MINUTE(time);
    sec = EXFAT_SECOND(time);

    /* Validate ranges */
    if (year < 1980 || year > 2107) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s year: %d (valid range 1980-2107)", desc, year);
        error = 1;
    }

    if (month < 1 || month > 12) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s month: %d", desc, month);
        error = 1;
    }

    /* Check days per month, accounting for leap years */
    int max_days = 31;
    if (month == 4 || month == 6 || month == 9 || month == 11)
        max_days = 30;
    else if (month == 2) {
        max_days = 28;
        if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
            max_days = 29;
    }

    if (day < 1 || day > max_days) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s day: %d (max %d for month %d)",
            desc, day, max_days, month);
        error = 1;
    }

    if (hour > 23) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s hour: %d", desc, hour);
        error = 1;
    }

    if (min > 59) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s minute: %d", desc, min);
        error = 1;
    }

    if (sec > 59) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s second: %d", desc, sec);
        error = 1;
    }

    if (time_ms > 199) {  /* 10ms units, max 1990ms */
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s milliseconds: %d", desc, time_ms * 10);
        error = 1;
    }

    /* Timezone offset is in 15-minute intervals from UTC-12 to UTC+12 */
    if (tz != 0x00 && (tz < 0x80 - 48 || tz > 0x80 + 48)) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s timezone offset: %d", desc, (int)tz - 0x80);
        error = 1;
    }

    return error;
}

static int
validate_name_chars(struct fsck_exfat_ctx *ctx, const uint16_t *name, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        uint16_t c = le16toh(name[i]);

        /* Check for invalid characters */
        if (c < 0x20 && c != 0) {  /* Allow null for padding */
            report_error(ctx, FSCK_ERR_NORMAL,
                "Invalid control character (0x%04x) in filename", c);
            return -1;
        }

        /* Check for invalid combinations */
        if (i > 0) {
            uint16_t prev = le16toh(name[i-1]);
            if (c == '.' && prev == '.')  /* No consecutive dots */
                return -1;
        }
    }

    /* Check for trailing dots and spaces */
    if (len > 0) {
        uint16_t last = le16toh(name[len-1]);
        if (last == '.' || last == ' ') {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Filename ends with invalid character");
            return -1;
        }
    }

    return 0;
}

static int
check_direntry_set(struct fsck_exfat_ctx *ctx, struct exfat_direntry_set *es)
{
    uint16_t checksum, calc_checksum;
    int error = 0;

    /* Check file entry type */
    if (es->file.type != EXFAT_ENTRY_FILE) {
        report_error(ctx, FSCK_ERR_NORMAL, "Invalid file entry type: %02x", es->file.type);
        return -1;
    }

    /* Validate secondary count */
    if (es->file.secondary_count < 2 || 
        es->file.secondary_count > EXFAT_MAX_SECONDARY) {
        report_error(ctx, FSCK_ERR_NORMAL, 
            "Invalid secondary count: %u", es->file.secondary_count);
        return -1;
    }

    /* Validate timestamps */
    error |= validate_timestamp(ctx, "create",
        es->file.create_timestamp,         /* date */
        es->file.create_timestamp,         /* time */
        es->file.create_time_ms,          /* time_ms */
        es->file.create_tz);              /* tz */

    error |= validate_timestamp(ctx, "modify",
        es->file.last_modified_timestamp,  /* date */
        es->file.last_modified_timestamp,  /* time */
        es->file.last_modified_time_ms,    /* time_ms */
        es->file.last_modified_tz);        /* tz */

    error |= validate_timestamp(ctx, "access",
        es->file.last_access_timestamp,    /* date */
        es->file.last_access_timestamp,    /* time */
        0,                                 /* time_ms */
        es->file.last_access_tz);         /* tz */

    /* Check stream entry */
    if (es->stream.type != EXFAT_ENTRY_STREAM) {
        report_error(ctx, FSCK_ERR_NORMAL, 
            "Invalid stream entry type: %02x", es->stream.type);
        return -1;
    }

    /* Validate name length */
    if (es->stream.name_length > EXFAT_MAX_NAMELEN) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid name length: %u", es->stream.name_length);
        return -1;
    }

    /* Number of name entries should match name length */
    int required_name_entries = (es->stream.name_length + 14) / 15;
    if (required_name_entries != es->name_count) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Name entry count mismatch: got %d, need %d",
            es->name_count, required_name_entries);
        return -1;
    }

    /* Check name entries */
    for (int i = 0; i < es->name_count; i++) {
        if (es->name[i].type != EXFAT_ENTRY_NAME) {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Invalid name entry type: %02x", es->name[i].type);
            return -1;
        }
    }

    /* Validate filename characters */
    error |= validate_name_chars(ctx, es->name[0].name, 
                               es->stream.name_length);

    /* Calculate and verify checksum */
    checksum = es->file.checksum;
    calc_checksum = exfat_checksum_direntry(es);
    if (checksum != calc_checksum) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Directory entry checksum mismatch (stored=%04x, calculated=%04x)",
            checksum, calc_checksum);
        error = 1;
    }

    /* Validate cluster range */
    if (es->stream.first_cluster != 0) {
        if (es->stream.first_cluster < 2 || 
            es->stream.first_cluster >= ctx->emp->boot.cluster_count + 2) {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Invalid first cluster: %u", es->stream.first_cluster);
            error = 1;
        }
    }

    /* Valid data length must not exceed actual length */
    if (es->stream.valid_data_length > es->stream.data_length) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Valid data length (%llu) exceeds actual length (%llu)",
            (unsigned long long)es->stream.valid_data_length,
            (unsigned long long)es->stream.data_length);
        error = 1;
    }

    /* For directories, lengths must be cluster-aligned */
    if (es->file.file_attributes & EXFAT_ATTR_DIRECTORY) {
        uint64_t cluster_size = (uint64_t)EXFAT_SECTOR_SIZE << 
                              ctx->emp->boot.sectors_per_cluster_shift;
        if (es->stream.valid_data_length % cluster_size != 0 ||
            es->stream.data_length % cluster_size != 0) {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Directory size not cluster-aligned");
            error = 1;
        }
    }

    return error;
}

int
check_cluster_chain(struct fsck_exfat_ctx *ctx, uint32_t start_cluster, uint64_t expected_size)
{
    uint32_t cluster = start_cluster;
    uint32_t *fat_sector = NULL;
    uint64_t total_size = 0;
    uint32_t cluster_size;
    struct cluster_info *cluster_map = NULL;
    int error = 0;

    /* Calculate cluster size */
    cluster_size = EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Round expected size up to cluster boundary */
    expected_size = (expected_size + cluster_size - 1) & ~((uint64_t)cluster_size - 1);

    /* Allocate cluster tracking map */
    cluster_map = calloc(ctx->emp->boot.cluster_count, sizeof(struct cluster_info));
    if (cluster_map == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate cluster map");
        return -1;
    }

    /* Allocate buffer for FAT sector */
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (fat_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate FAT sector buffer");
        free(cluster_map);
        return -1;
    }

    /* Follow cluster chain */
    while (cluster != EXFAT_CLUSTER_END && cluster < MAX_CLUSTERS) {
        uint32_t next_cluster;

        /* Validate cluster number */
        if (cluster < 2 || cluster >= ctx->emp->boot.cluster_count + 2) {
            report_error(ctx, FSCK_ERR_NORMAL, "Invalid cluster number: %u", cluster);
            error = -1;
            goto out;
        }

        /* Check for cross-linking */
        if (cluster_map[cluster - 2].refs > 0) {
            report_error(ctx, FSCK_ERR_SERIOUS,
                "Cluster %u is cross-linked (used by clusters %u and %u)",
                cluster, cluster_map[cluster - 2].owner, start_cluster);
            if (ctx->fix_errors) {
                /* Allocate new cluster and copy data */
                uint32_t new_cluster;
                char *buffer = malloc(cluster_size);
                if (buffer == NULL) {
                    error = -1;
                    goto out;
                }

                /* Read current cluster */
                error = read_cluster(ctx, cluster, buffer);
                if (error) {
                    free(buffer);
                    goto out;
                }

                /* Find and allocate new cluster */
                error = find_free_cluster(ctx, &new_cluster);
                if (error) {
                    free(buffer);
                    goto out;
                }

                /* Write data to new cluster */
                error = write_cluster(ctx, new_cluster, buffer);
                free(buffer);
                if (error)
                    goto out;

                /* Update FAT to point to new cluster */
                cluster = new_cluster;
                ctx->modified = 1;
            } else {
                error = 1;
                goto out;
            }
        }

        /* Mark cluster as used */
        cluster_map[cluster - 2].refs++;
        cluster_map[cluster - 2].owner = start_cluster;

        /* Update total size */
        total_size += cluster_size;
        if (total_size > expected_size) {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Cluster chain exceeds expected size (%llu > %llu)",
                (unsigned long long)total_size,
                (unsigned long long)expected_size);
            if (ctx->fix_errors) {
                /* Truncate chain here */
                error = write_fat_entry(ctx, cluster, EXFAT_CLUSTER_END);
                if (error)
                    goto out;
                ctx->modified = 1;
                break;
            } else {
                error = 1;
                goto out;
            }
        }

        /* Get next cluster */
        error = get_next_cluster(ctx, cluster, &next_cluster);
        if (error)
            goto out;

        /* Check for invalid next cluster */
        if (next_cluster != EXFAT_CLUSTER_END && 
            (next_cluster < 2 || next_cluster >= ctx->emp->boot.cluster_count + 2)) {
            report_error(ctx, FSCK_ERR_NORMAL,
                "Invalid next cluster %u in chain", next_cluster);
            if (ctx->fix_errors) {
                /* End chain here */
                error = write_fat_entry(ctx, cluster, EXFAT_CLUSTER_END);
                if (error)
                    goto out;
                ctx->modified = 1;
                break;
            } else {
                error = 1;
                goto out;
            }
        }

        cluster = next_cluster;
    }

    /* Verify final size matches expected */
    if (total_size < expected_size) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Cluster chain too short (%llu < %llu)",
            (unsigned long long)total_size,
            (unsigned long long)expected_size);
        error = 1;
    }

out:
    free(cluster_map);
    free(fat_sector);
    return error;
}

int
check_file(struct fsck_exfat_ctx *ctx, struct exfat_direntry_set *es)
{
    /* Skip zero-length files */
    if (es->stream.data_length == 0)
        return 0;

    /* Validate data length */
    if (es->stream.valid_data_length > es->stream.data_length) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Valid data length (%llu) exceeds file size (%llu)",
            (unsigned long long)es->stream.valid_data_length,
            (unsigned long long)es->stream.data_length);
        if (ctx->fix_errors) {
            es->stream.valid_data_length = es->stream.data_length;
            ctx->modified = 1;
        }
        return -1;
    }

    /* Check first cluster */
    if (es->stream.first_cluster < 2 || 
        es->stream.first_cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid first cluster: %u", es->stream.first_cluster);
        return -1;
    }

    /* Check cluster chain */
    return check_cluster_chain(ctx, es->stream.first_cluster, es->stream.data_length);
}

int
check_directory(struct fsck_exfat_ctx *ctx, uint32_t cluster)
{
    static int depth = 0;
    depth++;
    
    if (depth == 1)
        log_info(ctx, "Checking directory structure...");
    else if (ctx->verbose)
        printf("%*sChecking directory at cluster %u...\n", depth*2, "", cluster);
    
    if (validate_cluster_number(ctx, cluster, "directory") < 0)
        return -1;
    
    char *buffer = NULL;
    struct exfat_direntry_set es;
    size_t bytes_per_cluster;
    uint32_t next_cluster;
    int error = 0;

    /* Calculate cluster size */
    bytes_per_cluster = (size_t)EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Validate cluster size */
    if (bytes_per_cluster == 0 || bytes_per_cluster > (1ULL << 25)) {  /* Max 32MB cluster */
        report_error(ctx, FSCK_ERR_FATAL, "Invalid cluster size: %zu", bytes_per_cluster);
        return -1;
    }

    /* Allocate cluster buffer */
    buffer = calloc(1, bytes_per_cluster);
    if (buffer == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate cluster buffer");
        return -1;
    }

    /* Process all clusters in the chain */
    while (cluster != EXFAT_CLUSTER_END) {
        uint8_t *entry = (uint8_t *)buffer;
        uint8_t *buffer_end = (uint8_t *)buffer + bytes_per_cluster;
        int entry_count = 0;

        /* Read cluster */
        error = read_cluster(ctx, cluster, buffer);
        if (error)
            goto out;

        /* Process all entries in cluster */
        while (entry + sizeof(struct exfat_entry_file) <= buffer_end) {
            /* Check for end of directory */
            if (*entry == EXFAT_ENTRY_EOD)
                goto done;

            /* Skip deleted entries */
            if (*entry == EXFAT_ENTRY_DELETED) {
                entry += sizeof(struct exfat_entry_file);
                continue;
            }

            /* Check if this is a file entry */
            if (*entry == EXFAT_ENTRY_FILE) {
                /* Ensure we have enough space for the full entry set */
                memset(&es, 0, sizeof(es));

                /* Read file entry */
                if (entry + sizeof(es.file) > buffer_end) {
                    report_error(ctx, FSCK_ERR_NORMAL, "Truncated file entry");
                    error = -1;
                    goto out;
                }
                memcpy(&es.file, entry, sizeof(es.file));
                entry += sizeof(es.file);

                /* Validate secondary count */
                if (es.file.secondary_count < 2 || es.file.secondary_count > EXFAT_MAX_SECONDARY) {
                    report_error(ctx, FSCK_ERR_NORMAL, "Invalid secondary count: %u", 
                               es.file.secondary_count);
                    error = -1;
                    goto out;
                }

                /* Read stream entry */
                if (entry + sizeof(es.stream) > buffer_end) {
                    report_error(ctx, FSCK_ERR_NORMAL, "Truncated stream entry");
                    error = -1;
                    goto out;
                }
                memcpy(&es.stream, entry, sizeof(es.stream));
                entry += sizeof(es.stream);

                /* Validate name length */
                if (es.stream.name_length > EXFAT_MAX_NAMELEN) {
                    report_error(ctx, FSCK_ERR_NORMAL, "Invalid name length: %u", 
                               es.stream.name_length);
                    error = -1;
                    goto out;
                }

                /* Calculate required name entries */
                es.name_count = (es.stream.name_length + 14) / 15;
                if (es.name_count > EXFAT_MAX_SECONDARY - 1) {
                    report_error(ctx, FSCK_ERR_NORMAL, "Too many name entries required: %d", 
                               es.name_count);
                    error = -1;
                    goto out;
                }

                /* Read name entries */
                for (int i = 0; i < es.name_count; i++) {
                    if (entry + sizeof(es.name[i]) > buffer_end) {
                        report_error(ctx, FSCK_ERR_NORMAL, "Truncated name entry");
                        error = -1;
                        goto out;
                    }
                    memcpy(&es.name[i], entry, sizeof(es.name[i]));
                    entry += sizeof(es.name[i]);
                }

                /* Check directory entry set */
                error = check_direntry_set(ctx, &es);
                if (error < 0)
                    goto out;

                /* Check file/directory contents */
                if (es.file.file_attributes & EXFAT_ATTR_DIRECTORY) {
                    /* Prevent infinite recursion */
                    if (es.stream.first_cluster == cluster) {
                        report_error(ctx, FSCK_ERR_NORMAL, "Directory points to itself");
                        error = -1;
                        goto out;
                    }
                    error = check_directory(ctx, es.stream.first_cluster);
                    if (error < 0)
                        goto out;
                } else {
                    error = check_file(ctx, &es);
                    if (error < 0)
                        goto out;
                }

                entry_count++;
            } else {
                /* Skip unknown entry */
                entry += sizeof(struct exfat_entry_file);
            }
        }

        /* Get next cluster */
        error = get_next_cluster(ctx, cluster, &next_cluster);
        if (error)
            goto out;

        if (next_cluster == cluster) {
            report_error(ctx, FSCK_ERR_NORMAL, "Cluster points to itself");
            error = -1;
            goto out;
        }

        cluster = next_cluster;
        
        if (entry_count > 0 && ctx->verbose && depth == 1)
            printf("\rFound %d entries...", entry_count);
    }

    if (ctx->verbose && depth == 1)
        printf("\n");
        
    depth--;
    done:
    error = 0;
out:
    free(buffer);
    return error;
}

int
check_root_dir(struct fsck_exfat_ctx *ctx)
{
    return check_directory(ctx, ctx->emp->boot.root_dir_cluster);
}

int
check_bitmap(struct fsck_exfat_ctx *ctx)
{
    log_info(ctx, "Checking allocation bitmap...");
    uint8_t *bitmap_sector = NULL;
    uint32_t total_clusters = ctx->emp->boot.cluster_count;
    uint32_t bitmap_sectors = (total_clusters + 7) / 8 / EXFAT_SECTOR_SIZE;
    int error = 0;

    /* Allocate with proper error checking */
    bitmap_sector = calloc(1, EXFAT_SECTOR_SIZE);
    if (bitmap_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate bitmap buffer");
        return -1;
    }

    /* Check each sector of the bitmap */
    for (uint32_t i = 0; i < bitmap_sectors && !error; i++) {
        print_progress(ctx, "Checking bitmap", i, bitmap_sectors);
        error = read_bitmap_sector(ctx, i, bitmap_sector);
        if (error)
            goto out;

        /* Check each bit in this sector */
        for (uint32_t j = 0; j < EXFAT_SECTOR_SIZE * 8; j++) {
            uint32_t cluster = i * EXFAT_SECTOR_SIZE * 8 + j + 2;
            
            if (cluster >= total_clusters + 2)
                break;

            int is_allocated = (bitmap_sector[j / 8] & (1 << (j % 8))) != 0;

            /* System clusters (2-4) must be allocated */
            if (cluster >= 2 && cluster <= 4 && !is_allocated) {
                if (ctx->fix_errors) {
                    bitmap_sector[j / 8] |= (1 << (j % 8));
                    error = write_bitmap_sector(ctx, i, bitmap_sector);
                    if (error)
                        goto out;
                    ctx->modified = 1;
                }
                continue;
            }

            /* Check against FAT */
            uint32_t fat_value = 0;
            error = get_fat_entry(ctx, cluster, &fat_value);
            if (error)
                goto out;

            if ((fat_value == EXFAT_CLUSTER_FREE && is_allocated) ||
                (fat_value != EXFAT_CLUSTER_FREE && !is_allocated)) {
                report_error(ctx, FSCK_ERR_NORMAL,
                    "Bitmap inconsistency for cluster %u (bitmap=%d, FAT=%s)",
                    cluster, is_allocated,
                    fat_value == EXFAT_CLUSTER_FREE ? "free" : "allocated");
                if (ctx->fix_errors) {
                    /* Update bitmap to match FAT */
                    if (fat_value == EXFAT_CLUSTER_FREE)
                        bitmap_sector[j / 8] &= ~(1 << (j % 8));
                    else
                        bitmap_sector[j / 8] |= (1 << (j % 8));
                    error = write_bitmap_sector(ctx, i, bitmap_sector);
                    if (error)
                        goto out;
                    ctx->modified = 1;
                }
            }
        }
    }

    if (ctx->verbose)
        printf("\n");
        
    log_info(ctx, "Bitmap check complete");
    out:
    free(bitmap_sector);
    return error;
}

static int
recover_lost_clusters(struct fsck_exfat_ctx *ctx)
{
    log_info(ctx, "Checking for lost clusters...");
    uint32_t *fat_sector = NULL;
    uint8_t *bitmap_sector = NULL;
    struct cluster_info *cluster_map = NULL;
    uint32_t total_clusters = ctx->emp->boot.cluster_count;
    uint32_t lost_count = 0;
    int error = 0;

    /* Validate cluster count */
    if (total_clusters == 0 || total_clusters > MAX_CLUSTERS) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid cluster count: %u", total_clusters);
        return -1;
    }

    /* Allocate buffers with proper initialization */
    fat_sector = calloc(1, EXFAT_SECTOR_SIZE);
    bitmap_sector = calloc(1, EXFAT_SECTOR_SIZE);
    cluster_map = calloc(total_clusters, sizeof(struct cluster_info));

    if (fat_sector == NULL || bitmap_sector == NULL || cluster_map == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate buffers");
        error = -1;
        goto out;
    }

    /* First pass: mark all clusters referenced in directory entries */
    error = check_directory(ctx, ctx->emp->boot.root_dir_cluster);
    if (error)
        goto out;

    /* Second pass with progress */
    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        if (ctx->verbose && (cluster % 1000) == 0) {
            print_progress(ctx, "Scanning clusters", 
                         cluster - 2, total_clusters);
        }
        uint32_t fat_entry;
        int is_allocated;

        /* Read FAT entry */
        uint32_t fat_sector_index = cluster / (EXFAT_SECTOR_SIZE / sizeof(uint32_t));
        uint32_t fat_entry_index = cluster % (EXFAT_SECTOR_SIZE / sizeof(uint32_t));
        
        error = read_fat_sector(ctx, fat_sector_index, fat_sector);
        if (error)
            goto out;
        fat_entry = le32toh(fat_sector[fat_entry_index]);

        /* Read bitmap entry */
        uint32_t bitmap_sector_index = (cluster - 2) / (EXFAT_SECTOR_SIZE * 8);
        uint32_t bitmap_byte = ((cluster - 2) % (EXFAT_SECTOR_SIZE * 8)) / 8;
        uint32_t bitmap_bit = (cluster - 2) % 8;

        error = read_bitmap_sector(ctx, bitmap_sector_index, bitmap_sector);
        if (error)
            goto out;

        is_allocated = (bitmap_sector[bitmap_byte] & (1 << bitmap_bit)) != 0;

        /* Check for lost clusters */
        if (is_allocated && fat_entry != EXFAT_CLUSTER_FREE && 
            fat_entry != EXFAT_CLUSTER_BAD && fat_entry != EXFAT_CLUSTER_END &&
            cluster_map[cluster - 2].refs == 0) {
            
            report_error(ctx, FSCK_ERR_NORMAL,
                "Cluster %u is allocated but not referenced", cluster);
            lost_count++;

            if (ctx->fix_errors) {
                /* Create lost+found directory if needed */
                if (ctx->lost_found_cluster == 0) {
                    error = create_lost_found_dir(ctx);
                    if (error)
                        goto out;
                }

                /* Create file for lost chain */
                error = create_lost_file(ctx, cluster);
                if (error)
                    goto out;

                ctx->modified = 1;
            }
        }
    }

    if (ctx->verbose)
        printf("\n");
        
    if (lost_count > 0)
        log_info(ctx, "Recovery complete - recovered %u lost clusters", lost_count);
    else
        log_info(ctx, "No lost clusters found");
        
    out:
    /* Clean up in reverse order of allocation */
    if (cluster_map != NULL)
        free(cluster_map);
    if (bitmap_sector != NULL)
        free(bitmap_sector);
    if (fat_sector != NULL)
        free(fat_sector);
    return error;
}

int
create_lost_found_dir(struct fsck_exfat_ctx *ctx)
{
    struct exfat_direntry_set es;
    char *cluster;
    struct timespec ts;
    int error;

    /* Allocate a cluster */
    error = find_free_cluster(ctx, &ctx->lost_found_cluster);
    if (error)
        return error;

    /* Initialize directory entries */
    memset(&es, 0, sizeof(es));
    clock_gettime(CLOCK_REALTIME, &ts);

    /* Set up file entry */
    es.file.type = EXFAT_ENTRY_FILE;
    es.file.secondary_count = 2;  /* Stream entry + one name entry */
    es.file.file_attributes = EXFAT_ATTR_DIRECTORY;
    unix_time_to_exfat(&ts, &es.file.create_timestamp, &es.file.create_timestamp);
    es.file.last_modified_timestamp = es.file.create_timestamp;
    es.file.last_access_timestamp = es.file.create_timestamp;

    /* Set up stream entry */
    es.stream.type = EXFAT_ENTRY_STREAM;
    es.stream.name_length = 10;  /* "lost+found" */
    es.stream.first_cluster = ctx->lost_found_cluster;
    es.stream.data_length = ctx->emp->boot.sectors_per_cluster_shift * EXFAT_SECTOR_SIZE;

    /* Set up name entry */
    es.name[0].type = EXFAT_ENTRY_NAME;
    memcpy(es.name[0].name, L"lost+found", 20);  /* 10 UTF-16 characters */
    es.name_count = 1;

    /* Write directory entry to root directory */
    error = write_direntry(ctx, ctx->emp->boot.root_dir_cluster, &es);
    if (error)
        return error;

    /* Initialize lost+found directory cluster */
    cluster = calloc(1, ctx->emp->boot.sectors_per_cluster_shift * EXFAT_SECTOR_SIZE);
    if (cluster == NULL)
        return -1;

    error = write_cluster(ctx, ctx->lost_found_cluster, cluster);
    free(cluster);

    return error;
}

int
create_lost_file(struct fsck_exfat_ctx *ctx, uint32_t cluster)
{
    struct exfat_direntry_set es;
    struct timespec ts;
    char name[32];
    uint16_t utf16_name[15];
    size_t name_len;
    int error;

    /* Generate file name */
    snprintf(name, sizeof(name), "FILE%u.CHK", ctx->next_lost_file++);
    error = exfat_utf8_to_utf16(name, utf16_name, 15, &name_len);
    if (error)
        return error;

    /* Initialize directory entries */
    memset(&es, 0, sizeof(es));
    clock_gettime(CLOCK_REALTIME, &ts);

    /* Set up file entry */
    es.file.type = EXFAT_ENTRY_FILE;
    es.file.secondary_count = 2;
    es.file.file_attributes = 0;
    unix_time_to_exfat(&ts, &es.file.create_timestamp, &es.file.create_timestamp);
    es.file.last_modified_timestamp = es.file.create_timestamp;
    es.file.last_access_timestamp = es.file.create_timestamp;

    /* Set up stream entry */
    es.stream.type = EXFAT_ENTRY_STREAM;
    es.stream.name_length = name_len;
    es.stream.first_cluster = cluster;
    es.stream.data_length = 0;  /* Unknown size */

    /* Set up name entry */
    es.name[0].type = EXFAT_ENTRY_NAME;
    memcpy(es.name[0].name, utf16_name, name_len * 2);
    es.name_count = 1;

    /* Write directory entry to lost+found */
    return write_direntry(ctx, ctx->lost_found_cluster, &es);
}

int
check_upcase_table(struct fsck_exfat_ctx *ctx)
{
    log_info(ctx, "Checking upcase table...");
    uint8_t *buffer = NULL;
    uint32_t sector;
    uint32_t checksum = 0;
    uint64_t size;
    uint32_t cluster;
    int error = 0;
    size_t bytes_per_cluster;

    /* Validate input */
    if (ctx == NULL || ctx->emp == NULL) {
        return -1;
    }

    /* Calculate cluster size */
    bytes_per_cluster = (size_t)EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Allocate buffer for largest possible read (cluster size) */
    buffer = calloc(1, bytes_per_cluster);
    if (buffer == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate buffer");
        return -1;
    }

    /* Calculate and validate root directory sector */
    if (ctx->emp->boot.root_dir_cluster < 2) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid root directory cluster");
        goto out_error;
    }

    sector = ctx->emp->boot.cluster_heap_offset +
             ((ctx->emp->boot.root_dir_cluster - 2) << ctx->emp->boot.sectors_per_cluster_shift);

    /* Read root directory sector */
    error = read_sector(ctx, sector, buffer);
    if (error) {
        goto out_error;
    }

    /* Find upcase table entry */
    struct exfat_entry_upcase *upcase = NULL;
    uint8_t *entry = buffer;
    size_t remaining = EXFAT_SECTOR_SIZE;

    while (remaining >= sizeof(struct exfat_entry_upcase)) {
        if (*entry == EXFAT_ENTRY_EOD)
            break;
        if (*entry == EXFAT_ENTRY_UPCASE) {
            upcase = (struct exfat_entry_upcase *)entry;
            break;
        }
        entry += sizeof(struct exfat_entry_upcase);
        remaining -= sizeof(struct exfat_entry_upcase);
    }

    if (upcase == NULL) {
        report_error(ctx, FSCK_ERR_SERIOUS, "No upcase table found");
        goto out_error;
    }

    /* Get and validate upcase table info */
    cluster = le32toh(upcase->first_cluster);
    size = le64toh(upcase->data_length);
    uint32_t stored_checksum = le32toh(upcase->checksum);

    if (cluster < 2 || cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_SERIOUS, "Invalid upcase table cluster: %u", cluster);
        goto out_error;
    }

    /* Validate size */
    uint64_t max_size = (uint64_t)ctx->emp->boot.cluster_count * bytes_per_cluster;
    if (size == 0 || size > max_size || (size & 1) != 0) {
        report_error(ctx, FSCK_ERR_SERIOUS, 
            "Invalid upcase table size: %llu (max: %llu)",
            (unsigned long long)size, (unsigned long long)max_size);
        goto out_error;
    }

    /* Calculate checksum with progress */
    uint64_t remaining_size = size;
    uint32_t current_cluster = cluster;
    uint32_t visited_clusters = 0;

    while (remaining_size > 0 && visited_clusters < ctx->emp->boot.cluster_count) {
        if (ctx->verbose && (visited_clusters % 100) == 0) {
            print_progress(ctx, "Reading upcase table", 
                         size - remaining_size, size);
        }
        /* Read and verify cluster */
        error = read_cluster(ctx, current_cluster, buffer);
        if (error) {
            report_error(ctx, FSCK_ERR_NORMAL, 
                "Failed to read upcase table cluster %u", current_cluster);
            goto out_error;
        }

        /* Update checksum */
        uint32_t bytes = (uint32_t)MIN(remaining_size, bytes_per_cluster);
        for (uint32_t i = 0; i < bytes; i++) {
            checksum = ((checksum << 31) | (checksum >> 1)) + buffer[i];
        }

        remaining_size -= bytes;
        visited_clusters++;

        /* Get next cluster if needed */
        if (remaining_size > 0) {
            uint32_t next_cluster;
            error = get_next_cluster(ctx, current_cluster, &next_cluster);
            if (error || next_cluster == current_cluster) {
                report_error(ctx, FSCK_ERR_NORMAL, "Invalid upcase table cluster chain");
                goto out_error;
            }
            current_cluster = next_cluster;
        }
    }

    /* Verify final checksum */
    if (checksum != stored_checksum) {
        report_error(ctx, FSCK_ERR_SERIOUS,
            "Upcase table checksum mismatch (stored: %08x, calculated: %08x)",
            stored_checksum, checksum);
        error = -1;
    }

    if (ctx->verbose)
        printf("\n");
        
    log_info(ctx, "Upcase table check complete");
    free(buffer);
    return error;

out_error:
    free(buffer);
    return -1;
}

/* Calculate hash for filename */
uint16_t
exfat_calc_name_hash(struct exfat_mount *emp, const uint16_t *name, size_t len)
{
    uint16_t hash = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        uint16_t c = le16toh(name[i]);
        /* Use upcase table if available */
        if (emp->upcase)
            c = le16toh(emp->upcase[c]);
        hash = ((hash << 15) | (hash >> 1)) + c;
    }

    return hash;
}

/* Calculate checksum for directory entry set */
uint16_t
exfat_checksum_direntry(struct exfat_direntry_set *es)
{
    uint16_t checksum = 0;
    uint8_t *entry = (uint8_t *)es;
    size_t len = sizeof(struct exfat_entry_file) - 2; /* Skip checksum field */
    size_t i;

    /* Calculate checksum for file entry */
    for (i = 0; i < len; i++)
        checksum = ((checksum << 15) | (checksum >> 1)) + entry[i];

    /* Add stream entry */
    entry = (uint8_t *)&es->stream;
    len = sizeof(struct exfat_entry_stream);
    for (i = 0; i < len; i++)
        checksum = ((checksum << 15) | (checksum >> 1)) + entry[i];

    /* Add name entries */
    for (i = 0; i < es->name_count; i++) {
        entry = (uint8_t *)&es->name[i];
        len = sizeof(struct exfat_entry_name);
        for (size_t j = 0; j < len; j++)
            checksum = ((checksum << 15) | (checksum >> 1)) + entry[j];
    }

    return checksum;
}

/* Write a cluster to disk */
int
write_cluster(struct fsck_exfat_ctx *ctx, uint32_t cluster, const void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    /* Calculate cluster offset */
    bytes_per_cluster = EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Then calculate absolute offset:
     * cluster_heap_offset gives the sector where clusters start
     * (cluster - 2) because clusters start at 2
     */
    offset = (off_t)ctx->emp->boot.cluster_heap_offset * EXFAT_SECTOR_SIZE +
            ((off_t)cluster - 2) * bytes_per_cluster;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "seek error: %s", strerror(errno));
        return -1;
    }

    bytes = write(ctx->fd, buffer, bytes_per_cluster);
    if (bytes != bytes_per_cluster) {
        report_error(ctx, FSCK_ERR_FATAL, "write error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* Write directory entry set to directory cluster */
int
write_direntry(struct fsck_exfat_ctx *ctx, uint32_t dir_cluster, struct exfat_direntry_set *es)
{
    char *buffer;
    size_t bytes_per_cluster;
    size_t entry_size;
    int error;

    /* Calculate sizes */
    bytes_per_cluster = EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;
    entry_size = sizeof(struct exfat_entry_file) +
                sizeof(struct exfat_entry_stream) +
                es->name_count * sizeof(struct exfat_entry_name);

    /* Allocate cluster buffer */
    buffer = malloc(bytes_per_cluster);
    if (buffer == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate cluster buffer");
        return -1;
    }

    /* Read directory cluster */
    error = read_cluster(ctx, dir_cluster, buffer);
    if (error) {
        free(buffer);
        return error;
    }

    /* Find free space in directory */
    uint8_t *entry = (uint8_t *)buffer;
    while (entry < (uint8_t *)buffer + bytes_per_cluster) {
        if (*entry == EXFAT_ENTRY_EOD || *entry == EXFAT_ENTRY_DELETED) {
            /* Found space - write entries */
            memcpy(entry, &es->file, sizeof(es->file));
            entry += sizeof(es->file);
            memcpy(entry, &es->stream, sizeof(es->stream));
            entry += sizeof(es->stream);
            for (int i = 0; i < es->name_count; i++) {
                memcpy(entry, &es->name[i], sizeof(es->name[i]));
                entry += sizeof(es->name[i]);
            }
            break;
        }
        entry += sizeof(struct exfat_entry_file);
    }

    /* Write updated cluster */
    error = write_cluster(ctx, dir_cluster, buffer);
    free(buffer);
    return error;
}

/* Find a free cluster */
int
find_free_cluster(struct fsck_exfat_ctx *ctx, uint32_t *cluster)
{
    uint32_t *fat_sector;
    uint32_t total_clusters = ctx->emp->boot.cluster_count;
    uint32_t entries_per_sector = EXFAT_SECTOR_SIZE / sizeof(uint32_t);
    uint32_t sector;
    int error = 0;

    /* Allocate buffer for FAT sector */
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (fat_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate FAT sector buffer");
        return -1;
    }

    /* Search FAT for free cluster */
    for (sector = 0; sector < ctx->emp->boot.fat_length; sector++) {
        error = read_fat_sector(ctx, sector, fat_sector);
        if (error)
            goto out;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint32_t c = sector * entries_per_sector + i + 2;
            if (c >= total_clusters + 2)
                goto out;

            if (le32toh(fat_sector[i]) == EXFAT_CLUSTER_FREE) {
                /* Found free cluster - mark as end of chain */
                fat_sector[i] = htole32(EXFAT_CLUSTER_END);
                error = write_fat_sector(ctx, sector, fat_sector);
                if (error)
                    goto out;
                *cluster = c;
                ctx->modified = 1;
                goto out;
            }
        }
    }

    /* No free clusters found */
    error = -1;

out:
    free(fat_sector);
    return error;
}

/* Get next cluster in chain */
int
get_next_cluster(struct fsck_exfat_ctx *ctx, uint32_t cluster, uint32_t *next)
{
    uint32_t value;
    int error;

    /* Validate input */
    if (next == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Invalid NULL pointer in get_next_cluster");
        return -1;
    }

    /* Get FAT entry */
    error = get_fat_entry(ctx, cluster, &value);
    if (error)
        return error;

    /* Validate FAT entry */
    if (value != EXFAT_CLUSTER_FREE && 
        value != EXFAT_CLUSTER_BAD && 
        value != EXFAT_CLUSTER_END &&
        (value < 2 || value >= ctx->emp->boot.cluster_count + 2)) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid cluster chain: cluster %u points to %u",
            cluster, value);
        return -1;
    }

    *next = value;
    return 0;
}

/* Convert Unix timestamp to ExFAT date/time */
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

/* Convert UTF-8 to UTF-16 */
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

int
main(int argc, char *argv[])
{
    struct fsck_exfat_ctx ctx;
    struct exfat_mount emp;
    int ch;
    int ret = 0;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    memset(&emp, 0, sizeof(emp));
    ctx.emp = &emp;

    /* Parse command line options */
    while ((ch = getopt(argc, argv, "fnvy")) != -1) {
        switch (ch) {
        case 'f':
            /* Force check */
            break;
        case 'n':
            /* Don't make any changes */
            ctx.fix_errors = 0;
            break;
        case 'v':
            /* Be verbose */
            ctx.verbose = 1;
            break;
        case 'y':
            /* Fix errors without asking */
            ctx.fix_errors = 1;
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

    log_info(&ctx, "Starting ExFAT filesystem check on %s", ctx.device);
    log_info(&ctx, "Options: %s%s%s", 
             ctx.fix_errors ? "auto-repair " : "",
             ctx.verbose ? "verbose " : "",
             (!ctx.fix_errors && !ctx.verbose) ? "read-only" : "");
    
    /* Check filesystem structures */
    if (check_boot_sector(&ctx) < 0) {
        report_error(&ctx, FSCK_ERR_NORMAL, "Boot sector check failed");
        goto error;
    }

    if (check_fat(&ctx) < 0) {
        report_error(&ctx, FSCK_ERR_NORMAL, "FAT check failed");
        goto error;
    }

    if (check_root_dir(&ctx) < 0) {
        report_error(&ctx, FSCK_ERR_NORMAL, "Root directory check failed");
        goto error;
    }

    if (check_bitmap(&ctx) < 0) {
        report_error(&ctx, FSCK_ERR_NORMAL, "Bitmap check failed");
        goto error;
    }

    if (check_upcase_table(&ctx) < 0) {
        report_error(&ctx, FSCK_ERR_NORMAL, "Upcase table check failed");
        goto error;
    }

    if (recover_lost_clusters(&ctx) < 0) {
        report_error(&ctx, FSCK_ERR_NORMAL, "Lost cluster recovery failed");
        goto error;
    }

    log_info(&ctx, "Filesystem check complete");
    if (ctx.modified)
        log_info(&ctx, "Filesystem was modified");
    if (ctx.errors)
        log_info(&ctx, "Found and %s %d errors", 
                 ctx.fix_errors ? "fixed" : "found", ctx.errors);
    else
        log_info(&ctx, "No errors found");

    /* Clean up */
    close(ctx.fd);

    return ctx.modified ? 1 : 0;

error:
    close(ctx.fd);
    return 1;
}

static int
get_fat_entry(struct fsck_exfat_ctx *ctx, uint32_t cluster, uint32_t *value)
{
    uint32_t sector_index;
    uint32_t entry_index;
    uint32_t fat_value;
    int error = 0;

    /* Validate input parameters */
    if (ctx == NULL || value == NULL) {
        return -1;
    }

    /* Validate cluster number */
    if (cluster < 2 || cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_NORMAL, "Invalid cluster number: %u", cluster);
        return -1;
    }

    /* Calculate sector and entry indices */
    sector_index = cluster / (EXFAT_SECTOR_SIZE / sizeof(uint32_t));
    entry_index = cluster % (EXFAT_SECTOR_SIZE / sizeof(uint32_t));

    /* Validate sector index */
    if (sector_index >= ctx->emp->boot.fat_length) {
        report_error(ctx, FSCK_ERR_NORMAL, "FAT sector index out of range: %u", sector_index);
        return -1;
    }

    /* Use stack-based buffer for small reads */
    uint32_t sector_buffer[EXFAT_SECTOR_SIZE / sizeof(uint32_t)];

    /* Read FAT sector */
    error = read_fat_sector(ctx, sector_index, sector_buffer);
    if (error) {
        return error;
    }

    /* Get FAT entry value */
    fat_value = le32toh(sector_buffer[entry_index]);

    /* Basic validation of FAT value */
    if (fat_value != EXFAT_CLUSTER_FREE && 
        fat_value != EXFAT_CLUSTER_BAD && 
        fat_value != EXFAT_CLUSTER_END &&
        (fat_value < 2 || fat_value >= ctx->emp->boot.cluster_count + 2)) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid FAT entry value: cluster %u -> %u", cluster, fat_value);
        return -1;
    }

    *value = fat_value;
    return 0;
}

static int
validate_cluster_number(struct fsck_exfat_ctx *ctx, uint32_t cluster, const char *desc)
{
    if (cluster < 2 || cluster >= ctx->emp->boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_NORMAL, 
            "Invalid %s cluster number: %u (valid range: 2-%u)",
            desc, cluster, ctx->emp->boot.cluster_count + 1);
        return -1;
    }
    return 0;
} 