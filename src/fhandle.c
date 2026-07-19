/*
 * fhandle.c - NFS file handle construction, resolution, and wire format.
 *
 * The handle is SELF-DESCRIBING: it carries the name of the object it
 * refers to, so resolving it needs no server-side state.  There is no
 * cache, nothing to evict, and a handle stays valid across a server
 * restart -- which is what RFC 1813 requires.  NFS3ERR_STALE is returned
 * only when the object is genuinely unreachable (its export or dataset is
 * no longer exported), never because the server forgot something.
 *
 * See doc/readme_filehandles.md for the full rationale and layout.
 *
 * WIRE LAYOUT (60 bytes; <= NFS3_FHSIZE 64, and 4-byte aligned so XDR
 * adds no padding):
 *
 *   bytes  0- 3 : magic     = OUR_FH_MAGIC ('NFS3'), big-endian
 *   bytes  4- 7 : export_id = stable hash of the export path, big-endian
 *   bytes  8-51 : dsname    = 44 bytes, ASCII, blank-padded
 *   bytes 52-59 : member    =  8 bytes, ASCII, blank-padded
 *
 * Object kinds are distinguished by which name fields are set:
 *   export root   -> dsname all blank, member all blank
 *   PDS directory -> dsname set,       member all blank
 *   PDS member    -> dsname set,       member set
 *
 * ASCII, not EBCDIC: the names are stored in the handle exactly as they
 * travel on the wire, so a handle is readable in a packet trace.  That
 * means the pad/trim character MUST be the literal 0x20 (FH_PAD_CHAR) --
 * NOT ' ', which under JCC on MVS is EBCDIC 0x40.
 *
 * export_id is a HASH of the export path, not a table index: an index
 * would silently resolve to a different export if nfsd.conf were
 * reordered, returning wrong data.  A hash either matches the same export
 * or matches none (-> correctly stale).
 *
 * JCC C89 compliance: declarations precede statements; block comments only.
 */

#include <string.h>
#include <stdio.h>

#include "ebcdic.h"
#include "mvsio.h"     /* mvs_path_type, mvs_get_pds_dsn_and_member */
#include "mvsfid.h"    /* mvs_fid_ino32 -- stable export-path hash   */
#include "nfsd.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/* Internal: copy 'src' into 'n' wire bytes, ASCII-blank padded.        */
/* ------------------------------------------------------------------ */
static void fh_put_field(uint8_t *dst, const char *src, int n)
{
    int i;
    int len;

    len = (int)strlen(src);
    if (len > n) len = n;

    for (i = 0; i < len; i++)
        dst[i] = (uint8_t)src[i];
    for (; i < n; i++)
        dst[i] = (uint8_t)FH_PAD_CHAR;
}

/* ------------------------------------------------------------------ */
/* Internal: copy 'n' wire bytes into a NUL-terminated string with     */
/* trailing ASCII blanks removed.  'dst' must hold n+1 bytes.          */
/* ------------------------------------------------------------------ */
static void fh_get_field(char *dst, const uint8_t *src, int n)
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = (char)src[i];
    dst[n] = '\0';

    /* Trim trailing ASCII blanks (0x20 -- NOT ' ', see the header note). */
    for (i = n - 1; i >= 0; i--) {
        if ((uint8_t)dst[i] != (uint8_t)FH_PAD_CHAR) break;
        dst[i] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Internal: the stable identity of an export = hash of its path.      */
/* ------------------------------------------------------------------ */
static uint32_t fh_export_id(const export_t *exp)
{
    return mvs_fid_ino32(exp->export_path_ebcdic, NULL);
}

/* ------------------------------------------------------------------ */
/* Internal: find the export whose path hashes to 'id', or NULL.       */
/* ------------------------------------------------------------------ */
static export_t *fh_find_export(uint32_t id)
{
    int       i;
    int       n = exports_count();
    export_t *exp;

    for (i = 0; i < n; i++) {
        exp = exports_get(i);
        if (exp != NULL && fh_export_id(exp) == id)
            return exp;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fh_from_path: build the handle that names the object at abspath.     */
/*                                                                      */
/* abspath is the ASCII NFS path (e.g. "/exports/temp.proj.cntl/a.cntl")*/
/* Returns 0 on success, -1 if the path is not within any export.       */
/* ------------------------------------------------------------------ */
int fh_from_path(const char *abspath, our_fhandle_t *fh)
{
    char           ebcdic_path[MAX_PATH];
    char           dsn_ebcdic[MAX_DSNAME_LEN];
    char           mem_ebcdic[FH_MEMBER_LEN + 1];
    int            export_idx;
    int            dataset_idx;
    int            path_type;
    export_t      *exp;
    pds_dataset_t *ds;

    memset(fh, 0, sizeof(*fh));

    ascii_to_ebcdic((uint8_t *)ebcdic_path, (const uint8_t *)abspath,
                    MAX_PATH - 1);
    ebcdic_path[MAX_PATH - 1] = '\0';

    path_type = mvs_path_type(ebcdic_path, &export_idx, &dataset_idx);
    if (path_type < 0) return -1;

    exp = exports_get(export_idx);
    if (exp == NULL) return -1;

    fh->magic     = OUR_FH_MAGIC;
    fh->export_id = fh_export_id(exp);

    /* The export root: no dataset, no member. */
    if (path_type == MVS_PATH_TYPE_ROOT)
        return 0;

    /* Both DATASET and PDS_MEMBER name a dataset.  Take the dsname from
       the config (already ASCII) rather than re-deriving it. */
    ds = export_dataset_get(export_idx, dataset_idx);
    if (ds == NULL) return -1;
    strncpy(fh->dsname, ds->dsname_ascii, MAX_DSNAME_LEN - 1);
    fh->dsname[MAX_DSNAME_LEN - 1] = '\0';

    /* A PDS directory has no member. */
    if (path_type != MVS_PATH_TYPE_PDS_MEMBER)
        return 0;

    /* A member: let the MVS mapping validate + extract it (EBCDIC), then
       convert to the ASCII the handle carries. */
    if (mvs_get_pds_dsn_and_member(ebcdic_path, dsn_ebcdic,
                                   mem_ebcdic, export_idx) < 0)
        return -1;

    ebcdic_to_ascii((uint8_t *)fh->member, (const uint8_t *)mem_ebcdic,
                    strlen(mem_ebcdic));
    fh->member[strlen(mem_ebcdic)] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* fh_resolve: rebuild the ASCII NFS path this handle names.            */
/*                                                                      */
/* Returns 0 on success, -1 only when the handle is TRULY stale: its    */
/* export is gone, or the dataset it names is no longer exported.       */
/* ------------------------------------------------------------------ */
int fh_resolve(const our_fhandle_t *fh, char *abspath, uint32_t maxlen)
{
    export_t      *exp;
    pds_dataset_t *ds;
    pds_dataset_t *cand;
    int            export_idx;
    int            i;
    int            n;
    int            len;
    char           slash;
    char           dot;
    char           member_lc[FH_MEMBER_LEN + 1];
    char           ext_ascii[MAX_FILE_EXT_LEN];

    if (fh->magic != OUR_FH_MAGIC) return -1;

    exp = fh_find_export(fh->export_id);
    if (exp == NULL) {
        log_debug("fh_resolve: no export matches id=0x%08X (stale)",
                  fh->export_id);
        return -1;
    }

    /* The path we build is ASCII, so every separator must be converted:
       a C character literal is EBCDIC under JCC on MVS. */
    slash = (char)ebcdic_to_ascii_c('/');
    dot   = (char)ebcdic_to_ascii_c('.');

    /* No dsname -> the handle IS the export root. */
    if (fh->dsname[0] == '\0') {
        strncpy(abspath, exp->export_path, maxlen - 1);
        abspath[maxlen - 1] = '\0';
        return 0;
    }

    /* Find the named dataset within this export.  Matching on the real
       dsname (not an index) is what makes the handle survive a config
       change: if the dataset is no longer exported, the handle is stale. */
    export_idx = exports_get_id(exp);
    ds = NULL;
    n  = export_dataset_count(export_idx);
    for (i = 0; i < n; i++) {
        cand = export_dataset_get(export_idx, i);
        if (cand != NULL && strcmp(cand->dsname_ascii, fh->dsname) == 0) {
            ds = cand;
            break;
        }
    }
    if (ds == NULL) {
        log_debug("fh_resolve: dataset '%s' not exported (stale)",
                  log_ascii(fh->dsname));
        return -1;
    }

    /* No member -> the handle is the PDS directory. */
    if (fh->member[0] == '\0') {
        snprintf(abspath, maxlen, "%s%c%s",
                 exp->export_path, slash, ds->dirname_ascii);
        return 0;
    }

    /* A member: <export>/<dirname>/<member>.<ext>, lower-cased to match the
       names readdir hands out.  The extension is config-derived, which is
       why it need not live in the handle. */

    /* ASCII-only case fold: the handle is ASCII, so neither tolower() nor
       'A'/'Z' literals (EBCDIC under JCC) may be used here. */
    len = (int)strlen(fh->member);
    for (i = 0; i < len; i++) {
        uint8_t c = (uint8_t)fh->member[i];
        if (c >= 0x41u && c <= 0x5Au)      /* ASCII 'A'..'Z' */
            c = (uint8_t)(c + 0x20u);      /* -> 'a'..'z'    */
        member_lc[i] = (char)c;
    }
    member_lc[len] = '\0';

    /* file_ext is derived from the EBCDIC dsname, so it is EBCDIC. */
    len = (int)strlen(ds->file_ext);
    if (len > MAX_FILE_EXT_LEN - 1) len = MAX_FILE_EXT_LEN - 1;
    ebcdic_to_ascii((uint8_t *)ext_ascii, (const uint8_t *)ds->file_ext,
                    (size_t)len);
    ext_ascii[len] = '\0';

    /* The format string holds ONLY conversion specifiers: a literal '.' or
       '/' in it would be EBCDIC on MVS and corrupt the ASCII path.  A dataset
       with no extension (nofileext) appends neither the '.' nor the ext, so
       the path re-parses to the bare member name -- matching what readdir
       (generate_file_name) and the input parser produce. */
    if (ext_ascii[0] != '\0') {
        snprintf(abspath, maxlen, "%s%c%s%c%s%c%s",
                 exp->export_path, slash, ds->dirname_ascii, slash,
                 member_lc, dot, ext_ascii);
    } else {
        snprintf(abspath, maxlen, "%s%c%s%c%s",
                 exp->export_path, slash, ds->dirname_ascii, slash,
                 member_lc);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* fh_encode: serialise the handle into OUR_FHSIZE wire bytes.          */
/* ------------------------------------------------------------------ */
void fh_encode(const our_fhandle_t *fh, uint8_t *bytes)
{
    bytes[0] = (uint8_t)(fh->magic     >> 24);
    bytes[1] = (uint8_t)(fh->magic     >> 16);
    bytes[2] = (uint8_t)(fh->magic     >>  8);
    bytes[3] = (uint8_t)(fh->magic          );
    bytes[4] = (uint8_t)(fh->export_id >> 24);
    bytes[5] = (uint8_t)(fh->export_id >> 16);
    bytes[6] = (uint8_t)(fh->export_id >>  8);
    bytes[7] = (uint8_t)(fh->export_id      );

    fh_put_field(&bytes[8],  fh->dsname, FH_DSNAME_LEN);
    fh_put_field(&bytes[8 + FH_DSNAME_LEN], fh->member, FH_MEMBER_LEN);
}

/* ------------------------------------------------------------------ */
/* fh_decode: parse 'len' wire bytes into a handle.                     */
/* Returns 0 on success, -1 on a wrong length or bad magic.             */
/* ------------------------------------------------------------------ */
int fh_decode(const uint8_t *bytes, uint32_t len, our_fhandle_t *fh)
{
    if (len != (uint32_t)OUR_FHSIZE) return -1;

    memset(fh, 0, sizeof(*fh));

    fh->magic = ((uint32_t)bytes[0] << 24)
              | ((uint32_t)bytes[1] << 16)
              | ((uint32_t)bytes[2] <<  8)
              |  (uint32_t)bytes[3];

    if (fh->magic != OUR_FH_MAGIC) return -1;

    fh->export_id = ((uint32_t)bytes[4] << 24)
                  | ((uint32_t)bytes[5] << 16)
                  | ((uint32_t)bytes[6] <<  8)
                  |  (uint32_t)bytes[7];

    fh_get_field(fh->dsname, &bytes[8], FH_DSNAME_LEN);
    fh_get_field(fh->member, &bytes[8 + FH_DSNAME_LEN], FH_MEMBER_LEN);
    return 0;
}
