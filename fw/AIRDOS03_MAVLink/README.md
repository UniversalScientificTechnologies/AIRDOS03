# AIRDOS firwware suitable to connection MAVlink-based UAV platform

The firmware outputs [MAVLink messages](https://en.wikipedia.org/wiki/MAVLink) suitable to interfacing to [TF-ATMON](https://docs.thunderfly.cz/instruments/TF-ATMON) system.

### Loading compiled binaries to MCU


#### Avrdude

In case of using the avrdude and mightyCore bootloader we should use following command:

```
avrdude -v -patmega1284p -carduino -P/dev/ttyUSB0 -b57600 -D -Uflash:w:/tmp/arduino_build_743311/LABDOS.ino.hex:i
```
