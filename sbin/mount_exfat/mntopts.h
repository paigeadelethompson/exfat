/* $FreeBSD$ */

#ifndef MNTOPTS_H
#define MNTOPTS_H

#include <sys/cdefs.h>

struct mntopt {
    const char *name;     /* option name */
    int flags;           /* mount flags */
    int inverse;         /* negative option flags */
};

#define MOPT_STDOPTS     0x0001  /* standard mount options */
#define MOPT_FORCE       0x0002  /* force mount even if unclean */
#define MOPT_UPDATE      0x0004  /* update mount option */
#define MOPT_RO          0x0008  /* read only */
#define MOPT_RW          0x0010  /* read/write */
#define MOPT_NOATIME     0x0020  /* don't update access time */
#define MOPT_ASYNC       0x0040  /* asynchronous I/O */
#define MOPT_SYNC        0x0080  /* synchronous I/O */
#define MOPT_LOOP        0x0100  /* loop device mount */

/* Standard mount options */
#define MOPT_USERQUOTA   0x0200  /* user quota */
#define MOPT_GROUPQUOTA  0x0400  /* group quota */
#define MOPT_FSTAB       0x0800  /* mount from fstab */
#define MOPT_NODEV       0x1000  /* no device special files */
#define MOPT_NOEXEC      0x2000  /* no execution of binaries */
#define MOPT_NOSUID      0x4000  /* no set-user-id bit */
#define MOPT_RDONLY      0x8000  /* read only */
#define MOPT_UNION       0x10000 /* union mount */

extern const struct mntopt mopts[];

/* Function prototype for our getmntopts implementation */
int getmntopts(const char *options, const struct mntopt *m0, int *flagp, int *altflagp);

#endif /* MNTOPTS_H */ 