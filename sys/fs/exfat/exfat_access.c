#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/priv.h>

#include "exfat.h"

/*
 * Check access permissions
 */
int
exfat_access(struct vnode *vp, accmode_t accmode, struct ucred *cred,
             struct thread *td)
{
    struct exfat_node *ep = VTOE(vp);
    int error;

    /* Handle read-only filesystem */
    if ((accmode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY)) {
        if (vp->v_type == VDIR)
            return EISDIR;
        return EROFS;
    }

    /* Root can do anything except execute */
    if (priv_check_cred(cred, PRIV_VFS_ADMIN) == 0) {
        if ((accmode & VEXEC) && vp->v_type != VDIR && 
            (ep->finfo.attributes & EXFAT_ATTR_DIRECTORY) == 0) {
            return EACCES;
        }
        return 0;
    }

    /* Check if file is read-only */
    if ((accmode & VWRITE) && (ep->finfo.attributes & EXFAT_ATTR_READ_ONLY))
        return EACCES;

    /* Directory must be executable to be searchable */
    if (vp->v_type == VDIR && (accmode & VEXEC) == 0)
        return EACCES;

    return 0;
} 