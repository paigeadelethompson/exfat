#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "newfs_exfat.h"

static uint32_t calculate_checksum(const void *boot_region, size_t size) {
    const uint8_t *p = boot_region;
    uint32_t checksum = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        checksum = ((checksum << 31) | (checksum >> 1)) + p[i];
        if ((i + 1) % 512 == 0)
            printf("After sector %zu: %08x\n", i / 512, checksum);
    }
    return checksum;
}

int main() {
    uint8_t boot_region[EXFAT_BOOT_REGION_SIZE * EXFAT_SECTOR_SIZE] = {0};
    struct exfat_boot_record *boot = (struct exfat_boot_record *)boot_region;

    /* Initialize boot sector same as newfs_exfat */
    boot->jump_boot[0] = 0xEB;
    boot->jump_boot[1] = 0x76;
    boot->jump_boot[2] = 0x90;
    memcpy(boot->fs_name, "EXFAT   ", 8);
    memset(boot->must_be_zero, 0, sizeof(boot->must_be_zero));
    
    /* Set some test values */
    boot->volume_length = 1949696;
    boot->fat_offset = 24;
    boot->fat_length = 238;
    boot->cluster_heap_offset = 262;
    boot->cluster_count = 30459;
    boot->root_dir_cluster = 4;
    boot->volume_serial = 0x76FB0000;
    boot->fs_revision = 0x0100;
    boot->volume_flags = 0x0000;
    boot->bytes_per_sector_shift = 9;
    boot->sectors_per_cluster_shift = 6;
    boot->number_of_fats = 1;
    boot->drive_select = 0x80;
    boot->percent_in_use = 0;
    
    /* Add signature to each sector */
    for (int i = 0; i < EXFAT_BOOT_REGION_SIZE; i++) {
        boot_region[(i * EXFAT_SECTOR_SIZE) + 510] = 0x55;
        boot_region[(i * EXFAT_SECTOR_SIZE) + 511] = 0xAA;
    }

    /* Calculate checksum before writing it */
    uint32_t checksum = calculate_checksum(boot_region, 11 * EXFAT_SECTOR_SIZE);
    printf("Initial checksum: %08x\n", checksum);

    /* Write checksum to boot sector */
    *(uint32_t *)(boot_region + 0x50) = checksum;

    /* Fill validation pattern sectors */
    uint32_t pattern = EXFAT_BOOT_VALIDATION_PATTERN;
    uint8_t *validation1 = boot_region + (EXFAT_VALIDATION_SECTOR1 * EXFAT_SECTOR_SIZE);
    uint8_t *validation2 = boot_region + (EXFAT_VALIDATION_SECTOR2 * EXFAT_SECTOR_SIZE);
    
    for (int i = 0; i < EXFAT_SECTOR_SIZE; i += sizeof(pattern)) {
        *(uint32_t *)(validation1 + i) = pattern;
        *(uint32_t *)(validation2 + i) = pattern;
    }

    /* Calculate final checksum */
    checksum = calculate_checksum(boot_region, 11 * EXFAT_SECTOR_SIZE);
    printf("Final checksum: %08x\n", checksum);

    return 0;
} 