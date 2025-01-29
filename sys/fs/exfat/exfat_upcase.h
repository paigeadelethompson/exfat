#ifndef _FS_EXFAT_UPCASE_H_
#define _FS_EXFAT_UPCASE_H_

#include <sys/param.h>
#include <sys/systm.h>

/* Upcase table entry */
struct exfat_entry_upcase {
    uint8_t  type;               /* 0x82 */
    uint8_t  reserved1[3];
    uint32_t checksum;
    uint8_t  reserved2[12];
    uint32_t first_cluster;      /* First cluster of table */
    uint64_t data_length;        /* Size of table in bytes */
} __packed;

/* Function prototypes */
int exfat_init_upcase(struct exfat_mount *emp);
void exfat_cleanup_upcase(struct exfat_mount *emp);
uint16_t exfat_upcase(struct exfat_mount *emp, uint16_t unicode);
int exfat_name_compare(struct exfat_mount *emp, const uint16_t *name1, 
                      const uint16_t *name2, size_t len);

#endif /* _FS_EXFAT_UPCASE_H_ */ 