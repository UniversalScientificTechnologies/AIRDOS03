## Firmware build (PlatformIO)

Firmware projects are maintained as standalone PlatformIO projects in:

- `fw/AIRDOS03_MAVLink`
- `fw/AIRDOS03_USTDFF`

Each firmware project keeps `platformio.ini` as a symlink to the canonical configuration in TFUNIPAYLOAD01:

- `fw/AIRDOS03_MAVLink/platformio.ini -> ../../hw/modules/TFUNIPAYLOAD01/fw/platformio.ini`
- `fw/AIRDOS03_USTDFF/platformio.ini -> ../../hw/modules/TFUNIPAYLOAD01/fw/platformio.ini`

Each project can be built locally with:

```bash
pio run --project-dir fw/<project_name>
```

## Flashing

Use `avrdude` with the generated `.hex` firmware.

```bash
avrdude -v -patmega1284p -carduino -P/dev/ttyUSB0 -b57600 -D -Uflash:w:./firmware.hex:i
```
