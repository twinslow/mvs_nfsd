/*
 * logger.h - Levelled logging utility for the MVS NFS server.
 *
 * Usage:
 *
 *   log_set_level(LOG_DEBUG);          -- show all messages
 *   log_set_output(my_fp);             -- redirect (default: stderr)
 *   log_set_timestamps(1);             -- prepend "YYYY-MM-DD HH:MM:SS "
 *
 *   log_info("loaded %d export(s)", n);
 *   log_warn("client sent bad XID");
 *   log_error("bind port %d: %s", port, strerror(errno));
 *
 * ASCII strings on MVS:
 *
 *   On MVS, fprintf() writes EBCDIC.  Any runtime string that is held
 *   in ASCII (e.g. NFS client paths, PDS member names) must be wrapped
 *   in log_ascii() before being passed as a %s argument:
 *
 *     log_info("vfs_open path=\"%s\"", log_ascii(path));
 *
 *   log_ascii() converts up to LOG_ASCII_MAX_ARGS ASCII strings per
 *   log call, cycling through a small pool of static buffers.  On
 *   non-MVS platforms it is a no-op that returns the original pointer.
 *
 * JCC C89 compliance: no C99 features are used.
 */

#ifndef LOGGER_H_INCLUDED
#define LOGGER_H_INCLUDED

#include <stdio.h>

/* -------------------------------------------------------------------- */
/* Log levels                                                           */
/* -------------------------------------------------------------------- */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

/* -------------------------------------------------------------------- */
/* Configuration                                                        */
/* -------------------------------------------------------------------- */

/* Set the minimum level that is actually emitted (default: LOG_INFO). */
void log_set_level(log_level_t min_level);

/* Redirect log output to fp (default: stderr).  Pass NULL to restore
 * stderr.  The caller retains ownership of fp. */
void log_set_output(FILE *fp);

/* Enable (1) or disable (0) the "YYYY-MM-DD HH:MM:SS " timestamp
 * prefix on every line (default: disabled). */
void log_set_timestamps(int enabled);

/* -------------------------------------------------------------------- */
/* Logging functions                                                    */
/* -------------------------------------------------------------------- */

void log_msg  (log_level_t level, const char *fmt, ...);

void log_debug(const char *fmt, ...);
void log_info (const char *fmt, ...);
void log_warn (const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_fatal(const char *fmt, ...);

/* -------------------------------------------------------------------- */
/* ASCII -> EBCDIC helper for log arguments (MVS only)                 */
/* -------------------------------------------------------------------- */

/*
 * Maximum number of ASCII string arguments supported in a single log
 * call.  Each call to log_ascii() consumes one slot; the pool rotates
 * so the converted strings remain valid until vfprintf() returns.
 */
#define LOG_ASCII_MAX_ARGS  4
#define LOG_ASCII_BUF_LEN   256

/*
 * log_ascii: convert an ASCII string to EBCDIC so it can be passed as
 * a %s argument to any log_* function on MVS.
 *
 * On non-MVS platforms this returns the original pointer unchanged.
 */
const char *log_ascii(const char *ascii_str);

#endif /* LOGGER_H_INCLUDED */
