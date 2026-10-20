/*
 * serialscope - a terminal serial monitor for embedded developers.
 *
 * Reads a serial port and prints what arrives (optionally timestamped, as a
 * hex dump, filtered or highlighted by regex, and logged to a file), forwards
 * your keystrokes to the device, and reconnects automatically when the board
 * resets or is unplugged. Exit with Ctrl-].
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "output.h"
#include "serial.h"

#define VERSION        "0.1.0"
#define EXIT_KEY       0x1d   /* Ctrl-] */
#define IDLE_MS        100    /* flush partial lines after this much silence */
#define RECONNECT_MS   500

typedef enum { EOL_CR, EOL_LF, EOL_CRLF } eol_t;

static volatile sig_atomic_t stop;
static struct termios saved_tty;
static bool tty_raw;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void restore_tty(void)
{
    if (tty_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
    tty_raw = false;
}

/* Put the keyboard in raw mode so every key (Ctrl-C included) goes to the device */
static void raw_tty(void)
{
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_tty) < 0)
        return;
    struct termios raw = saved_tty;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST | ONLCR;   /* keep \n -> \r\n on our own output */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        tty_raw = true;
        atexit(restore_tty);
    }
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: serialscope [options] DEVICE\n"
        "\n"
        "  -b, --baud N          baud rate (default 115200)\n"
        "  -t, --timestamp       prefix lines with wall-clock time\n"
        "  -T, --relative        prefix lines with seconds since start\n"
        "  -x, --hex             hex dump instead of text\n"
        "  -l, --log FILE        append everything shown to FILE (no colours)\n"
        "  -H, --highlight RE    highlight matches of an extended regex\n"
        "  -f, --filter RE       only show lines matching an extended regex\n"
        "  -i, --ignore-case     case-insensitive --highlight / --filter\n"
        "  -e, --eol cr|lf|crlf  what the Enter key sends (default cr)\n"
        "  -n, --no-reconnect    exit when the device disappears\n"
        "      --color           force ANSI colours (default: only on a terminal)\n"
        "      --no-color        disable ANSI colours\n"
        "  -L, --list            list serial devices and exit\n"
        "  -V, --version         print version and exit\n"
        "  -h, --help            this help\n"
        "\n"
        "Keys are sent to the device as you type. Ctrl-] quits.\n");
}

static int compile_regex(regex_t *re, const char *pattern, bool icase, const char *what)
{
    int rc = regcomp(re, pattern, REG_EXTENDED | (icase ? REG_ICASE : 0));
    if (rc != 0) {
        char err[256];
        regerror(rc, re, err, sizeof err);
        fprintf(stderr, "serialscope: bad %s regex '%s': %s\n", what, pattern, err);
        return -1;
    }
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Send keyboard input to the device. Returns false if the exit key was pressed. */
static bool forward_input(int dev, const unsigned char *buf, ssize_t n, eol_t eol)
{
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == EXIT_KEY && tty_raw)
            return false;
        if (dev < 0)
            continue;   /* disconnected: drop keystrokes */
        if (c == '\r' || c == '\n') {
            static const char *seq[] = { "\r", "\n", "\r\n" };
            const char *s = seq[eol];
            (void)!write(dev, s, strlen(s));
        } else {
            (void)!write(dev, &c, 1);
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    unsigned baud = 115200;
    bool reconnect = true, icase = false;
    const char *highlight = NULL, *filter = NULL, *log_path = NULL;
    eol_t eol = EOL_CR;
    output_config_t cfg = { .color = isatty(STDOUT_FILENO) };

    static const struct option opts[] = {
        { "baud", required_argument, 0, 'b' },     { "timestamp", no_argument, 0, 't' },
        { "relative", no_argument, 0, 'T' },       { "hex", no_argument, 0, 'x' },
        { "log", required_argument, 0, 'l' },      { "highlight", required_argument, 0, 'H' },
        { "filter", required_argument, 0, 'f' },   { "ignore-case", no_argument, 0, 'i' },
        { "eol", required_argument, 0, 'e' },      { "no-reconnect", no_argument, 0, 'n' },
        { "no-color", no_argument, 0, 'C' },       { "color", no_argument, 0, 'c' },
        { "list", no_argument, 0, 'L' },
        { "version", no_argument, 0, 'V' },        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };

    int c;
    while ((c = getopt_long(argc, argv, "b:tTxl:H:f:ie:nLVh", opts, NULL)) != -1) {
        switch (c) {
        case 'b': {
            char *end;
            unsigned long v = strtoul(optarg, &end, 10);
            if (*end || serial_baud_supported((unsigned)v) < 0) {
                fprintf(stderr, "serialscope: unsupported baud rate '%s'\n", optarg);
                return 2;
            }
            baud = (unsigned)v;
            break;
        }
        case 't': cfg.timestamps = TS_ABSOLUTE; break;
        case 'T': cfg.timestamps = TS_RELATIVE; break;
        case 'x': cfg.hex = true; break;
        case 'l': log_path = optarg; break;
        case 'H': highlight = optarg; break;
        case 'f': filter = optarg; break;
        case 'i': icase = true; break;
        case 'e':
            if (!strcmp(optarg, "cr")) eol = EOL_CR;
            else if (!strcmp(optarg, "lf")) eol = EOL_LF;
            else if (!strcmp(optarg, "crlf")) eol = EOL_CRLF;
            else {
                fprintf(stderr, "serialscope: --eol must be cr, lf or crlf\n");
                return 2;
            }
            break;
        case 'n': reconnect = false; break;
        case 'C': cfg.color = false; break;
        case 'c': cfg.color = true; break;
        case 'L': serial_list_devices(); return 0;
        case 'V': printf("serialscope %s\n", VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind != argc - 1) {
        usage(stderr);
        return 2;
    }
    const char *device = argv[optind];

    regex_t hl_re, filter_re;
    if (highlight) {
        if (compile_regex(&hl_re, highlight, icase, "highlight") < 0)
            return 2;
        cfg.highlight = &hl_re;
    }
    if (filter) {
        if (compile_regex(&filter_re, filter, icase, "filter") < 0)
            return 2;
        cfg.filter = &filter_re;
    }
    if (log_path) {
        cfg.log = fopen(log_path, "a");
        if (!cfg.log) {
            fprintf(stderr, "serialscope: cannot open log '%s': %s\n", log_path, strerror(errno));
            return 1;
        }
    }

    output_t *out = output_create(&cfg, stdout);
    if (!out)
        return 1;

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int dev = serial_open(device, baud);
    if (dev < 0) {
        if (!reconnect) {
            fprintf(stderr, "serialscope: %s: %s\n", device, strerror(errno));
            return 1;
        }
        output_status(out, "waiting for %s (%s)", device, strerror(errno));
    } else {
        output_status(out, "connected to %s at %u baud%s", device, baud,
                      isatty(STDIN_FILENO) ? ", Ctrl-] to quit" : "");
    }
    raw_tty();

    bool stdin_open = true;
    int exit_code = 0;
    unsigned char buf[4096];

    while (!stop) {
        struct pollfd pfd[2];
        int n = 0;
        int dev_idx = -1, in_idx = -1;
        if (dev >= 0) {
            dev_idx = n;
            pfd[n++] = (struct pollfd){ .fd = dev, .events = POLLIN };
        }
        if (stdin_open) {
            in_idx = n;
            pfd[n++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
        }

        int timeout = dev >= 0 ? IDLE_MS : RECONNECT_MS;
        int rc = poll(pfd, (nfds_t)n, timeout);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("serialscope: poll");
            exit_code = 1;
            break;
        }

        if (rc == 0) {
            output_idle(out);
            if (dev < 0 && reconnect) {
                dev = serial_open(device, baud);
                if (dev >= 0)
                    output_status(out, "reconnected to %s", device);
            }
            continue;
        }

        if (in_idx >= 0 && pfd[in_idx].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
            if (r <= 0)
                stdin_open = false;            /* stdin closed: keep monitoring */
            else if (!forward_input(dev, buf, r, eol))
                break;                         /* Ctrl-] */
        }

        if (dev_idx >= 0 && pfd[dev_idx].revents) {
            ssize_t r = read(dev, buf, sizeof buf);
            if (r > 0) {
                output_feed(out, buf, (size_t)r);
            } else if (r == 0 || (errno != EAGAIN && errno != EINTR) ||
                       pfd[dev_idx].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                /* Device gone (unplugged, board reset re-enumerating USB, pty closed) */
                close(dev);
                dev = -1;
                if (!reconnect) {
                    output_status(out, "%s disconnected", device);
                    exit_code = 1;
                    break;
                }
                output_status(out, "%s disconnected, waiting...", device);
                sleep_ms(RECONNECT_MS);
            }
        }
    }

    restore_tty();
    output_destroy(out);
    if (dev >= 0)
        close(dev);
    if (cfg.log)
        fclose(cfg.log);
    if (highlight)
        regfree(&hl_re);
    if (filter)
        regfree(&filter_re);
    return exit_code;
}
