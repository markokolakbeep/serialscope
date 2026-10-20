# serialscope

A small terminal serial monitor for embedded developers. Plug in your board, run `serialscope /dev/ttyUSB0`, and you get:

- **Timestamps** on every line: wall clock (`-t`) or seconds since start (`-T`)
- **Regex filter** (`-f 'ERR|WARN'`) to show only the lines you care about
- **Regex highlight** (`-H 'fail|error'`) in red
- **Hex dump** (`-x`) for binary protocols
- **Logging** to a file (`-l session.log`), without colour codes
- **Auto-reconnect** when the board resets or the USB cable is replugged, so you don't miss the boot log
- **Keyboard passthrough**: type to the device, choose what Enter sends (`-e cr|lf|crlf`), quit with `Ctrl-]`

Plain C, no dependencies beyond libc. Linux for now.

## Build and install

```sh
make
sudo make install        # installs to /usr/local/bin
```

You may need to be in the `dialout` (Debian/Ubuntu) or `uucp` (Arch) group to open serial ports:

```sh
sudo usermod -aG dialout $USER    # then log out and back in
```

## Usage

```sh
serialscope --list                          # find your board
serialscope /dev/ttyACM0                    # 115200 baud by default
serialscope -b 9600 -t /dev/ttyUSB0         # 9600 baud, timestamps
serialscope -T -H 'error|fail' -i /dev/ttyUSB0
serialscope -f 'ERR|WARN' -l errors.log /dev/ttyUSB0
serialscope -x /dev/ttyUSB0                 # hex dump
```

```
-- connected to /dev/ttyACM0 at 115200 baud, Ctrl-] to quit
[14:02:11.204] boot: qfc 0.1.0
[14:02:11.209] imu: ICM-42688 ok
[14:02:11.530] rx: CRSF link up
-- /dev/ttyACM0 disconnected, waiting...
-- reconnected to /dev/ttyACM0
```

Run `serialscope --help` for all options.

## Tests

```sh
make test
```

The tests run the real binary against a pseudo-terminal that stands in for a board, so they need no hardware. They cover text, timestamps, filtering, highlighting, hex dumps, keyboard forwarding, logging and disconnect handling.

## Roadmap

- macOS support
- toggle hex/timestamps with hotkeys while running
- send a file to the device
- DTR/RTS control (reset ESP32 / Arduino boards into the bootloader)

Contributions welcome. Open an issue or a pull request.

## License

MIT, see [LICENSE](LICENSE).
