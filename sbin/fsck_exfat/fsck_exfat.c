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
#include "exfat.h"

#define MAX_CLUSTERS 0xFFFFFFF5  /* Maximum valid cluster number */

/* Add cluster tracking structure */
struct cluster_info {
    uint32_t refs;      /* Reference count */
    uint32_t owner;     /* First cluster of file/dir that owns this cluster */
};

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
read_fat_sector(struct fsck_exfat_ctx *ctx, off_t sector, uint32_t *buffer)
{
    off_t offset = (ctx->emp->boot.fat_offset + sector) * EXFAT_SECTOR_SIZE;
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

int
check_boot_sector(struct fsck_exfat_ctx *ctx)
{
    struct exfat_boot_record boot;
    uint8_t *sector = (uint8_t *)&boot;
    int i;

    /* Read boot sector */
    if (read_sector(ctx, 0, &boot) < 0)
        return -1;

    /* Check jump instruction */
    if (boot.jump_boot[0] != 0xEB || boot.jump_boot[2] != 0x90) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid jump instruction in boot sector");
        return -1;
    }

    /* Check filesystem name */
    if (memcmp(boot.fs_name, "EXFAT   ", 8) != 0) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid filesystem name in boot sector");
        return -1;
    }

    /* Check must_be_zero field */
    for (i = 0; i < 53; i++) {
        if (boot.must_be_zero[i] != 0) {
            report_error(ctx, FSCK_ERR_SERIOUS,
                "Non-zero value in reserved field");
            if (ctx->fix_errors) {
                boot.must_be_zero[i] = 0;
                ctx->modified = 1;
            }
        }
    }

    /* Check bytes per sector (must be power of 2, 512-4096) */
    uint32_t bytes_per_sector = 1 << boot.bytes_per_sector_shift;
    if (boot.bytes_per_sector_shift < 9 || boot.bytes_per_sector_shift > 12) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid bytes per sector: %u", bytes_per_sector);
        return -1;
    }

    /* Check sectors per cluster (must be power of 2) */
    if (boot.sectors_per_cluster_shift > 25 - boot.bytes_per_sector_shift) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid sectors per cluster: %u",
            1 << boot.sectors_per_cluster_shift);
        return -1;
    }

    /* Check number of FATs (must be 1 or 2) */
    if (boot.number_of_fats < 1 || boot.number_of_fats > 2) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid number of FATs: %u", boot.number_of_fats);
        return -1;
    }

    /* Check volume length */
    if (boot.volume_length == 0) {
        report_error(ctx, FSCK_ERR_FATAL, "Volume length is zero");
        return -1;
    }

    /* Check FAT offset and length */
    if (boot.fat_offset == 0 || boot.fat_length == 0) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid FAT offset/length: %u/%u",
            boot.fat_offset, boot.fat_length);
        return -1;
    }

    /* Check cluster heap offset */
    if (boot.cluster_heap_offset < boot.fat_offset + boot.fat_length) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Cluster heap overlaps with FAT");
        return -1;
    }

    /* Check cluster count */
    if (boot.cluster_count == 0) {
        report_error(ctx, FSCK_ERR_FATAL, "Cluster count is zero");
        return -1;
    }

    /* Check root directory cluster */
    if (boot.root_dir_cluster < 2 ||
        boot.root_dir_cluster >= boot.cluster_count + 2) {
        report_error(ctx, FSCK_ERR_FATAL,
            "Invalid root directory cluster: %u", boot.root_dir_cluster);
        return -1;
    }

    /* Store boot sector in context */
    memcpy(&ctx->emp->boot, &boot, sizeof(boot));

    return 0;
}

int
check_fat(struct fsck_exfat_ctx *ctx)
{
    uint32_t *fat_sector;
    uint32_t total_clusters = ctx->emp->boot.cluster_count;
    uint32_t entries_per_sector = EXFAT_SECTOR_SIZE / sizeof(uint32_t);
    uint32_t sector, i;
    int error = 0;

    /* Allocate buffer for one FAT sector */
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (fat_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate FAT sector buffer");
        return -1;
    }

    /* Check each FAT sector */
    for (sector = 0; sector < ctx->emp->boot.fat_length; sector++) {
        error = read_fat_sector(ctx, sector, fat_sector);
        if (error)
            goto out;

        /* Check entries in this sector */
        for (i = 0; i < entries_per_sector; i++) {
            uint32_t cluster = sector * entries_per_sector + i;
            uint32_t entry = le32toh(fat_sector[i]);

            /* Skip if beyond total clusters */
            if (cluster >= total_clusters)
                continue;

            /* Check for valid cluster values */
            if (entry != EXFAT_CLUSTER_FREE &&
                entry != EXFAT_CLUSTER_BAD &&
                entry != EXFAT_CLUSTER_END &&
                (entry < 2 || entry >= total_clusters + 2)) {
                report_error(ctx, FSCK_ERR_NORMAL,
                    "Invalid cluster value %u in FAT entry %u",
                    entry, cluster + 2);
                if (ctx->fix_errors) {
                    /* Mark as free if invalid */
                    fat_sector[i] = htole32(EXFAT_CLUSTER_FREE);
                    ctx->modified = 1;
                }
            }
        }

        /* Write back if modified */
        if (ctx->modified) {
            off_t offset = (ctx->emp->boot.fat_offset + sector) * EXFAT_SECTOR_SIZE;
            if (lseek(ctx->fd, offset, SEEK_SET) != offset ||
                write(ctx->fd, fat_sector, EXFAT_SECTOR_SIZE) != EXFAT_SECTOR_SIZE) {
                report_error(ctx, FSCK_ERR_FATAL,
                    "Error writing FAT sector: %s", strerror(errno));
                error = -1;
                goto out;
            }
        }
    }

    /* If there are 2 FATs, verify they match */
    if (ctx->emp->boot.number_of_fats == 2) {
        uint32_t *fat2_sector = malloc(EXFAT_SECTOR_SIZE);
        if (fat2_sector == NULL) {
            report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate second FAT buffer");
            error = -1;
            goto out;
        }

        for (sector = 0; sector < ctx->emp->boot.fat_length; sector++) {
            /* Read from first FAT */
            error = read_fat_sector(ctx, sector, fat_sector);
            if (error) {
                free(fat2_sector);
                goto out;
            }

            /* Read from second FAT */
            error = read_fat_sector(ctx, sector + ctx->emp->boot.fat_length, fat2_sector);
            if (error) {
                free(fat2_sector);
                goto out;
            }

            /* Compare sectors */
            if (memcmp(fat_sector, fat2_sector, EXFAT_SECTOR_SIZE) != 0) {
                report_error(ctx, FSCK_ERR_SERIOUS,
                    "FAT1 and FAT2 mismatch in sector %u", sector);
                if (ctx->fix_errors) {
                    /* Copy FAT1 to FAT2 */
                    off_t offset = (ctx->emp->boot.fat_offset + 
                                  sector + ctx->emp->boot.fat_length) * EXFAT_SECTOR_SIZE;
                    if (lseek(ctx->fd, offset, SEEK_SET) != offset ||
                        write(ctx->fd, fat_sector, EXFAT_SECTOR_SIZE) != EXFAT_SECTOR_SIZE) {
                        report_error(ctx, FSCK_ERR_FATAL,
                            "Error writing FAT2 sector: %s", strerror(errno));
                        free(fat2_sector);
                        error = -1;
                        goto out;
                    }
                    ctx->modified = 1;
                }
            }
        }
        free(fat2_sector);
    }

out:
    free(fat_sector);
    return error;
}

static int
read_cluster(struct fsck_exfat_ctx *ctx, uint32_t cluster, void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    /* Calculate cluster offset */
    offset = ((off_t)ctx->emp->boot.cluster_heap_offset +
             ((off_t)cluster - 2) * (1 << ctx->emp->boot.sectors_per_cluster_shift)) *
             EXFAT_SECTOR_SIZE;

    /* Calculate cluster size */
    bytes_per_cluster = EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        report_error(ctx, FSCK_ERR_FATAL, "seek error: %s", strerror(errno));
        return -1;
    }

    bytes = read(ctx->fd, buffer, bytes_per_cluster);
    if (bytes != bytes_per_cluster) {
        report_error(ctx, FSCK_ERR_FATAL, "read error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int
validate_timestamp(struct fsck_exfat_ctx *ctx, const char *desc, 
                  uint32_t date, uint32_t time, uint8_t time_ms, uint8_t tz)
{
    int year, month, day, hour, min, sec;

    /* Extract fields */
    year = EXFAT_YEAR(date);
    month = EXFAT_MONTH(date);
    day = EXFAT_DAY(date);
    hour = EXFAT_HOUR(time);
    min = EXFAT_MINUTE(time);
    sec = EXFAT_SECOND(time);

    /* Basic range checks */
    if (year < 1980 || year > 2107 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59 ||
        time_ms > 199 || (tz != 0x80 && tz > 0x3F)) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid %s timestamp: %04d-%02d-%02d %02d:%02d:%02d.%03d tz=%02x",
            desc, year, month, day, hour, min, sec, time_ms * 10, tz);
        return -1;
    }

    /* Check days in month */
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_days = days_in_month[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        max_days = 29;
    if (day > max_days) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Invalid day (%d) in %s timestamp", day, desc);
        return -1;
    }

    return 0;
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
    uint16_t checksum;
    int i;

    /* Check file entry type */
    if (es->file.type != EXFAT_ENTRY_FILE) {
        report_error(ctx, FSCK_ERR_NORMAL, "Invalid file entry type: %02x", es->file.type);
        return -1;
    }

    /* Check secondary count */
    if (es->file.secondary_count < 2 || es->file.secondary_count > 18) {
        report_error(ctx, FSCK_ERR_NORMAL, 
            "Invalid secondary count: %u", es->file.secondary_count);
        return -1;
    }

    /* Validate timestamps */
    if (validate_timestamp(ctx, "creation",
                         es->file.create_timestamp,
                         es->file.create_timestamp,
                         es->file.create_time_ms,
                         es->file.create_tz) < 0 ||
        validate_timestamp(ctx, "modification",
                         es->file.last_modified_timestamp,
                         es->file.last_modified_timestamp,
                         es->file.last_modified_time_ms,
                         es->file.last_modified_tz) < 0 ||
        validate_timestamp(ctx, "access",
                         es->file.last_access_timestamp,
                         es->file.last_access_timestamp,
                         0, es->file.last_access_tz) < 0) {
        return -1;
    }

    /* Check stream entry type */
    if (es->stream.type != EXFAT_ENTRY_STREAM) {
        report_error(ctx, FSCK_ERR_NORMAL, "Invalid stream entry type: %02x", es->stream.type);
        return -1;
    }

    /* Check name length */
    if (es->stream.name_length == 0 || 
        es->stream.name_length > 255 ||
        es->stream.name_length > es->name_count * 15) {
        report_error(ctx, FSCK_ERR_NORMAL, "Invalid name length: %u", es->stream.name_length);
        return -1;
    }

    /* Check name entries */
    for (i = 0; i < es->name_count; i++) {
        if (es->name[i].type != EXFAT_ENTRY_NAME) {
            report_error(ctx, FSCK_ERR_NORMAL, 
                "Invalid name entry type: %02x", es->name[i].type);
            return -1;
        }
    }

    /* Validate name characters */
    if (validate_name_chars(ctx, es->name[0].name, es->stream.name_length) < 0)
        return -1;

    /* Verify name hash */
    uint16_t calc_hash = exfat_calc_name_hash(ctx->emp, es->name[0].name, 
                                             es->stream.name_length);
    if (calc_hash != es->stream.name_hash) {
        report_error(ctx, FSCK_ERR_NORMAL, 
            "Name hash mismatch (stored: %04x, calculated: %04x)",
            es->stream.name_hash, calc_hash);
        if (ctx->fix_errors) {
            es->stream.name_hash = calc_hash;
            ctx->modified = 1;
        }
        return -1;
    }

    /* Verify checksum */
    checksum = exfat_checksum_direntry(es);
    if (checksum != es->file.checksum) {
        report_error(ctx, FSCK_ERR_NORMAL, "Directory entry checksum mismatch");
        if (ctx->fix_errors) {
            es->file.checksum = checksum;
            ctx->modified = 1;
        }
        return -1;
    }

    return 0;
}

static int
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

                /* Read old cluster */
                if (read_cluster(ctx, cluster, buffer) < 0) {
                    free(buffer);
                    error = -1;
                    goto out;
                }

                /* Find free cluster */
                for (new_cluster = 2; new_cluster < ctx->emp->boot.cluster_count + 2; new_cluster++) {
                    if (cluster_map[new_cluster - 2].refs == 0)
                        break;
                }

                if (new_cluster >= ctx->emp->boot.cluster_count + 2) {
                    report_error(ctx, FSCK_ERR_FATAL, "No free clusters available");
                    free(buffer);
                    error = -1;
                    goto out;
                }

                /* Write to new cluster */
                if (write_cluster(ctx, new_cluster, buffer) < 0) {
                    free(buffer);
                    error = -1;
                    goto out;
                }

                free(buffer);

                /* Update FAT to point to new cluster */
                error = read_fat_sector(ctx, cluster / (EXFAT_SECTOR_SIZE / 4), fat_sector);
                if (error)
                    goto out;

                fat_sector[cluster % (EXFAT_SECTOR_SIZE / 4)] = htole32(new_cluster);
                cluster = new_cluster;
                ctx->modified = 1;
            }
            error = -1;
            goto out;
        }

        /* Mark cluster as used */
        cluster_map[cluster - 2].refs++;
        if (cluster_map[cluster - 2].refs == 1)
            cluster_map[cluster - 2].owner = start_cluster;

        /* Read FAT sector containing this cluster */
        error = read_fat_sector(ctx, cluster / (EXFAT_SECTOR_SIZE / 4), fat_sector);
        if (error)
            goto out;

        /* Get next cluster */
        next_cluster = le32toh(fat_sector[cluster % (EXFAT_SECTOR_SIZE / 4)]);

        /* Update size */
        total_size += cluster_size;

        /* Check for loops */
        if (total_size / cluster_size > ctx->emp->boot.cluster_count) {
            report_error(ctx, FSCK_ERR_SERIOUS, "Cluster chain loop detected");
            error = -1;
            goto out;
        }

        /* Move to next cluster */
        cluster = next_cluster;
    }

    /* Check if chain size matches file size */
    if (total_size < expected_size) {
        report_error(ctx, FSCK_ERR_NORMAL, 
            "Cluster chain too short (expected %llu bytes, got %llu)",
            (unsigned long long)expected_size, (unsigned long long)total_size);
        error = -1;
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
    char *buffer;
    struct exfat_direntry_set es;
    size_t bytes_per_cluster;
    uint32_t next_cluster;
    int error = 0;

    /* Calculate cluster size */
    bytes_per_cluster = EXFAT_SECTOR_SIZE << ctx->emp->boot.sectors_per_cluster_shift;

    /* Allocate cluster buffer */
    buffer = malloc(bytes_per_cluster);
    if (buffer == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate cluster buffer");
        return -1;
    }

    /* Process all clusters in the chain */
    while (cluster != EXFAT_CLUSTER_END) {
        uint8_t *entry = (uint8_t *)buffer;
        int entry_count = 0;

        /* Read cluster */
        error = read_cluster(ctx, cluster, buffer);
        if (error)
            goto out;

        /* Process all entries in cluster */
        while (entry < (uint8_t *)buffer + bytes_per_cluster) {
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
                memset(&es, 0, sizeof(es));
                memcpy(&es.file, entry, sizeof(es.file));
                entry += sizeof(es.file);

                /* Read stream entry */
                if (entry >= (uint8_t *)buffer + bytes_per_cluster) {
                    report_error(ctx, FSCK_ERR_NORMAL, "Directory entry spans clusters");
                    error = -1;
                    goto out;
                }
                memcpy(&es.stream, entry, sizeof(es.stream));
                entry += sizeof(es.stream);

                /* Read name entries */
                es.name_count = (es.stream.name_length + 14) / 15;
                for (int i = 0; i < es.name_count; i++) {
                    if (entry >= (uint8_t *)buffer + bytes_per_cluster) {
                        report_error(ctx, FSCK_ERR_NORMAL, "Directory entry spans clusters");
                        error = -1;
                        goto out;
                    }
                    memcpy(&es.name[i], entry, sizeof(es.name[i]));
                    entry += sizeof(es.name[i]);
                }

                /* Check directory entry set */
                if (check_direntry_set(ctx, &es) < 0) {
                    error = -1;
                    goto out;
                }

                /* Check file/directory contents */
                if (es.file.file_attributes & EXFAT_ATTR_DIRECTORY) {
                    if (check_directory(ctx, es.stream.first_cluster) < 0) {
                        error = -1;
                        goto out;
                    }
                } else {
                    if (check_file(ctx, &es) < 0) {
                        error = -1;
                        goto out;
                    }
                }

                entry_count++;
            } else {
                /* Skip unknown entry */
                entry += sizeof(struct exfat_entry_file);
            }
        }

        /* Get next cluster */
        error = read_fat_sector(ctx, cluster / (EXFAT_SECTOR_SIZE / 4),
                              (uint32_t *)buffer);
        if (error)
            goto out;
        next_cluster = le32toh(((uint32_t *)buffer)[cluster % (EXFAT_SECTOR_SIZE / 4)]);
        cluster = next_cluster;
    }

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

static int
read_bitmap_sector(struct fsck_exfat_ctx *ctx, off_t sector, uint8_t *buffer)
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

    bytes = read(ctx->fd, buffer, EXFAT_SECTOR_SIZE);
    if (bytes != EXFAT_SECTOR_SIZE) {
        report_error(ctx, FSCK_ERR_FATAL, "read error: %s", strerror(errno));
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
check_bitmap(struct fsck_exfat_ctx *ctx)
{
    uint8_t *bitmap_sector;
    uint32_t *fat_sector;
    uint32_t total_clusters = ctx->emp->boot.cluster_count;
    uint32_t bitmap_sectors = (total_clusters + 8 * EXFAT_SECTOR_SIZE - 1) / 
                             (8 * EXFAT_SECTOR_SIZE);
    uint32_t sector, i;
    int error = 0;

    /* Allocate buffers */
    bitmap_sector = malloc(EXFAT_SECTOR_SIZE);
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    if (bitmap_sector == NULL || fat_sector == NULL) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot allocate sector buffers");
        error = -1;
        goto out;
    }

    /* Check each bitmap sector */
    for (sector = 0; sector < bitmap_sectors; sector++) {
        error = read_bitmap_sector(ctx, sector, bitmap_sector);
        if (error)
            goto out;

        /* Check each bit in this sector */
        for (i = 0; i < EXFAT_SECTOR_SIZE * 8; i++) {
            uint32_t cluster = sector * EXFAT_SECTOR_SIZE * 8 + i;
            int bitmap_allocated = (bitmap_sector[i / 8] & (1 << (i % 8))) != 0;
            uint32_t fat_entry;

            /* Skip if beyond total clusters */
            if (cluster >= total_clusters)
                break;

            /* Read FAT entry */
            error = read_fat_sector(ctx, (cluster + 2) / (EXFAT_SECTOR_SIZE / 4),
                                  fat_sector);
            if (error)
                goto out;

            fat_entry = le32toh(fat_sector[(cluster + 2) % (EXFAT_SECTOR_SIZE / 4)]);
            int fat_allocated = (fat_entry != EXFAT_CLUSTER_FREE);

            /* Check for mismatches */
            if (bitmap_allocated != fat_allocated) {
                report_error(ctx, FSCK_ERR_NORMAL,
                    "Cluster %u allocation mismatch (bitmap: %d, FAT: %d)",
                    cluster + 2, bitmap_allocated, fat_allocated);
                if (ctx->fix_errors) {
                    /* Update bitmap to match FAT */
                    if (fat_allocated)
                        bitmap_sector[i / 8] |= (1 << (i % 8));
                    else
                        bitmap_sector[i / 8] &= ~(1 << (i % 8));
                    ctx->modified = 1;
                }
            }
        }

        /* Write back if modified */
        if (ctx->modified) {
            error = write_bitmap_sector(ctx, sector, bitmap_sector);
            if (error)
                goto out;
        }
    }

out:
    free(bitmap_sector);
    free(fat_sector);
    return error;
}

static int
recover_lost_clusters(struct fsck_exfat_ctx *ctx)
{
    uint32_t *fat_sector;
    uint8_t *bitmap_sector;
    struct cluster_info *cluster_map;
    uint32_t total_clusters = ctx->emp->boot.cluster_count;
    uint32_t lost_count = 0;
    int error = 0;

    /* Allocate buffers */
    fat_sector = malloc(EXFAT_SECTOR_SIZE);
    bitmap_sector = malloc(EXFAT_SECTOR_SIZE);
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

    /* Second pass: check for clusters that are allocated but not referenced */
    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        uint32_t fat_entry;
        int is_allocated;

        /* Read FAT entry */
        error = read_fat_sector(ctx, cluster / (EXFAT_SECTOR_SIZE / 4), fat_sector);
        if (error)
            goto out;
        fat_entry = le32toh(fat_sector[cluster % (EXFAT_SECTOR_SIZE / 4)]);

        /* Read bitmap entry */
        uint32_t bitmap_byte = (cluster - 2) / 8;
        uint32_t bitmap_bit = (cluster - 2) % 8;
        error = read_bitmap_sector(ctx, bitmap_byte / EXFAT_SECTOR_SIZE, bitmap_sector);
        if (error)
            goto out;
        is_allocated = (bitmap_sector[bitmap_byte % EXFAT_SECTOR_SIZE] & (1 << bitmap_bit)) != 0;

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

    if (lost_count > 0) {
        report_error(ctx, FSCK_ERR_NORMAL,
            "Found %u lost clusters", lost_count);
    }

out:
    free(cluster_map);
    free(bitmap_sector);
    free(fat_sector);
    return error;
}

static int
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

static int
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

static int
check_upcase_table(struct fsck_exfat_ctx *ctx)
{
    struct buf *bp;
    uint32_t sector;
    uint32_t checksum = 0;
    uint32_t size;
    uint32_t cluster;
    int error;

    /* Read first sector of root directory to find upcase table entry */
    sector = ctx->emp->boot.cluster_heap_offset +
             ((ctx->emp->boot.root_dir_cluster - 2) << ctx->emp->boot.sectors_per_cluster_shift);

    error = bread(ctx->fd, sector, EXFAT_SECTOR_SIZE, &bp);
    if (error) {
        report_error(ctx, FSCK_ERR_FATAL, "Cannot read root directory");
        return -1;
    }

    /* Find upcase table entry */
    struct exfat_entry_upcase *upcase = (struct exfat_entry_upcase *)bp->b_data;
    while (upcase->type != EXFAT_ENTRY_UPCASE && upcase->type != EXFAT_ENTRY_EOD) {
        upcase++;
        if ((uint8_t *)upcase >= (uint8_t *)bp->b_data + EXFAT_SECTOR_SIZE) {
            report_error(ctx, FSCK_ERR_SERIOUS, "No upcase table found");
            brelse(bp);
            return -1;
        }
    }

    if (upcase->type == EXFAT_ENTRY_EOD) {
        report_error(ctx, FSCK_ERR_SERIOUS, "No upcase table found");
        brelse(bp);
        return -1;
    }

    /* Get upcase table info */
    cluster = le32toh(upcase->first_cluster);
    size = le64toh(upcase->data_length);
    uint32_t stored_checksum = le32toh(upcase->checksum);

    brelse(bp);

    /* Verify cluster chain */
    error = check_cluster_chain(ctx, cluster, size);
    if (error)
        return error;

    /* Calculate checksum */
    uint32_t remaining = size;
    while (remaining > 0) {
        error = bread(ctx->fd, 
                     ctx->emp->boot.cluster_heap_offset + 
                     ((cluster - 2) << ctx->emp->boot.sectors_per_cluster_shift),
                     EXFAT_SECTOR_SIZE, &bp);
        if (error) {
            report_error(ctx, FSCK_ERR_FATAL, "Cannot read upcase table");
            return -1;
        }

        /* Update checksum */
        uint32_t bytes = MIN(remaining, EXFAT_SECTOR_SIZE);
        for (uint32_t i = 0; i < bytes; i++)
            checksum = ((checksum << 31) | (checksum >> 1)) + ((uint8_t *)bp->b_data)[i];

        brelse(bp);
        remaining -= bytes;
        
        if (remaining > 0) {
            error = get_next_cluster(ctx, cluster, &cluster);
            if (error)
                return error;
        }
    }

    /* Verify checksum */
    if (checksum != stored_checksum) {
        report_error(ctx, FSCK_ERR_SERIOUS,
            "Upcase table checksum mismatch (stored: %08x, calculated: %08x)",
            stored_checksum, checksum);
        if (ctx->fix_errors) {
            /* TODO: Replace with default upcase table */
            report_error(ctx, FSCK_ERR_NORMAL,
                "Upcase table repair not implemented yet");
        }
        return -1;
    }

    return 0;
}

int
main(int argc, char *argv[])
{
    struct fsck_exfat_ctx ctx;
    int ch;

    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));

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

    /* Allocate mount structure */
    ctx.emp = calloc(1, sizeof(struct exfat_mount));
    if (ctx.emp == NULL)
        err(1, "Cannot allocate mount structure");

    /* Check filesystem structures */
    if (check_boot_sector(&ctx) < 0)
        goto error;

    /* Check FAT */
    if (check_fat(&ctx) < 0)
        goto error;

    /* Check root directory */
    if (check_root_dir(&ctx) < 0)
        goto error;

    /* Check allocation bitmap */
    if (check_bitmap(&ctx) < 0)
        goto error;

    /* Check upcase table */
    if (check_upcase_table(&ctx) < 0)
        goto error;

    /* Check for lost clusters */
    if (recover_lost_clusters(&ctx) < 0)
        goto error;

    /* Clean up */
    free(ctx.emp);
    close(ctx.fd);

    return ctx.modified ? 1 : 0;

error:
    free(ctx.emp);
    close(ctx.fd);
    return 1;
} 