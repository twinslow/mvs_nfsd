/*
 * nfserr.c - errno -> NFSv3 status mapping (RFC 1813 section 2.6).
 *
 * Shared by every VFS backend.  The mapping is fixed by the protocol, not by
 * the filesystem underneath, so all backends must agree on it -- which is why
 * it lives here rather than in vfs.c / mvsvfs.c / mockvfs.c.  It was
 * previously duplicated in all three and had already drifted: mockvfs.c had
 * EXDEV and EROFS commented out while mvsvfs.c mapped both, so the same errno
 * reached the client as two different statuses depending on which backend was
 * linked.
 *
 * NOT MVS-specific -- hence no "mvs" prefix -- and deliberately free of
 * conditional compilation.  Every errno it names is guaranteed to exist
 * because nfsd.h supplies the ones JCC omits (see the errno compatibility
 * block there).  That is what lets one table serve both platforms: a case
 * guarded by #ifdef would silently vanish on JCC and the client would get
 * NFS3ERR_IO from the default arm instead of the right status.
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <errno.h>

#include "nfsd.h"

/*
 * Translate a POSIX errno into the NFSv3 status a client should see.
 *
 * err == 0 is success (NFS3_OK).  Anything not listed maps to NFS3ERR_IO:
 * the honest answer for "something went wrong that the protocol has no word
 * for", and never silently treated as success.
 */
uint32_t vfs_errno_to_nfs3(int err)
{
    switch (err) {
        case 0:             return NFS3_OK;
        case EPERM:         return NFS3ERR_PERM;
        case ENOENT:        return NFS3ERR_NOENT;
        case EIO:           return NFS3ERR_IO;
        case ENXIO:         return NFS3ERR_NXIO;
        case EACCES:        return NFS3ERR_ACCES;
        case EEXIST:        return NFS3ERR_EXIST;
        case EXDEV:         return NFS3ERR_XDEV;
        case ENODEV:        return NFS3ERR_NODEV;
        case ENOTDIR:       return NFS3ERR_NOTDIR;
        case EISDIR:        return NFS3ERR_ISDIR;
        case EINVAL:        return NFS3ERR_INVAL;
        case EFBIG:         return NFS3ERR_FBIG;
        case ENOSPC:        return NFS3ERR_NOSPC;
        case EROFS:         return NFS3ERR_ROFS;
        case EMLINK:        return NFS3ERR_MLINK;
        case ENAMETOOLONG:  return NFS3ERR_NAMETOOLONG;
        case ENOTEMPTY:     return NFS3ERR_NOTEMPTY;
        case EDQUOT:        return NFS3ERR_DQUOT;
        default:            return NFS3ERR_IO;
    }
}
