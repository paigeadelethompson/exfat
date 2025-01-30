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
 
#ifndef _FS_EXFAT_UTIL_H_
#define _FS_EXFAT_UTIL_H_

#include <sys/param.h>
#include <sys/time.h>

/* Timestamp update flags */
#define EXFAT_UTIME_ACCESS  0x01
#define EXFAT_UTIME_MODIFY  0x02
#define EXFAT_UTIME_CREATE  0x04

void exfat_timestamp_to_timespec(uint32_t timestamp, uint8_t time_ms,
                               uint8_t tz_offset, struct timespec *ts);
void exfat_timespec_to_timestamp(const struct timespec *ts,
                               uint32_t *timestamp, uint8_t *time_ms,
                               uint8_t *tz_offset);
void exfat_update_timestamps(struct exfat_entry_file *file, int update_mask);

#endif /* _FS_EXFAT_UTIL_H_ */ 