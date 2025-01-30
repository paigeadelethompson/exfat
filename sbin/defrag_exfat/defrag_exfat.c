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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#ifdef __APPLE__
#include <sys/disk.h>
#endif

#include "defrag_exfat.h"

/* FAT table buffer */
static uint32_t *fat_table = NULL;

static void
usage(void)
{
    fprintf(stderr,
        "usage: defrag_exfat [-fnv] [-v|-vv|-vvv] filesystem\n"
        "  -f         Force defragmentation\n"
        "  -n         Dry run (analyze only)\n"
        "  -v         Be verbose (can be repeated for more detail)\n"
        "\nVerbosity levels:\n"
        "  -v         Basic progress information\n"
        "  -vv        Detailed progress information\n"
        "  -vvv       Debug data dumps\n");
    exit(1);
}

void
print_progress(struct defrag_exfat_ctx *ctx, const char *phase, 
              uint32_t current, uint32_t total)
{
    if (!ctx->verbose)
        return;
        
    printf("\r%s: %u/%u (%2.1f%%)...", 
           phase, current, total,
           total ? ((float)current * 100.0f) / total : 0.0f);
    fflush(stdout);
}

static int
read_cluster(struct defrag_exfat_ctx *ctx, uint32_t cluster, void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    /* Calculate cluster size and offset */
    bytes_per_cluster = ctx->bytes_per_sector * ctx->sectors_per_cluster;
    offset = (off_t)ctx->cluster_heap_offset * ctx->bytes_per_sector +
             ((off_t)cluster - 2) * bytes_per_cluster;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("cluster seek error: %s", strerror(errno));
        return -1;
    }

    bytes = read(ctx->fd, buffer, bytes_per_cluster);
    if (bytes != bytes_per_cluster) {
        warn("cluster read error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int
write_cluster(struct defrag_exfat_ctx *ctx, uint32_t cluster, const void *buffer)
{
    off_t offset;
    size_t bytes_per_cluster;
    ssize_t bytes;

    if (ctx->dry_run)
        return 0;

    /* Calculate cluster size and offset */
    bytes_per_cluster = ctx->bytes_per_sector * ctx->sectors_per_cluster;
    offset = (off_t)ctx->cluster_heap_offset * ctx->bytes_per_sector +
             ((off_t)cluster - 2) * bytes_per_cluster;

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("cluster seek error: %s", strerror(errno));
        return -1;
    }

    bytes = write(ctx->fd, buffer, bytes_per_cluster);
    if (bytes != bytes_per_cluster) {
        warn("cluster write error: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int
read_boot_sector(struct defrag_exfat_ctx *ctx)
{
    struct exfat_boot_sector boot;
    ssize_t bytes;

    /* Read boot sector */
    if (lseek(ctx->fd, 0, SEEK_SET) < 0) {
        warn("seek error: %s", strerror(errno));
        return -1;
    }

    bytes = read(ctx->fd, &boot, sizeof(boot));
    if (bytes != sizeof(boot)) {
        warn("read error: %s", strerror(errno));
        return -1;
    }

    /* Validate signature */
    if (memcmp(boot.fs_name, "EXFAT   ", 8) != 0) {
        warnx("Not an ExFAT filesystem");
        return -1;
    }

    /* Store filesystem parameters */
    ctx->bytes_per_sector = 1 << boot.bytes_per_sector_shift;
    ctx->sectors_per_cluster = 1 << boot.sectors_per_cluster_shift;
    ctx->cluster_count = le32toh(boot.cluster_count);
    ctx->fat_offset = le32toh(boot.fat_offset);
    ctx->fat_length = le32toh(boot.fat_length);
    ctx->cluster_heap_offset = le32toh(boot.cluster_heap_offset);

    if (ctx->verbose >= DEBUG_DETAIL) {
        printf("Filesystem parameters:\n");
        printf("  Bytes per sector: %u\n", ctx->bytes_per_sector);
        printf("  Sectors per cluster: %u\n", ctx->sectors_per_cluster);
        printf("  Total clusters: %u\n", ctx->cluster_count);
        printf("  FAT offset: %u sectors\n", ctx->fat_offset);
        printf("  FAT length: %u sectors\n", ctx->fat_length);
        printf("  Cluster heap offset: %u sectors\n", ctx->cluster_heap_offset);
    }

    return 0;
}

static int
read_fat(struct defrag_exfat_ctx *ctx)
{
    size_t fat_size;
    off_t offset;
    ssize_t bytes;

    /* Calculate FAT size and allocate buffer */
    fat_size = ctx->fat_length * ctx->bytes_per_sector;
    fat_table = malloc(fat_size);
    if (fat_table == NULL) {
        warn("Cannot allocate FAT buffer");
        return -1;
    }

    /* Read entire FAT */
    offset = (off_t)ctx->fat_offset * ctx->bytes_per_sector;
    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("FAT seek error: %s", strerror(errno));
        return -1;
    }

    bytes = read(ctx->fd, fat_table, fat_size);
    if (bytes != fat_size) {
        warn("FAT read error: %s", strerror(errno));
        return -1;
    }

    /* Convert entries to host byte order */
    for (size_t i = 0; i < fat_size / sizeof(uint32_t); i++) {
        fat_table[i] = le32toh(fat_table[i]);
    }

    return 0;
}

static int
get_cluster_chain(struct defrag_exfat_ctx *ctx, uint32_t start_cluster,
                 struct file_fragments **fragments)
{
    uint32_t cluster = start_cluster;
    uint32_t prev_cluster = 0;
    struct file_fragments *frag = NULL;
    int fragment_count = 0;

    *fragments = NULL;

    while (cluster != EXFAT_CLUSTER_END && cluster < ctx->cluster_count + 2) {
        /* Start new fragment if not contiguous */
        if (prev_cluster && cluster != prev_cluster + 1) {
            struct file_fragments *new_frag = malloc(sizeof(*new_frag));
            if (!new_frag) {
                warn("Cannot allocate fragment");
                return -1;
            }
            new_frag->start_cluster = cluster;
            new_frag->length = 1;
            new_frag->next = NULL;

            if (frag)
                frag->next = new_frag;
            else
                *fragments = new_frag;
            frag = new_frag;
            fragment_count++;
        }
        /* Extend current fragment */
        else if (frag) {
            frag->length++;
        }
        /* Start first fragment */
        else {
            frag = malloc(sizeof(*frag));
            if (!frag) {
                warn("Cannot allocate fragment");
                return -1;
            }
            frag->start_cluster = cluster;
            frag->length = 1;
            frag->next = NULL;
            *fragments = frag;
            fragment_count++;
        }

        prev_cluster = cluster;
        cluster = fat_table[cluster];
    }

    return fragment_count;
}

static int
scan_directory(struct defrag_exfat_ctx *ctx, uint32_t cluster, const char *path)
{
    uint8_t *buffer;
    size_t bytes_per_cluster;
    struct file_info *file_list = NULL;
    int error = 0;

    /* Calculate cluster size */
    bytes_per_cluster = ctx->bytes_per_sector * ctx->sectors_per_cluster;

    /* Allocate cluster buffer */
    buffer = malloc(bytes_per_cluster);
    if (buffer == NULL) {
        warn("Cannot allocate cluster buffer");
        return -1;
    }

    /* Process all clusters in directory chain */
    while (cluster != EXFAT_CLUSTER_END && cluster < ctx->cluster_count + 2) {
        struct exfat_entry_file *file;
        struct exfat_entry_stream *stream;
        uint8_t *entry;
        uint8_t *end;

        /* Read directory cluster */
        error = read_cluster(ctx, cluster, buffer);
        if (error)
            goto out;

        /* Process all entries in cluster */
        entry = buffer;
        end = buffer + bytes_per_cluster;

        while (entry + sizeof(struct exfat_entry_file) <= end) {
            file = (struct exfat_entry_file *)entry;

            /* Check for end of directory */
            if (file->type == EXFAT_ENTRY_EOD)
                goto done;

            /* Skip deleted entries */
            if (file->type == EXFAT_ENTRY_DELETED) {
                entry += sizeof(struct exfat_entry_file);
                continue;
            }

            /* Process file entry sets */
            if (file->type == EXFAT_ENTRY_FILE) {
                char *file_path;
                struct file_info *info;
                struct file_fragments *frags = NULL;
                int fragment_count;

                /* Validate secondary count */
                if (file->secondary_count < 2) {
                    warn("Invalid secondary count: %u", file->secondary_count);
                    error = -1;
                    goto out;
                }

                /* Get stream entry */
                stream = (struct exfat_entry_stream *)(entry + sizeof(*file));
                if (stream->type != EXFAT_ENTRY_STREAM) {
                    warn("Missing stream entry");
                    error = -1;
                    goto out;
                }

                /* Skip zero-length files */
                if (stream->data_length == 0) {
                    entry += (1 + file->secondary_count) * sizeof(*file);
                    continue;
                }

                /* Build full path */
                if (asprintf(&file_path, "%s/%.*s", path,
                           stream->name_length, entry + 2 * sizeof(*file)) < 0) {
                    warn("Cannot allocate path");
                    error = -1;
                    goto out;
                }

                /* Get cluster chain info */
                fragment_count = get_cluster_chain(ctx, stream->first_cluster, &frags);
                if (fragment_count < 0) {
                    free(file_path);
                    error = -1;
                    goto out;
                }

                /* Create file info */
                info = calloc(1, sizeof(*info));
                if (info == NULL) {
                    free(file_path);
                    error = -1;
                    goto out;
                }

                info->path = file_path;
                info->first_cluster = stream->first_cluster;
                info->size = stream->data_length;
                info->fragment_count = fragment_count;
                info->fragments = frags;

                /* Add to context's file list */
                info->next = ctx->file_list;
                ctx->file_list = info;

                /* Update statistics */
                ctx->files_total++;
                if (fragment_count > 1) {
                    ctx->files_fragmented++;
                    if (ctx->verbose >= DEBUG_DETAIL) {
                        printf("Fragmented file: %s\n", file_path);
                        printf("  Size: %llu bytes\n", (unsigned long long)info->size);
                        printf("  Fragments: %u\n", fragment_count);
                    }
                }

                /* Recursively scan subdirectories */
                if (file->file_attributes & EXFAT_ATTR_DIRECTORY) {
                    error = scan_directory(ctx, stream->first_cluster, file_path);
                    if (error)
                        goto out;
                }

                /* Skip remaining entries in set */
                entry += (1 + file->secondary_count) * sizeof(*file);
            } else {
                /* Skip unknown entry */
                entry += sizeof(*file);
            }
        }

        /* Get next cluster */
        cluster = fat_table[cluster];
    }

done:
    error = 0;

out:
    /* Clean up file list */
    while (file_list) {
        struct file_info *next = file_list->next;
        struct file_fragments *frag = file_list->fragments;
        
        while (frag) {
            struct file_fragments *next_frag = frag->next;
            free(frag);
            frag = next_frag;
        }
        
        free(file_list->path);
        free(file_list);
        file_list = next;
    }

    free(buffer);
    return error;
}

int
analyze_fragmentation(struct defrag_exfat_ctx *ctx)
{
    int error;

    /* Read and validate boot sector */
    error = read_boot_sector(ctx);
    if (error)
        return error;

    /* Read FAT */
    error = read_fat(ctx);
    if (error)
        return error;

    /* Scan filesystem starting at root directory */
    error = scan_directory(ctx, ctx->root_dir_cluster, "");
    if (error)
        goto out;

    if (ctx->verbose >= DEBUG_BASIC) {
        printf("\nFragmentation analysis complete:\n");
        printf("  Total files: %u\n", ctx->files_total);
        printf("  Fragmented files: %u (%.1f%%)\n", 
               ctx->files_fragmented,
               ctx->files_total ? 
               ((float)ctx->files_fragmented * 100.0f) / ctx->files_total : 0.0f);
    }

out:
    /* Clean up */
    free(fat_table);
    fat_table = NULL;

    return error;
}

static int
find_free_space(struct defrag_exfat_ctx *ctx, uint32_t clusters_needed, uint32_t *start_cluster)
{
    uint32_t current_start = 2;
    uint32_t current_length = 0;

    /* Scan FAT for contiguous free clusters */
    for (uint32_t i = 2; i < ctx->cluster_count + 2; i++) {
        if (fat_table[i] == EXFAT_CLUSTER_FREE) {
            if (current_length == 0)
                current_start = i;
            current_length++;
            
            if (current_length >= clusters_needed) {
                *start_cluster = current_start;
                return 0;
            }
        } else {
            current_length = 0;
        }
    }

    return -1; /* No suitable free space found */
}

static int
update_fat_chain(struct defrag_exfat_ctx *ctx, uint32_t start_cluster, uint32_t length)
{
    off_t offset;
    uint32_t i;

    if (ctx->dry_run)
        return 0;

    /* Link clusters in chain */
    for (i = 0; i < length - 1; i++) {
        fat_table[start_cluster + i] = start_cluster + i + 1;
    }
    fat_table[start_cluster + length - 1] = EXFAT_CLUSTER_END;

    /* Write updated FAT entries */
    offset = (off_t)ctx->fat_offset * ctx->bytes_per_sector +
             start_cluster * sizeof(uint32_t);

    if (lseek(ctx->fd, offset, SEEK_SET) != offset) {
        warn("FAT seek error");
        return -1;
    }

    /* Write chain to disk */
    if (write(ctx->fd, &fat_table[start_cluster], length * sizeof(uint32_t)) !=
        length * sizeof(uint32_t)) {
        warn("FAT write error");
        return -1;
    }

    return 0;
}

static int
update_directory_entry(struct defrag_exfat_ctx *ctx, const char *path, uint32_t new_start_cluster)
{
    uint8_t *buffer;
    size_t bytes_per_cluster;
    uint32_t cluster;
    int found = 0;
    int error = 0;

    if (ctx->dry_run)
        return 0;

    /* Calculate cluster size */
    bytes_per_cluster = ctx->bytes_per_sector * ctx->sectors_per_cluster;

    /* Allocate cluster buffer */
    buffer = malloc(bytes_per_cluster);
    if (buffer == NULL) {
        warn("Cannot allocate cluster buffer");
        return -1;
    }

    /* Start at root directory */
    cluster = ctx->root_dir_cluster;

    /* Search for file entry */
    while (cluster != EXFAT_CLUSTER_END && !found) {
        struct exfat_entry_file *file;
        struct exfat_entry_stream *stream;
        uint8_t *entry;
        uint8_t *end;

        error = read_cluster(ctx, cluster, buffer);
        if (error)
            goto out;

        entry = buffer;
        end = buffer + bytes_per_cluster;

        while (entry + sizeof(struct exfat_entry_file) <= end && !found) {
            file = (struct exfat_entry_file *)entry;

            if (file->type == EXFAT_ENTRY_FILE) {
                stream = (struct exfat_entry_stream *)(entry + sizeof(*file));
                
                /* Compare path */
                char *entry_path;
                if (asprintf(&entry_path, "%.*s", 
                           stream->name_length, entry + 2 * sizeof(*file)) >= 0) {
                    if (strcmp(path, entry_path) == 0) {
                        /* Update first cluster */
                        stream->first_cluster = htole32(new_start_cluster);
                        
                        /* Write updated entry */
                        error = write_cluster(ctx, cluster, buffer);
                        found = 1;
                    }
                    free(entry_path);
                }
            }
            entry += sizeof(struct exfat_entry_file);
        }

        if (!found)
            cluster = fat_table[cluster];
    }

    if (!found) {
        warnx("Could not find directory entry for %s", path);
        error = -1;
    }

out:
    free(buffer);
    return error;
}

/* Calculate fragmentation score for sorting */
static float
calculate_frag_score(const struct file_info *file)
{
    float size_factor;
    float frag_factor;
    
    /* Normalize file size (log scale) */
    size_factor = log2f(file->size + 1) / 30.0f;  /* Assuming max file size ~1GB */
    
    /* Normalize fragment count */
    frag_factor = (float)(file->fragment_count - 1) / 100.0f;  /* Cap at 100 fragments */
    
    /* Combined score - higher means more fragmented/larger */
    return size_factor + frag_factor;
}

/* Compare function for sorting */
static int
compare_files(const void *a, const void *b)
{
    const struct file_info *fa = *(const struct file_info **)a;
    const struct file_info *fb = *(const struct file_info **)b;
    float score_a = calculate_frag_score(fa);
    float score_b = calculate_frag_score(fb);
    
    /* Sort by score descending */
    if (score_a > score_b) return -1;
    if (score_a < score_b) return 1;
    return 0;
}

/* Sort files for optimal defragmentation order */
static struct file_info **
sort_files(struct defrag_exfat_ctx *ctx)
{
    struct file_info **files;
    struct file_info *file;
    size_t count = 0;
    size_t i;

    /* Count fragmented files */
    for (file = ctx->file_list; file != NULL; file = file->next) {
        if (file->fragment_count > 1)
            count++;
    }

    if (count == 0)
        return NULL;

    /* Allocate array */
    files = calloc(count, sizeof(*files));
    if (files == NULL) {
        warn("Cannot allocate file array");
        return NULL;
    }

    /* Fill array */
    i = 0;
    for (file = ctx->file_list; file != NULL; file = file->next) {
        if (file->fragment_count > 1)
            files[i++] = file;
    }

    /* Sort array */
    qsort(files, count, sizeof(*files), compare_files);

    if (ctx->verbose >= DEBUG_DETAIL) {
        printf("\nSorted files for defragmentation:\n");
        for (i = 0; i < count; i++) {
            printf("  %s\n", files[i]->path);
            printf("    Size: %llu bytes\n", (unsigned long long)files[i]->size);
            printf("    Fragments: %u\n", files[i]->fragment_count);
            printf("    Score: %.3f\n", calculate_frag_score(files[i]));
        }
    }

    return files;
}

int
defrag_file(struct defrag_exfat_ctx *ctx, const char *path, struct file_fragments *frags)
{
    uint32_t total_clusters = 0;
    uint32_t new_start_cluster;
    struct file_fragments *frag;
    uint8_t *buffer;
    size_t bytes_per_cluster;
    int error = 0;

    /* Count total clusters needed */
    for (frag = frags; frag != NULL; frag = frag->next) {
        total_clusters += frag->length;
    }

    /* Find contiguous free space */
    error = find_free_space(ctx, total_clusters, &new_start_cluster);
    if (error) {
        warnx("Could not find %u contiguous free clusters", total_clusters);
        return error;
    }

    if (ctx->verbose >= DEBUG_DETAIL) {
        printf("Moving file %s\n", path);
        printf("  New location: clusters %u-%u\n", 
               new_start_cluster, new_start_cluster + total_clusters - 1);
    }

    /* Allocate cluster buffer */
    bytes_per_cluster = ctx->bytes_per_sector * ctx->sectors_per_cluster;
    buffer = malloc(bytes_per_cluster);
    if (buffer == NULL) {
        warn("Cannot allocate cluster buffer");
        return -1;
    }

    /* Copy clusters to new location */
    uint32_t dest_cluster = new_start_cluster;
    for (frag = frags; frag != NULL; frag = frag->next) {
        for (uint32_t i = 0; i < frag->length; i++) {
            /* Read source cluster */
            error = read_cluster(ctx, frag->start_cluster + i, buffer);
            if (error)
                goto out;

            /* Write to destination */
            error = write_cluster(ctx, dest_cluster + i, buffer);
            if (error)
                goto out;

            ctx->bytes_moved += bytes_per_cluster;
        }
        dest_cluster += frag->length;
    }

    /* Update FAT chain */
    error = update_fat_chain(ctx, new_start_cluster, total_clusters);
    if (error)
        goto out;

    /* Update directory entry */
    error = update_directory_entry(ctx, path, new_start_cluster);
    if (error)
        goto out;

    /* Mark old clusters as free */
    if (!ctx->dry_run) {
        for (frag = frags; frag != NULL; frag = frag->next) {
            for (uint32_t i = 0; i < frag->length; i++) {
                fat_table[frag->start_cluster + i] = EXFAT_CLUSTER_FREE;
            }
        }
    }

    ctx->files_defragged++;
    ctx->fragments_eliminated += total_clusters - 1;

out:
    free(buffer);
    return error;
}

int
defrag_exfat(struct defrag_exfat_ctx *ctx)
{
    struct file_info **sorted_files;
    int error;
    size_t i;

    /* First analyze current fragmentation state */
    error = analyze_fragmentation(ctx);
    if (error)
        return error;

    /* Print analysis results */
    printf("\nFragmentation analysis:\n");
    printf("  Total files: %u\n", ctx->files_total);
    printf("  Fragmented files: %u (%.1f%%)\n", 
           ctx->files_fragmented,
           ctx->files_total ? 
           ((float)ctx->files_fragmented * 100.0f) / ctx->files_total : 0.0f);

    if (ctx->dry_run) {
        printf("\nDry run - no changes made\n");
        return 0;
    }

    /* Sort files by size/fragmentation */
    sorted_files = sort_files(ctx);
    if (sorted_files == NULL && ctx->files_fragmented > 0) {
        warnx("Failed to sort files");
        return -1;
    }

    /* Process files in sorted order */
    for (i = 0; i < ctx->files_fragmented; i++) {
        error = defrag_file(ctx, sorted_files[i]->path, sorted_files[i]->fragments);
        if (error)
            break;

        /* Show progress */
        if (ctx->progress_cb) {
            ctx->progress_cb(ctx, "Defragmenting files", i + 1, ctx->files_fragmented);
        }
    }

    /* Print final statistics */
    printf("\nDefragmentation complete:\n");
    printf("  Files defragmented: %u\n", ctx->files_defragged);
    printf("  Fragments eliminated: %u\n", ctx->fragments_eliminated);
    printf("  Total bytes moved: %llu\n", (unsigned long long)ctx->bytes_moved);

    /* Clean up file list at end */
    while (ctx->file_list) {
        struct file_info *next = ctx->file_list->next;
        struct file_fragments *frag = ctx->file_list->fragments;
        
        while (frag) {
            struct file_fragments *next_frag = frag->next;
            free(frag);
            frag = next_frag;
        }
        
        free(ctx->file_list->path);
        free(ctx->file_list);
        ctx->file_list = next;
    }

    free(sorted_files);
    return error;
}

int
main(int argc, char *argv[])
{
    struct defrag_exfat_ctx ctx = {0};
    int ch;

    /* Parse command line options */
    while ((ch = getopt(argc, argv, "fnv")) != -1) {
        switch (ch) {
        case 'f':
            ctx.force = 1;
            break;
        case 'n':
            ctx.dry_run = 1;
            break;
        case 'v':
            ctx.verbose++;
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

    /* Set up progress callback */
    ctx.progress_cb = print_progress;

    /* Run defragmentation */
    if (defrag_exfat(&ctx) < 0) {
        close(ctx.fd);
        return 1;
    }

    close(ctx.fd);
    return 0;
} 