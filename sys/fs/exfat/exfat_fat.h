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
 
/* FAT entry operations */
int exfat_write_fat_entry(struct exfat_mount *emp, uint32_t cluster, uint32_t value);
int exfat_read_fat_entry(struct exfat_mount *emp, uint32_t cluster, uint32_t *value);
int exfat_update_bitmap(struct exfat_mount *emp, uint32_t cluster, int allocated); 