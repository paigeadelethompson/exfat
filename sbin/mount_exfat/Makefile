PROG=	mount_exfat
MAN=	mount_exfat.8

SRCS=	mount_exfat.c getmntopts.c

CFLAGS+=	-Wall -I../../sys
LDADD+=		-lutil

CLEANFILES+=	.depend .depend.*

.include "/usr/src/share/mk/bsd.prog.mk" 