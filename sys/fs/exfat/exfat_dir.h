#ifndef _FS_EXFAT_DIR_H_
#define _FS_EXFAT_DIR_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kernel.h>

/* Directory entry set structure */
struct exfat_direntry_set {
    struct exfat_entry_file file;
    struct exfat_entry_stream stream;
    struct exfat_entry_name name[17];  /* Maximum name entries */
    uint8_t name_count;
};

/* Directory scanning context */
struct exfat_scan_ctx {
    struct exfat_mount *emp;
    uint32_t cluster;           /* Current cluster */
    uint32_t offset;            /* Offset within cluster */
    struct buf *bp;             /* Current buffer */
    uint8_t *entry;            /* Current entry pointer */
};

/* Function prototypes */
int exfat_scan_directory(struct vnode *vp, struct exfat_scan_ctx *ctx);
int exfat_next_dirent(struct exfat_scan_ctx *ctx, struct exfat_direntry_set *es);
void exfat_scan_cleanup(struct exfat_scan_ctx *ctx);
int exfat_name_match(struct exfat_mount *emp, const struct exfat_direntry_set *es,
                    const char *name, size_t len);

/* Directory entry operations */
int exfat_create_entry(struct vnode *dvp, const char *name, int namelen,
                      uint16_t attr, struct timespec *ctime,
                      struct exfat_direntry_set *es);
int exfat_remove_entry(struct vnode *dvp, struct exfat_direntry_set *es, off_t offset);

#endif /* _FS_EXFAT_DIR_H_ */ 