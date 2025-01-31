#
#

PACKAGE=runtime
PROG=	mount_exfat
SRCS=	mount_exfat.c getmntopts.c
MAN=	mount_exfat.8

MOUNT=	${.CURDIR:H}/mount
CFLAGS+= -I${MOUNT}

.PATH:	${MOUNT}

.include <bsd.prog.mk>
