# AIRDOS03 Firmware

Two firmware variants are available for AIRDOS03B. Both run on the ATmega1284P (8 MHz) on the [TFUNIPAYLOAD01](https://docs.thunderfly.cz/avionics/TFUNIPAYLOAD01/) interface board and communicate at **115200 bps** on the UART (TELEM) port.

| Variant | Directory | Output | Details |
|---|---|---|---|
| **MAVLink** | `AIRDOS03_MAVLink/` | MAVLink TUNNEL packets | For TF-ATMON / PX4 / ArduPilot integration. See [`AIRDOS03_MAVLink/README.md`](AIRDOS03_MAVLink/README.md) |
| **UST DFF** | `AIRDOS03_USTDFF/` | UST Dosimeters File Format (ASCII) | For stand-alone loggers and USB/serial capture. See [`AIRDOS03_USTDFF/README.md`](AIRDOS03_USTDFF/README.md) |

Pre-built `.hex` binaries are in [`build/`](build/).

## Building (PlatformIO)

Each variant is a standalone PlatformIO project. The `platformio.ini` in each project is a symlink to the canonical hardware configuration:

```
AIRDOS03_MAVLink/platformio.ini  ->  ../../hw/modules/TFUNIPAYLOAD01/fw/platformio.ini
AIRDOS03_USTDFF/platformio.ini   ->  ../../hw/modules/TFUNIPAYLOAD01/fw/platformio.ini
```

Build a variant (UART bootloader environment):

```bash
pio run --project-dir fw/AIRDOS03_MAVLink -e TFUNIPAYLOAD01_uart
# or
pio run --project-dir fw/AIRDOS03_USTDFF -e TFUNIPAYLOAD01_uart
```

Build and upload via the UART bootloader (requires the bootloader to be
active — `avrdude` toggles DTR/RTS on the host serial port to reset the
target; with a
[TFUSBSERIAL01](https://docs.thunderfly.cz/tools/TFUSBSERIAL01/) adapter its
RTS output is wired to the board's CTS pin, so this happens automatically;
with a generic adapter, reset the board manually first):

```bash
pio run --project-dir fw/AIRDOS03_MAVLink -e TFUNIPAYLOAD01_uart -t upload
```

## Flashing with avrdude

Any `.hex` binary can be flashed using `avrdude` with the MightyCore bootloader. The bootloader communication runs at 57600 bps (independent of the 115200 bps application baud rate):

```bash
avrdude -v -patmega1284p -carduino -P/dev/ttyUSB0 -b57600 -D -Uflash:w:./firmware.hex:i
```
