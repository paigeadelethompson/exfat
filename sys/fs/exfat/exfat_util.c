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
#include <sys/time.h>

#include "exfat.h"
#include "exfat_util.h"

/*
 * Convert ExFAT timestamp to struct timespec
 */
void
exfat_timestamp_to_timespec(uint32_t timestamp, uint8_t time_ms,
                          uint8_t tz_offset, struct timespec *ts)
{
    int year, month, day, hour, min, sec;
    int32_t tzoffset;

    year  = ((timestamp >> 25) & 0x7F) + 1980;
    month = ((timestamp >> 21) & 0x0F);
    day   = ((timestamp >> 16) & 0x1F);
    hour  = ((timestamp >> 11) & 0x1F);
    min   = ((timestamp >>  5) & 0x3F);
    sec   = ((timestamp & 0x1F) << 1);

    ts->tv_sec = (year - 1970) * 31536000 +  /* Seconds in a year */
                 month * 2592000 +           /* Seconds in a month (approx) */
                 day * 86400 +               /* Seconds in a day */
                 hour * 3600 +               /* Seconds in an hour */
                 min * 60 +                  /* Seconds in a minute */
                 sec;                        /* Seconds */

    /* Add milliseconds */
    ts->tv_nsec = time_ms * 10000000;

    /* Apply timezone offset (15-minute intervals from UTC-12 to UTC+12) */
    if (tz_offset != 0x80) {  /* 0x80 = timezone offset is not set */
        tzoffset = (int8_t)tz_offset * 15 * 60;
        ts->tv_sec -= tzoffset;
    }
}

/*
 * Convert struct timespec to ExFAT timestamp
 */
void
exfat_timespec_to_timestamp(const struct timespec *ts,
                          uint32_t *timestamp, uint8_t *time_ms,
                          uint8_t *tz_offset)
{
    time_t t = ts->tv_sec;
    int year, month, day, hour, min, sec;

    /* Convert seconds since epoch to date/time */
    year = 1970 + (t / 31536000);
    t %= 31536000;
    month = t / 2592000;
    t %= 2592000;
    day = t / 86400;
    t %= 86400;
    hour = t / 3600;
    t %= 3600;
    min = t / 60;
    sec = t % 60;

    *timestamp = ((year - 1980) << 25) |
                (month << 21) |
                (day   << 16) |
                (hour  << 11) |
                (min   <<  5) |
                (sec   >>  1);

    /* Convert nanoseconds to 10ms units (0-199) */
    *time_ms = ts->tv_nsec / 10000000;

    /* Set timezone to UTC */
    *tz_offset = 0;
}

/*
 * Update timestamps in directory entry
 */
void
exfat_update_timestamps(struct exfat_entry_file *file, int update_mask)
{
    struct timespec ts;
    uint32_t timestamp;
    uint8_t time_ms, tz_offset;

    vfs_timestamp(&ts);
    exfat_timespec_to_timestamp(&ts, &timestamp, &time_ms, &tz_offset);

    if (update_mask & EXFAT_UTIME_ACCESS) {
        file->last_access_timestamp = timestamp;
        file->last_access_tz = tz_offset;
    }
    if (update_mask & EXFAT_UTIME_MODIFY) {
        file->last_modified_timestamp = timestamp;
        file->last_modified_time_ms = time_ms;
        file->last_modified_tz = tz_offset;
    }
    if (update_mask & EXFAT_UTIME_CREATE) {
        file->create_timestamp = timestamp;
        file->create_time_ms = time_ms;
        file->create_tz = tz_offset;
    }
} 