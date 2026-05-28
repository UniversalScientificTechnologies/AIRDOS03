# AIRDOS03_USTDFF — UST Dosimeters File Format firmware

This firmware variant outputs data in the [UST Dosimeters File Format](https://docs.dos.ust.cz/xdos_format) — the same plain-text ASCII format used by AIRDOS04, SPACEDOS, and GEODOS instruments. It is the preferred choice when connecting AIRDOS03 to a generic UART data logger, a PC over USB (via [TFUSBSERIAL01](https://docs.thunderfly.cz/avionics/TFUSBSERIAL01/)), or any system that already processes the shared UST format.

The UART port runs at **115200 bps**, 8N1, no hardware flow control required for reception.

## Building (PlatformIO)

```bash
# build only
pio run --project-dir fw/AIRDOS03_USTDFF

# build and upload (resets the board via CTS to enter the bootloader first)
pio run --project-dir fw/AIRDOS03_USTDFF -t upload
```

## Flashing pre-built binary with avrdude

The MightyCore bootloader communicates at 57600 bps (separate from the 115200 bps application baud rate):

```bash
avrdude -v -patmega1284p -carduino -P/dev/ttyUSB0 -b57600 -D \
    -Uflash:w:../build/fw_AIRDOS03_USTDFF_AIRDOS03.hex:i
```

## Output

On power-up the firmware emits a header (`$DOS`, `$ADC`) identifying the device and firmware version, followed by repeated 10-second measurement blocks. Each block contains individual particle events (`$E` lines) and an integrated 64-channel histogram (`$STOP`). Environmental data (`$ENV`) is emitted periodically.

For a full description of the message format see the [UST Dosimeters File Format documentation](https://docs.dos.ust.cz/xdos_format).
