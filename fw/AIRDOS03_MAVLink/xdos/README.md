# Output format check (MAVLink variant)

This firmware sends MAVLink TUNNEL packets, so the check decodes them with
[tools/mavlink_to_airdos.py](../tools/mavlink_to_airdos.py) first and judges the resulting text
against the [UST Dosimeters File Format](https://docs.dos.ust.cz/xdos_format). The verdict is
about the firmware and the decoder together.

```bash
pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart
```

Everything else works as described for the
[USTDFF variant](../../AIRDOS03_USTDFF/xdos/README.md).
