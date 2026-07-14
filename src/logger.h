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

/*
 * Severity ladder, least to most severe.  Lower value == more detailed.
 * The emit filter is  "level < current_level ? drop : print", so setting
 * the level to LOG_TRACE emits TRACE and everything above it.
 *
 * TRACE sits between DEBUG and INFO: more detailed than INFO, less than
 * DEBUG.  It is deliberately below LOG_INFO so TRACE lines go only to the
 * log stream, never to the operator console (WTO fires at INFO+).
 *
 * NOTE: these values are array-friendly ordinals.  Do not reorder or
 * assign explicit values; several tables in logger.c are indexed by them.
 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_TRACE,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

/*
 * Sentinel used in the per-procedure level table (see log_proc_set_level):
 * a procedure carrying this value "inherits" the global level rather than
 * pinning its own.  Chosen outside the log_level_t range so it can never
 * collide with a real level.
 */
#define LOG_LEVEL_INHERIT  (-1)

/* -------------------------------------------------------------------- */
/* Per-NFS3-procedure log slots                                         */
/* -------------------------------------------------------------------- */

/*
 * One slot per NFSv3 procedure whose log verbosity can be tuned
 * independently of the global level.  Add new slots immediately before
 * LOG_PROC_COUNT; the values are array indices into the level table in
 * logger.c, so do not reorder or assign explicit values.
 */
typedef enum {
    LOG_PROC_GETATTR = 0,
    LOG_PROC_SETATTR,
    LOG_PROC_LOOKUP,
    LOG_PROC_ACCESS,
    LOG_PROC_READ,
    LOG_PROC_WRITE,
    LOG_PROC_CREATE,
    LOG_PROC_REMOVE,
    LOG_PROC_RENAME,
    LOG_PROC_READDIR,
    LOG_PROC_READDIRPLUS,
    LOG_PROC_FSSTAT,
    LOG_PROC_FSINFO,
    LOG_PROC_PATHCONF,
    LOG_PROC_COMMIT,
    LOG_PROC_NULL,

    LOG_PROC_COUNT   /* sentinel -- must remain last */
} log_proc_t;

/* -------------------------------------------------------------------- */
/* Configuration                                                        */
/* -------------------------------------------------------------------- */

/* Set the minimum (global) level that is actually emitted to the log
 * stream.  default: LOG_INFO. */
void log_set_level(log_level_t min_level);

/* Return the current global (log-stream) level. */
log_level_t log_get_level(void);

/*
 * Set / get the minimum level sent to the MVS operator console via WTO.
 * default: LOG_INFO.
 *
 * The console is a *subset* of the log stream: a line is written to the
 * console only when it clears both the stream level and the WTO level, so
 * the effective console floor is max(log level, WTO level).  Raising
 * WTOLVL quietens the console independently of the stream, but it can
 * never surface a line the stream level has already filtered out.
 *
 * On non-MVS builds the value is stored but has no effect (there is no
 * console), which keeps the MODIFY-command parser testable off-platform.
 */
void log_set_wto_level(log_level_t min_level);
log_level_t log_get_wto_level(void);

/* -------------------------------------------------------------------- */
/* Per-procedure log levels                                             */
/* -------------------------------------------------------------------- */

/*
 * log_proc_init: reset every per-procedure slot to LOG_LEVEL_INHERIT so
 * all procedures follow the global level.  Call once at startup.
 */
void log_proc_init(void);

/*
 * log_proc_set_level: pin one procedure's level, or clear the pin.
 *
 *   proc  -- a LOG_PROC_* value.
 *   level -- a log_level_t to pin, or LOG_LEVEL_INHERIT to follow global.
 *
 * Returns 0 on success, -1 if proc or level is out of range.
 */
int log_proc_set_level(int proc, int level);

/*
 * log_proc_get_level: effective level for proc.  Returns the pinned
 * level, or the global level when the slot inherits (or proc is out of
 * range).  This is what the dispatcher installs for the call's duration.
 */
log_level_t log_proc_get_level(int proc);

/*
 * log_handle_modify: parse and apply an MVS MODIFY command that targets
 * the logger.  Currently recognises:
 *
 *     SET LOGLVL <level>                -- change the global log level
 *     SET LOGLVL <level> PROC=<name>    -- pin one procedure's log level
 *     SET WTOLVL <level>                -- change the console (WTO) level
 *
 * <level> is DEBUG|TRACE|INFO|WARN|ERROR|FATAL; <name> is an NFS3
 * procedure name (GETATTR, SETATTR, WRITE, ...).  Parsing is
 * case-insensitive.  On MVS the text is EBCDIC; the parser uses only
 * char-literal comparisons so it is correct under JCC.
 *
 * Returns:
 *    0  recognised as a logger command and applied,
 *   -1  recognised as a logger command but malformed (diagnostic logged),
 *    1  not a logger command (the caller should report it as unhandled).
 */
int log_handle_modify(const char *cmd);

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
void log_trace(const char *fmt, ...);
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
