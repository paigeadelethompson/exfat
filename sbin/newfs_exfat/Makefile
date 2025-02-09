MK_BRANCH_PROTECTION=no
OPT_INIT_ALL=none
MK_ZEROREGS=no

PROG=	newfs_exfat
MAN=	newfs_exfat.8

SRCS=	newfs_exfat.c

CFLAGS+=	-Wall -I../../sys
LDADD+=		-lutil

CLEANFILES+=	.depend .depend.*

.include "/usr/src/share/mk/bsd.prog.mk" 
