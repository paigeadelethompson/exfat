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