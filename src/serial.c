#define _DEFAULT_SOURCE
#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static const struct {
    unsigned baud;
    speed_t code;
} baud_table[] = {
    { 1200, B1200 },       { 2400, B2400 },       { 4800, B4800 },
    { 9600, B9600 },       { 19200, B19200 },     { 38400, B38400 },
    { 57600, B57600 },     { 115200, B115200 },   { 230400, B230400 },
    { 460800, B460800 },   { 500000, B500000 },   { 576000, B576000 },
    { 921600, B921600 },   { 1000000, B1000000 }, { 1500000, B1500000 },
    { 2000000, B2000000 }, { 3000000, B3000000 }, { 4000000, B4000000 },
};

static int baud_code(unsigned baud, speed_t *out)
{
    for (size_t i = 0; i < sizeof baud_table / sizeof baud_table[0]; i++) {
        if (baud_table[i].baud == baud) {
            *out = baud_table[i].code;
            return 0;
        }
    }
    return -1;
}

int serial_baud_supported(unsigned baud)
{
    speed_t code;
    return baud_code(baud, &code);
}

int serial_open(const char *path, unsigned baud)
{
    speed_t code;
    if (baud_code(baud, &code) < 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        goto fail;
    cfmakeraw(&tio);                  /* no echo, no line editing, 8 data bits */
    tio.c_cflag |= CLOCAL | CREAD;    /* ignore modem lines, enable receiver */
    tio.c_cflag &= ~(CSTOPB | CRTSCTS);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, code) < 0 || cfsetospeed(&tio, code) < 0)
        goto fail;
    if (tcsetattr(fd, TCSANOW, &tio) < 0)
        goto fail;
    tcflush(fd, TCIOFLUSH);
    return fd;

fail: {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

void serial_list_devices(void)
{
    static const char *patterns[] = {
        "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyAMA*", "/dev/ttyS[0-9]*", "/dev/serial/by-id/*",
    };
    int found = 0;
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
        glob_t g;
        if (glob(patterns[i], 0, NULL, &g) == 0) {
            for (size_t j = 0; j < g.gl_pathc; j++) {
                /* ttyS* always exist on PCs; only list ones a driver backs */
                if (i == 3) {
                    int fd = open(g.gl_pathv[j], O_RDWR | O_NOCTTY | O_NONBLOCK);
                    struct termios t;
                    int ok = fd >= 0 && tcgetattr(fd, &t) == 0;
                    if (fd >= 0)
                        close(fd);
                    if (!ok)
                        continue;
                }
                printf("%s\n", g.gl_pathv[j]);
                found++;
            }
        }
        globfree(&g);
    }
    if (!found)
        printf("no serial devices found\n");
}
