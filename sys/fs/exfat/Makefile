KMOD=	exfat

SRCS= exfat_access.c   \
      exfat_dir.c      \
      exfat_direntry.c \
      exfat_fat.c      \
      exfat_lookup.c   \
      exfat_node.c     \
      exfat_upcase.c   \
      exfat_utf16.c    \
      exfat_util.c     \
      exfat_vfsops.c   \
      exfat_vnops.c    \
      exfat_volume.c   \
      vnode_if.h

SYSDIR?= /usr/src/sys
CFLAGS+= -I${.CURDIR} -I${SYSDIR}

CLEANFILES= vnode_if.h vnode_if.c

.PATH: ${.CURDIR}

.include "/usr/src/share/mk/bsd.kmod.mk" 
