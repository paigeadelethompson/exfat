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
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include "exfat.h"
#include "exfat_upcase.h"

/* In-memory upcase table */
struct exfat_upcase {
    uint16_t *table;
    size_t size;
};

/* Add function prototypes */
static int exfat_read_upcase_table(struct exfat_mount *emp, struct exfat_upcase *upcase);
int exfat_load_upcase_table(struct exfat_mount *emp);
void exfat_unload_upcase_table(struct exfat_mount *emp);

/*
 * Read upcase table from disk
 */
static int
exfat_read_upcase_table(struct exfat_mount *emp, struct exfat_upcase *upcase)
{
    if (bootverbose)
        printf("exfat: [exfat_read_upcase_data] reading upcase table data, size %lu bytes\n",
               (unsigned long)(upcase->size * sizeof(uint16_t)));

    struct buf *bp;
    uint32_t cluster, sector, remaining;
    uint16_t *table;
    int error;

    /* Allocate memory for table */
    table = malloc(upcase->size * sizeof(uint16_t), M_EXFAT, M_WAITOK);
    upcase->table = table;

    /* Read table data */
    cluster = emp->upcase_cluster;
    remaining = upcase->size * sizeof(uint16_t);
    sector = emp->boot.cluster_heap_offset +
             ((cluster - 2) << emp->boot.sectors_per_cluster_shift);

    while (remaining > 0) {
        if (bootverbose)
            printf("exfat: reading sector %u, %u bytes remaining\n", 
                   sector, remaining);

        error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
        if (error) {
            if (bootverbose)
                printf("exfat: failed to read sector: %d\n", error);
            brelse(bp);
            free(table, M_EXFAT);
            return error;
        }

        /* Copy data from buffer */
        size_t size = MIN(remaining, EXFAT_SECTOR_SIZE);
        memcpy(table, bp->b_data, size);
        brelse(bp);
        sector++;
        table += size / sizeof(uint16_t);
        remaining -= size;
    }

    if (bootverbose)
        printf("exfat: [exfat_read_upcase_data] upcase table data read successfully\n");

    return 0;
}

/*
 * Initialize upcase table
 */
int
exfat_load_upcase_table(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: [exfat_init_upcase] loading upcase table\n");

    struct buf *bp;
    struct exfat_entry_upcase *upcase;
    uint32_t sector;
    int error;

    /* Read first sector of root directory */
    sector = emp->boot.cluster_heap_offset +
             ((emp->root_cluster - 2) << emp->boot.sectors_per_cluster_shift);

    error = bread(emp->devvp, sector, EXFAT_SECTOR_SIZE, NOCRED, &bp);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read root directory sector: %d\n", error);
        brelse(bp);
        return error;
    }

    /* Look for upcase table entry */
    upcase = (struct exfat_entry_upcase *)bp->b_data;
    while (upcase->type != EXFAT_ENTRY_UPCASE && upcase->type != EXFAT_ENTRY_EOD) {
        upcase++;
        if ((uint8_t *)upcase >= (uint8_t *)bp->b_data + EXFAT_SECTOR_SIZE) {
            if (bootverbose)
                printf("exfat: upcase table entry not found\n");
            brelse(bp);
            return ENOENT;
        }
    }

    if (upcase->type == EXFAT_ENTRY_EOD) {
        if (bootverbose)
            printf("exfat: reached end of directory without finding upcase table\n");
        brelse(bp);
        return ENOENT;
    }

    if (bootverbose)
        printf("exfat: [exfat_init_upcase] found upcase table at cluster %u, size %lu bytes\n",
               upcase->first_cluster, (unsigned long)upcase->data_length);

    /* Allocate memory for upcase table */
    struct exfat_upcase *up = malloc(sizeof(*up), M_EXFAT, M_WAITOK);
    up->size = upcase->data_length / sizeof(uint16_t);
    emp->upcase = up;

    /* Read upcase table data */
    error = exfat_read_upcase_table(emp, up);
    if (error) {
        if (bootverbose)
            printf("exfat: failed to read upcase table data: %d\n", error);
        free(up, M_EXFAT);
        emp->upcase = NULL;
        brelse(bp);
        return error;
    }

    if (bootverbose)
        printf("exfat: [exfat_init_upcase] upcase table loaded successfully\n");

    brelse(bp);
    return 0;
}

/*
 * Clean up upcase table
 */
void
exfat_unload_upcase_table(struct exfat_mount *emp)
{
    if (bootverbose)
        printf("exfat: [exfat_cleanup_upcase] unloading upcase table\n");

    struct exfat_upcase *up = emp->upcase;
    if (up) {
        if (up->table)
            free(up->table, M_EXFAT);
        free(up, M_EXFAT);
        emp->upcase = NULL;
    }
}

/*
 * Convert character to uppercase
 */
uint16_t
exfat_upcase(struct exfat_mount *emp, uint16_t c)
{
    struct exfat_upcase *up = emp->upcase;
    
    /* If no upcase table loaded, use simple ASCII conversion */
    if (!up || !up->table) {
        return (c >= 'a' && c <= 'z') ? c - 0x20 : c;
    }

    /* Check if character is in range */
    if (c >= up->size) {
        if (bootverbose)
            printf("exfat: character 0x%04x out of upcase table range\n", c);
        return c;
    }

    return up->table[c];
}

/*
 * Compare names case-insensitively
 */
int
exfat_name_compare(struct exfat_mount *emp, const uint16_t *name1,
                  const uint16_t *name2, size_t len)
{
    if (bootverbose)
        printf("exfat: comparing names case-insensitively (len %zu)\n", len);

    size_t i;

    for (i = 0; i < len; i++) {
        uint16_t c1 = exfat_upcase(emp, le16toh(name1[i]));
        uint16_t c2 = exfat_upcase(emp, le16toh(name2[i]));
        
        if (c1 < c2) {
            if (bootverbose)
                printf("exfat: name1 < name2 at position %zu (%04x < %04x)\n",
                       i, c1, c2);
            return -1;
        }
        if (c1 > c2) {
            if (bootverbose)
                printf("exfat: name1 > name2 at position %zu (%04x > %04x)\n",
                       i, c1, c2);
            return 1;
        }
    }

    if (bootverbose)
        printf("exfat: names are equal\n");
    return 0;
} 