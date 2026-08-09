/*
 * xdr.c - XDR (External Data Representation) encode and decode.
 *
 * XDR is big-endian.  All read/write operations use explicit byte
 * manipulation so this code is correct on both little-endian (Linux
 * x86_64) and big-endian (MVS) hosts.  No htonl() / ntohl() used.
 *
 * XDR rules that matter for NFS:
 *   - All scalars are 4-byte aligned, big-endian.
 *   - uint64 is two uint32 words (high word first).
 *   - Variable-length opaque / string: 4-byte length, then data,
 *     then zero-padding to the next 4-byte boundary.
 */

#include <string.h>   /* memcpy, memset */
#include "nfsd.h"

/* -------------------------------------------------------------------- */
/* Initialise an XDR buffer for reading                                 */
/* -------------------------------------------------------------------- */
void xdr_init_read(xdr_t *x, uint8_t *buf, uint32_t len)
{
    x->base     = buf;
    x->capacity = len;
    x->pos      = 0;
    x->error    = 0;
}

/* -------------------------------------------------------------------- */
/* Initialise an XDR buffer for writing                                 */
/* -------------------------------------------------------------------- */
void xdr_init_write(xdr_t *x, uint8_t *buf, uint32_t cap)
{
    x->base     = buf;
    x->capacity = cap;
    x->pos      = 0;
    x->error    = 0;
}

/* -------------------------------------------------------------------- */
/* Read a big-endian uint32                                             */
/* -------------------------------------------------------------------- */
uint32_t xdr_read_uint32(xdr_t *x)
{
    uint8_t *p;
    uint32_t v;

    if (x->error)             return 0;
    if (x->pos + 4 > x->capacity) { x->error = 1; return 0; }

    p = x->base + x->pos;
    v = ((uint32_t)p[0] << 24)
      | ((uint32_t)p[1] << 16)
      | ((uint32_t)p[2] <<  8)
      |  (uint32_t)p[3];
    x->pos += 4;
    return v;
}

/* -------------------------------------------------------------------- */
/* Read a big-endian uint64 (high word first)                           */
/* -------------------------------------------------------------------- */
uint64_t xdr_read_uint64(xdr_t *x)
{
    uint32_t hi = xdr_read_uint32(x);
    uint32_t lo = xdr_read_uint32(x);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* -------------------------------------------------------------------- */
/* Read exactly 'len' raw bytes (no padding)                            */
/* -------------------------------------------------------------------- */
void xdr_read_raw(xdr_t *x, uint8_t *dst, uint32_t len)
{
    if (x->error)                  return;
    if (x->pos + len > x->capacity) { x->error = 1; return; }
    memcpy(dst, x->base + x->pos, len);
    x->pos += len;
}

/* -------------------------------------------------------------------- */
/* Read a variable-length opaque: 4-byte length + data + padding        */
/* *dstlen is set to the actual data length; dst receives the data.     */
/* -------------------------------------------------------------------- */
void xdr_read_opaque(xdr_t *x, uint8_t *dst,
                     uint32_t *dstlen, uint32_t maxlen)
{
    uint32_t len;
    uint32_t pad;

    len = xdr_read_uint32(x);
    if (x->error) return;

    if (len > maxlen) {
        x->error = 1;
        return;
    }
    xdr_read_raw(x, dst, len);

    /* skip zero-padding to next 4-byte boundary */
    pad = (4u - (len & 3u)) & 3u;
    if (pad > 0) xdr_skip(x, pad);

    if (dstlen) *dstlen = len;
}

/* -------------------------------------------------------------------- */
/* Read a string into a null-terminated C buffer.                       */
/* Returns the string length, or -1 on error.                           */
/* -------------------------------------------------------------------- */
int xdr_read_string(xdr_t *x, char *dst, uint32_t maxlen)
{
    uint32_t len;
    uint32_t pad;

    len = xdr_read_uint32(x);
    if (x->error) return -1;

    /* maxlen includes space for the NUL terminator */
    if (len >= maxlen) {
        /* Skip the data rather than truncating silently */
        pad = (len + 3u) & ~3u;
        xdr_skip(x, pad);
        x->error = 1;
        return -1;
    }
    xdr_read_raw(x, (uint8_t *)dst, len);
    dst[len] = '\0';

    pad = (4u - (len & 3u)) & 3u;
    if (pad > 0) xdr_skip(x, pad);

    return (int)len;
}

/* -------------------------------------------------------------------- */
/* Skip nbytes in the read buffer (advance pos)                         */
/* -------------------------------------------------------------------- */
void xdr_skip(xdr_t *x, uint32_t nbytes)
{
    if (x->error) return;
    if (x->pos + nbytes > x->capacity) { x->error = 1; return; }
    x->pos += nbytes;
}

/* -------------------------------------------------------------------- */
/* Write a big-endian uint32                                            */
/* -------------------------------------------------------------------- */
void xdr_write_uint32(xdr_t *x, uint32_t v)
{
    uint8_t *p;

    if (x->error)                  return;
    if (x->pos + 4 > x->capacity) { x->error = 1; return; }

    p = x->base + x->pos;
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
    x->pos += 4;
}

/* -------------------------------------------------------------------- */
/* Write a big-endian uint64 (high word first)                          */
/* -------------------------------------------------------------------- */
void xdr_write_uint64(xdr_t *x, uint64_t v)
{
    xdr_write_uint32(x, (uint32_t)(v >> 32));
    xdr_write_uint32(x, (uint32_t)(v & 0xFFFFFFFFu));
}

/* -------------------------------------------------------------------- */
/* Write exactly 'len' raw bytes (no length prefix, no padding)         */
/* -------------------------------------------------------------------- */
void xdr_write_raw(xdr_t *x, const uint8_t *src, uint32_t len)
{
    if (x->error)                       return;
    if (x->pos + len > x->capacity)    { x->error = 1; return; }
    memcpy(x->base + x->pos, src, len);
    x->pos += len;
}

/* -------------------------------------------------------------------- */
/* Write a variable-length opaque: 4-byte length + data + padding       */
/* -------------------------------------------------------------------- */
void xdr_write_opaque(xdr_t *x, const uint8_t *src, uint32_t len)
{
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };
    uint32_t pad = (4u - (len & 3u)) & 3u;

    xdr_write_uint32(x, len);
    xdr_write_raw(x, src, len);
    if (pad > 0) xdr_write_raw(x, zeros, pad);
}

/* -------------------------------------------------------------------- */
/* Write a string (same wire encoding as opaque)                        */
/* -------------------------------------------------------------------- */
void xdr_write_string(xdr_t *x, const char *s, uint32_t len)
{
    xdr_write_opaque(x, (const uint8_t *)s, len);
}

/* -------------------------------------------------------------------- */
/* Write an NFS3 file handle: encodes our OUR_FHSIZE-byte handle as an  */
/* opaque.  OUR_FHSIZE is 4-byte aligned, so XDR adds no padding.       */
/* -------------------------------------------------------------------- */
void xdr_write_fhandle(xdr_t *x, const our_fhandle_t *fh)
{
    uint8_t bytes[OUR_FHSIZE];
    fh_encode(fh, bytes);
    xdr_write_opaque(x, bytes, OUR_FHSIZE);
}

/* -------------------------------------------------------------------- */
/* Position accessors (used by READDIR/READDIRPLUS to backtrack)        */
/* -------------------------------------------------------------------- */
uint32_t xdr_get_pos(const xdr_t *x)
{
    return x->pos;
}

void xdr_set_pos(xdr_t *x, uint32_t pos)
{
    if (pos <= x->capacity) x->pos = pos;
    else x->error = 1;
}
