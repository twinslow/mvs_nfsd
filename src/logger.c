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
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <mvsutils.h>  /* _write2op */
#include "ebcdic.h"
#else
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#endif

#include "types.h"
#include "logger.h"

/* -------------------------------------------------------------------- */
/* Module state                                                          */
/* -------------------------------------------------------------------- */

static log_level_t  g_log_level      = LOG_INFO;
static log_level_t  g_wto_level      = LOG_INFO; /* console (WTO) floor */
static FILE        *g_log_fp         = NULL;    /* NULL -> use stderr   */
static int          g_log_timestamps = 0;

/*
 * Per-procedure level overrides.  Each entry is a log_level_t value, or
 * LOG_LEVEL_INHERIT to follow g_log_level.  log_proc_init() sets every
 * slot to inherit; until it runs the C static-zero (== LOG_DEBUG) is a
 * harmless default because main() also starts the global at LOG_DEBUG.
 */
static int g_proc_level[LOG_PROC_COUNT];

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

log_level_t log_get_level(void)
{
    return g_log_level;
}

void log_set_wto_level(log_level_t min_level)
{
    g_wto_level = min_level;
}

log_level_t log_get_wto_level(void)
{
    return g_wto_level;
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
    case LOG_TRACE: return "TRACE";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "?????";
    }
}

/* Longest line written to the log stream in ONE record.
 *
 * MUST stay below the STDERR DD's record limit.  RECFM=V BLKSIZE=n yields
 * n-8 bytes of data (4-byte BDW + 4-byte RDW), so the started task's
 * BLKSIZE=250 allows 242.  A small BLKSIZE is deliberate -- less sits
 * unwritten when the task dies, which matters because JCC's fflush is a
 * no-op -- so the headroom is bought here rather than by growing the block.
 *
 * Exceeding the record limit is not a truncated line: QSAM abends the task
 * S002-14 and the server dies.  That happened on 2026-07-30, from a single
 * over-long log_warn.  Hence splitting rather than trusting callers to
 * count characters.
 */
#define LOG_LINE_MAX  200

/* Write prefix + body to fp, split across as many records as it takes so no
   single one exceeds LOG_LINE_MAX.  The prefix rides on the first record;
   continuations carry body text only.  Nothing is truncated. */
static void log_emit(FILE *fp, const char *prefix, const char *body)
{
    size_t plen = strlen(prefix);
    size_t blen = strlen(body);
    size_t off  = 0;
    size_t room;

    fwrite(prefix, 1, plen, fp);
    room = (plen < (size_t)LOG_LINE_MAX)
         ? (size_t)LOG_LINE_MAX - plen : (size_t)1;

    for (;;) {
        size_t n = blen - off;
        if (n > room) n = room;
        if (n > 0) fwrite(body + off, 1, n, fp);
        fputc('\n', fp);
        off += n;
        if (off >= blen) break;
        room = (size_t)LOG_LINE_MAX;
    }
}

static void vlog_msg(log_level_t level, const char *fmt, va_list ap)
{
    FILE       *fp;
    struct tm  *tm_ptr;
    time_t      now;
    char        ts_buf[22];   /* "YYYY-MM-DD HH:MM:SS " + NUL */
    char        pfx_buf[32];  /* ts_buf + "[LEVEL] "           */
    char        msg_buf[480]; /* formatted message body        */
    char        wto_buf[490]; /* "[LEVEL] " + msg_buf          */

    /* The log-stream level gates everything: a line that is too detailed
       for the stream is never written to the console either.  The console
       is therefore always a subset of the stream. */
    if (level < g_log_level) return;

    fp = (g_log_fp != NULL) ? g_log_fp : stderr;

    /* Format message body once; reuse for both the log stream and WTO.
       Bounded: several callers pass two or more %s arguments that can each
       be a full MAX_PATH (256) string, which comfortably exceeds msg_buf --
       an unbounded vsprintf here would smash this frame. */
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
    msg_buf[sizeof(msg_buf) - 1] = '\0';   /* some libcs omit the NUL */

#ifdef __MVS__
    /* CONSOLE FIRST, and the order is load-bearing.
     *
     * WTO is a supervisor call and touches none of the C runtime.  The stream
     * write below goes through @STDIO into @IO, which serialises every file
     * operation on one module-scope lock -- and an abend trapped by a STAE
     * can leave that lock held, after which the next file operation deadlocks
     * against itself (see doc/design_nfs_write.md Sec 7.3.1).  Writing the
     * stream first meant the message died in the deadlock it was trying to
     * report, which is why a hung task went silent with no final line.
     *
     * Reaching here already means level >= g_log_level, so the effective
     * console floor is max(g_log_level, g_wto_level) -- raising WTOLVL
     * quietens the console without touching the stream, but it can never
     * surface a line the stream itself has filtered out.
     * No timestamp -- operator messages stay concise.
     */
    if (level >= g_wto_level) {
        snprintf(wto_buf, sizeof(wto_buf), "[%s] %s",
                 level_tag(level), msg_buf);
        wto_buf[sizeof(wto_buf) - 1] = '\0';
        _write2op(wto_buf);
    }
#endif

    /* Optional timestamp prefix (log stream only, not WTO). */
    ts_buf[0] = '\0';
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
        }
    }

    /* The timestamp counts against the record limit, so it is part of the
       prefix rather than a separate write. */
    sprintf(pfx_buf, "%s[%s] ", ts_buf, level_tag(level));
    log_emit(fp, pfx_buf, msg_buf);
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

void log_trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msg(LOG_TRACE, fmt, ap);
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

/* -------------------------------------------------------------------- */
/* Per-procedure level table                                            */
/* -------------------------------------------------------------------- */

void log_proc_init(void)
{
    int i;
    for (i = 0; i < LOG_PROC_COUNT; i++)
        g_proc_level[i] = LOG_LEVEL_INHERIT;
}

int log_proc_set_level(int proc, int level)
{
    if (proc < 0 || proc >= LOG_PROC_COUNT)
        return -1;
    if (level != LOG_LEVEL_INHERIT &&
        (level < (int)LOG_DEBUG || level > (int)LOG_FATAL))
        return -1;
    g_proc_level[proc] = level;
    return 0;
}

log_level_t log_proc_get_level(int proc)
{
    int lvl;

    if (proc < 0 || proc >= LOG_PROC_COUNT)
        return g_log_level;
    lvl = g_proc_level[proc];
    if (lvl == LOG_LEVEL_INHERIT)
        return g_log_level;
    return (log_level_t)lvl;
}

/* -------------------------------------------------------------------- */
/* MODIFY command parsing (SET LOGLVL ...)                              */
/* -------------------------------------------------------------------- */

/*
 * Name <-> value tables.  The literal names are compared with the
 * caller's (uppercased) tokens using strcmp; on MVS both sides are
 * EBCDIC, so the char-literal strings match correctly under JCC.
 */
static const struct { const char *name; log_level_t level; } g_level_names[] = {
    { "DEBUG", LOG_DEBUG },
    { "TRACE", LOG_TRACE },
    { "INFO",  LOG_INFO  },
    { "WARN",  LOG_WARN  },
    { "ERROR", LOG_ERROR },
    { "FATAL", LOG_FATAL }
};

static const struct { const char *name; log_proc_t proc; } g_proc_names[] = {
    { "GETATTR",     LOG_PROC_GETATTR     },
    { "SETATTR",     LOG_PROC_SETATTR     },
    { "LOOKUP",      LOG_PROC_LOOKUP      },
    { "ACCESS",      LOG_PROC_ACCESS      },
    { "READ",        LOG_PROC_READ        },
    { "WRITE",       LOG_PROC_WRITE       },
    { "CREATE",      LOG_PROC_CREATE      },
    { "REMOVE",      LOG_PROC_REMOVE      },
    { "RENAME",      LOG_PROC_RENAME      },
    { "READDIR",     LOG_PROC_READDIR     },
    { "READDIRPLUS", LOG_PROC_READDIRPLUS },
    { "RDIRPLUS",    LOG_PROC_READDIRPLUS },  /* alias */
    { "FSSTAT",      LOG_PROC_FSSTAT      },
    { "FSINFO",      LOG_PROC_FSINFO      },
    { "PATHCONF",    LOG_PROC_PATHCONF    },
    { "COMMIT",      LOG_PROC_COMMIT      },
    { "NULL",        LOG_PROC_NULL        }
};

static int level_from_name(const char *name, log_level_t *out)
{
    int i;
    int n = (int)(sizeof(g_level_names) / sizeof(g_level_names[0]));
    for (i = 0; i < n; i++) {
        if (strcmp(g_level_names[i].name, name) == 0) {
            *out = g_level_names[i].level;
            return 0;
        }
    }
    return -1;
}

static int proc_from_name(const char *name, log_proc_t *out)
{
    int i;
    int n = (int)(sizeof(g_proc_names) / sizeof(g_proc_names[0]));
    for (i = 0; i < n; i++) {
        if (strcmp(g_proc_names[i].name, name) == 0) {
            *out = g_proc_names[i].proc;
            return 0;
        }
    }
    return -1;
}

/* Advance past run of blanks. */
static const char *skip_blanks(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

/*
 * Copy the next token (uppercased) from s into out, stopping at a blank,
 * an '=', or end of string.  out is always NUL-terminated and never
 * overflows outlen.  Returns a pointer to the delimiter that stopped the
 * scan (the caller inspects it to distinguish "PROC=" from "PROC ").
 */
static const char *scan_token(const char *s, char *out, int outlen)
{
    int i = 0;
    while (*s != '\0' && *s != ' ' && *s != '=') {
        if (i < outlen - 1)
            out[i++] = (char)toupper((unsigned char)*s);
        s++;
    }
    out[i] = '\0';
    return s;
}

/*
 * handle_set_loglvl: apply "SET LOGLVL <level> [PROC=<name>]".
 * p points just past the "LOGLVL" keyword.  Returns 0 or -1.
 */
static int handle_set_loglvl(const char *p)
{
    char        tok[24];
    log_level_t level;
    log_proc_t  proc;

    /* Level name (required). */
    p = skip_blanks(p);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (tok[0] == '\0') {
        log_error("SET LOGLVL: missing level");
        return -1;
    }
    if (level_from_name(tok, &level) < 0) {
        log_error("SET LOGLVL: unknown level '%s'", tok);
        return -1;
    }

    /* No further operand -> change the global level. */
    p = skip_blanks(p);
    if (*p == '\0') {
        log_set_level(level);
        log_info("SET LOGLVL: global log level now %s", level_tag(level));
        return 0;
    }

    /* Otherwise expect PROC=<name>. */
    p = scan_token(p, tok, (int)sizeof(tok));
    if (strcmp(tok, "PROC") != 0) {
        log_error("SET LOGLVL: unexpected operand '%s'", tok);
        return -1;
    }
    p = skip_blanks(p);
    if (*p != '=') {
        log_error("SET LOGLVL: expected PROC=<name>");
        return -1;
    }
    p++;                                /* skip '='                      */
    p = skip_blanks(p);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (tok[0] == '\0') {
        log_error("SET LOGLVL: missing PROC name");
        return -1;
    }
    if (proc_from_name(tok, &proc) < 0) {
        log_error("SET LOGLVL: unknown PROC '%s'", tok);
        return -1;
    }

    log_proc_set_level((int)proc, (int)level);
    log_info("SET LOGLVL: PROC %s log level now %s", tok, level_tag(level));
    return 0;
}

/*
 * handle_set_wtolvl: apply "SET WTOLVL <level>".
 * p points just past the "WTOLVL" keyword.  No PROC operand is accepted;
 * the console threshold is a single global value.  Returns 0 or -1.
 */
static int handle_set_wtolvl(const char *p)
{
    char        tok[24];
    log_level_t level;

    p = skip_blanks(p);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (tok[0] == '\0') {
        log_error("SET WTOLVL: missing level");
        return -1;
    }
    if (level_from_name(tok, &level) < 0) {
        log_error("SET WTOLVL: unknown level '%s'", tok);
        return -1;
    }

    p = skip_blanks(p);
    if (*p != '\0') {
        log_error("SET WTOLVL: unexpected operand '%s'", p);
        return -1;
    }

    log_set_wto_level(level);
    log_info("SET WTOLVL: console (WTO) level now %s", level_tag(level));
    return 0;
}

int log_handle_modify(const char *cmd)
{
    char        tok[24];
    const char *p;

    if (cmd == NULL)
        return 1;

    /* Verb must be "SET" for us to look further. */
    p = skip_blanks(cmd);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (strcmp(tok, "SET") != 0)
        return 1;                       /* not a logger command          */

    /* Target keyword selects which threshold we adjust. */
    p = skip_blanks(p);
    p = scan_token(p, tok, (int)sizeof(tok));
    if (strcmp(tok, "LOGLVL") == 0)
        return handle_set_loglvl(p);
    if (strcmp(tok, "WTOLVL") == 0)
        return handle_set_wtolvl(p);

    return 1;                           /* "SET" but not one of ours     */
}
