# AIRDOS firmware suitable for connecting a generic UART platform

The firmware outputs [UST Dosimeters file format](https://docs.dos.ust.cz/xdos_format) suitable for interfacing to a generic UART system working as a data logger.

### Loading compiled binaries to MCU


#### Avrdude

In case of using the avrdude and mightyCore bootloader, we should use the following command:

```
avrdude -v -patmega1284p -carduino -P/dev/ttyUSB0 -b57600 -D -Uflash:w:../build/AIRDOS.hex:i
```
