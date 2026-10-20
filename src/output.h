#ifndef OUTPUT_H
#define OUTPUT_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum { TS_NONE, TS_ABSOLUTE, TS_RELATIVE } timestamp_mode_t;

typedef struct {
    timestamp_mode_t timestamps;
    bool hex;
    bool color;              /* ANSI colours on the terminal (never in the log) */
    regex_t *highlight;      /* NULL = off */
    regex_t *filter;         /* NULL = show every line */
    FILE *log;               /* NULL = no log file */
} output_config_t;

typedef struct output output_t;

output_t *output_create(const output_config_t *cfg, FILE *term);
void output_destroy(output_t *o);

/* Feed bytes received from the device */
void output_feed(output_t *o, const unsigned char *data, size_t len);

/* Nothing received for a while: emit a partial line / hex row so it isn't stuck */
void output_idle(output_t *o);

/* Status message from serialscope itself (connect, disconnect, ...) */
void output_status(output_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
