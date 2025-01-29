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
    struct tm tm;
    int32_t tzoffset;

    tm.tm_year = ((timestamp >> 25) & 0x7F) + 80;
    tm.tm_mon  = ((timestamp >> 21) & 0x0F) - 1;
    tm.tm_mday = ((timestamp >> 16) & 0x1F);
    tm.tm_hour = ((timestamp >> 11) & 0x1F);
    tm.tm_min  = ((timestamp >>  5) & 0x3F);
    tm.tm_sec  = ((timestamp & 0x1F) << 1);

    /* Convert to Unix timestamp */
    ts->tv_sec = timegm(&tm);

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
    struct tm tm;
    time_t t = ts->tv_sec;

    gmtime_r(&t, &tm);

    *timestamp = ((tm.tm_year - 80) << 25) |
                ((tm.tm_mon + 1)    << 21) |
                (tm.tm_mday         << 16) |
                (tm.tm_hour         << 11) |
                (tm.tm_min          <<  5) |
                (tm.tm_sec          >>  1);

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