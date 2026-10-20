#ifndef SERIAL_H
#define SERIAL_H

/* Open a serial device in raw 8N1 mode at the given baud rate.
 * Returns a non-blocking fd, or -1 with errno set. */
int serial_open(const char *path, unsigned baud);

/* Returns 0 if the baud rate is supported, -1 otherwise. */
int serial_baud_supported(unsigned baud);

/* Print likely serial devices (/dev/ttyUSB*, /dev/ttyACM*, ...) to stdout. */
void serial_list_devices(void);

#endif
