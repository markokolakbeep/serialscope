#define _POSIX_C_SOURCE 200809L
#include "output.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINE_MAX_LEN 4096
#define HEX_ROW 16

#define ANSI_RESET  "\033[0m"
#define ANSI_DIM    "\033[2m"
#define ANSI_HILITE "\033[1;31m"
#define ANSI_STATUS "\033[1;33m"

struct output {
    output_config_t cfg;
    FILE *term;
    struct timespec start;

    char line[LINE_MAX_LEN];     /* current, not yet printed line */
    size_t line_len;
    bool line_started;           /* streaming mode: timestamp already printed */

    unsigned char row[HEX_ROW];  /* hex mode: current row */
    size_t row_len;
    unsigned long offset;
};

output_t *output_create(const output_config_t *cfg, FILE *term)
{
    output_t *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->cfg = *cfg;
    o->term = term;
    clock_gettime(CLOCK_MONOTONIC, &o->start);
    return o;
}

static void timestamp(output_t *o, char *buf, size_t size)
{
    if (o->cfg.timestamps == TS_ABSOLUTE) {
        struct timespec now;
        struct tm tm;
        clock_gettime(CLOCK_REALTIME, &now);
        localtime_r(&now.tv_sec, &tm);
        snprintf(buf, size, "[%02d:%02d:%02d.%03ld] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 now.tv_nsec / 1000000);
    } else if (o->cfg.timestamps == TS_RELATIVE) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (double)(now.tv_sec - o->start.tv_sec) + (now.tv_nsec - o->start.tv_nsec) / 1e9;
        snprintf(buf, size, "[%10.3f] ", t);
    } else {
        buf[0] = '\0';
    }
}

/* Write to the terminal (optionally coloured) and the log (always plain) */
static void emit(output_t *o, const char *color, const char *text, size_t len)
{
    if (color && o->cfg.color)
        fputs(color, o->term);
    fwrite(text, 1, len, o->term);
    if (color && o->cfg.color)
        fputs(ANSI_RESET, o->term);
    if (o->cfg.log)
        fwrite(text, 1, len, o->cfg.log);
}

static void emit_timestamp(output_t *o)
{
    char ts[32];
    timestamp(o, ts, sizeof ts);
    if (ts[0])
        emit(o, ANSI_DIM, ts, strlen(ts));
}

/* Print a complete (or forced-out) line with highlighting */
static void print_line(output_t *o, bool newline)
{
    o->line[o->line_len] = '\0';
    if (o->cfg.filter && regexec(o->cfg.filter, o->line, 0, NULL, 0) != 0) {
        o->line_len = 0;
        return;
    }

    emit_timestamp(o);
    const char *p = o->line;
    regmatch_t m;
    while (o->cfg.highlight && *p && regexec(o->cfg.highlight, p, 1, &m, p == o->line ? 0 : REG_NOTBOL) == 0) {
        if (m.rm_eo == m.rm_so) {   /* empty match: avoid looping forever */
            emit(o, NULL, p, 1);
            p++;
            continue;
        }
        emit(o, NULL, p, (size_t)m.rm_so);
        emit(o, ANSI_HILITE, p + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
        p += m.rm_eo;
    }
    emit(o, NULL, p, strlen(p));
    if (newline)
        emit(o, NULL, "\n", 1);
    o->line_len = 0;
}

static bool line_buffered(const output_t *o)
{
    return o->cfg.filter || o->cfg.highlight;
}

static void feed_text(output_t *o, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r')
            continue;            /* treat CRLF and LF alike */

        if (line_buffered(o)) {
            if (c == '\n' || o->line_len == LINE_MAX_LEN - 1) {
                print_line(o, true);
                if (c == '\n')
                    continue;
            }
            o->line[o->line_len++] = c;
            continue;
        }

        /* Streaming: print bytes as they arrive, timestamp at each line start */
        if (!o->line_started) {
            emit_timestamp(o);
            o->line_started = true;
        }
        emit(o, NULL, &c, 1);
        if (c == '\n')
            o->line_started = false;
    }
}

static void print_hex_row(output_t *o)
{
    char buf[128];
    int n = snprintf(buf, sizeof buf, "%08lx  ", o->offset);
    for (size_t i = 0; i < HEX_ROW; i++) {
        if (i < o->row_len)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%02x ", o->row[i]);
        else
            n += snprintf(buf + n, sizeof buf - (size_t)n, "   ");
        if (i == 7)
            buf[n++] = ' ';
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n, " |");
    for (size_t i = 0; i < o->row_len; i++)
        buf[n++] = (o->row[i] >= 0x20 && o->row[i] < 0x7f) ? (char)o->row[i] : '.';
    n += snprintf(buf + n, sizeof buf - (size_t)n, "|\n");

    emit_timestamp(o);
    emit(o, NULL, buf, (size_t)n);
    o->offset += o->row_len;
    o->row_len = 0;
}

void output_feed(output_t *o, const unsigned char *data, size_t len)
{
    if (o->cfg.hex) {
        for (size_t i = 0; i < len; i++) {
            o->row[o->row_len++] = data[i];
            if (o->row_len == HEX_ROW)
                print_hex_row(o);
        }
    } else {
        feed_text(o, data, len);
    }
    fflush(o->term);
    if (o->cfg.log)
        fflush(o->cfg.log);
}

void output_idle(output_t *o)
{
    if (o->cfg.hex && o->row_len)
        print_hex_row(o);
    else if (!o->cfg.hex && line_buffered(o) && o->line_len)
        print_line(o, true);
    fflush(o->term);
}

void output_status(output_t *o, const char *fmt, ...)
{
    output_idle(o);
    if (o->line_started) {      /* don't glue the message onto a half-printed line */
        emit(o, NULL, "\n", 1);
        o->line_started = false;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof msg - 2)
        n = (int)sizeof msg - 2;
    msg[n++] = '\n';
    emit(o, ANSI_STATUS, "-- ", 3);
    emit(o, ANSI_STATUS, msg, (size_t)n);
    fflush(o->term);
}

void output_destroy(output_t *o)
{
    if (!o)
        return;
    output_idle(o);
    free(o);
}
