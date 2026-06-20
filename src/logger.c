/*
 * logger.c - Levelled logging utility for the MVS NFS server.
 *
 * See logger.h for the public API and usage notes.
 *
 * JCC C89 compliance: all variable declarations precede executable
 * statements within each function body.  Block comments only.
 */

#ifdef __MVS__
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include "ebcdic.h"
#else
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#endif

#include "types.h"
#include "logger.h"

/* -------------------------------------------------------------------- */
/* Module state                                                          */
/* -------------------------------------------------------------------- */

static log_level_t  g_log_level      = LOG_INFO;
static FILE        *g_log_fp         = NULL;    /* NULL -> use stderr   */
static int          g_log_timestamps = 0;

/* -------------------------------------------------------------------- */
/* ASCII conversion pool                                                 */
/* -------------------------------------------------------------------- */

static char s_ascii_bufs[LOG_ASCII_MAX_ARGS][LOG_ASCII_BUF_LEN];
static int  s_ascii_buf_idx = 0;

const char *log_ascii(const char *ascii_str)
{
#ifdef __MVS__
    char  *buf;
    size_t len;

    if (ascii_str == NULL) return "(null)";

    buf = s_ascii_bufs[s_ascii_buf_idx];
    s_ascii_buf_idx = (s_ascii_buf_idx + 1) % LOG_ASCII_MAX_ARGS;

    len = strlen(ascii_str);
    if (len >= (size_t)LOG_ASCII_BUF_LEN)
        len = (size_t)(LOG_ASCII_BUF_LEN - 1);

    ascii_to_ebcdic((uint8_t *)buf, (const uint8_t *)ascii_str, len);
    buf[len] = '\0';
    return buf;
#else
    return (ascii_str == NULL) ? "(null)" : ascii_str;
#endif
}

/* -------------------------------------------------------------------- */
/* Configuration                                                         */
/* -------------------------------------------------------------------- */

void log_set_level(log_level_t min_level)
{
    g_log_level = min_level;
}

void log_set_output(FILE *fp)
{
    g_log_fp = fp;
}

void log_set_timestamps(int enabled)
{
    g_log_timestamps = enabled;
}

/* -------------------------------------------------------------------- */
/* Internal helpers                                                      */
/* -------------------------------------------------------------------- */

static const char *level_tag(log_level_t level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "?????";
    }
}

static void vlog_msg(log_level_t level, const char *fmt, va_list ap)
{
    FILE       *fp;
    struct tm  *tm_ptr;
    time_t      now;
    char        ts_buf[22]; /* "YYYY-MM-DD HH:MM:SS " + NUL */

    if (level < g_log_level) return;

    fp = (g_log_fp != NULL) ? g_log_fp : stderr;

    /* Optional timestamp prefix */
    if (g_log_timestamps) {
        now    = time(NULL);
        tm_ptr = gmtime(&now);
        if (tm_ptr != NULL) {
            sprintf(ts_buf, "%04d-%02d-%02d %02d:%02d:%02d ",
                    tm_ptr->tm_year + 1900,
                    tm_ptr->tm_mon  + 1,
                    tm_ptr->tm_mday,
                    tm_ptr->tm_hour,
                    tm_ptr->tm_min,
                    tm_ptr->tm_sec);
            fprintf(fp, "%s", ts_buf);
        }
    }

    fprintf(fp,  "[%s] ", level_tag(level));
    vfprintf(fp, fmt, ap);
    fprintf(fp,  "\n");
    fflush(fp);
}

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */

void log_msg(log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(level, fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(LOG_DEBUG, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(LOG_INFO, fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(LOG_WARN, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(LOG_ERROR, fmt, ap);
    va_end(ap);
}

void log_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(LOG_FATAL, fmt, ap);
    va_end(ap);
}
