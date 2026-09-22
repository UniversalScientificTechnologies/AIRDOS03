# Output format check (xdos-check)

After a build the firmware is run in an ATmega1284P simulation (simavr) against the board
model in [board.yaml](board.yaml), and its output is checked to still conform to the
[UST Dosimeters File Format](https://docs.dos.ust.cz/xdos_format).

**Locally the check only warns and never stops the build.** CI is what blocks.

## What is here

| File | What it is for |
|---|---|
| [board.yaml](board.yaml) | what sits at which I2C address and on which pin |
| [scenarios/basic.yaml](scenarios/basic.yaml) | what the simulation feeds the firmware (events, sensors, GNSS) |
| [checker.txt](checker.txt) | pinned version of the checker |
| [hook.py](hook.py) | wiring into PlatformIO (build and upload) |
| [../xdos_check.ini](../xdos_check.ini) | extra config that turns the hook on |

The checks themselves, the format schema and the component models live in the
`ust_format_checker` package in
[DOSPORTAL](https://github.com/UniversalScientificTechnologies/DOSPORTAL), so that they are
shared by all xDOS devices.

## Setup

Install requirements:

```bash
pipx install platformio # if not available yet
sudo apt install simavr libsimavr-dev libelf-dev gcc
```

Run it:

```bash
pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart            # check after a build
pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart -t upload  # before an upload as well
XDOS_CHECK=0 pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart   # check disabled
```

## In CI

[.github/workflows/xdos_check.yml](../../../.github/workflows/xdos_check.yml) runs the same
check on every PR to `AIRDOS03B` and on every push to `AIRDOS03B`.

Findings are emitted as GitHub annotations, so they show up on the run page and in the PR
check box without opening the log. The full report goes to the run summary, and the
simulation capture is kept as an artifact.

In CI the checker version is the one from [checker.txt](checker.txt) as well, so it matches
what you have on your desk.

## Tests specific to this device

[tests/](tests) holds tests of behaviour that belongs to AIRDOS03.

```bash
pip install ust-format-checker pytest       # one-off
pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart   # the ELF has to exist
pytest xdos/tests
```

There is no `conftest.py` here. The `capture` fixture comes from the package as a pytest
plugin: it finds the built ELF, simulates it against [board.yaml](board.yaml) and returns what
the firmware printed. The simulation runs once even when several tests depend on it. A
scenario other than the default [scenarios/timing.yaml](scenarios/timing.yaml) is selected
with `--xdos-scenario`.

Without a built firmware, or without simavr, the tests skip rather than fail.

**Format rules do not belong here.** They live in the `ust_format_checker` package — written here, other devices would not know about them. What is asserted here is only what is specific to this firmware.

## When the check finds something

Three levels:

- **error** — the format is broken, in CI this stops the merge,
- **warning** — worth attention, for example when DOSPORTAL silently drops a line,
- **info** — an observation, such as a new message or a field appended at the end.

An intentional format change goes through the format itself, not through this repository:
describe the field in the format documentation, add it to `messages.yaml` in the checker and
release a new version, then teach dosview and the DOSPORTAL parser to read it. Until all of
that has happened the finding stands, because until then the new data reaches nobody.

## Writing a scenario

Scenarios live in [scenarios/](scenarios). Values may be random within the limits of the
component; the seed is derived from the scenario name, so runs stay reproducible:

```yaml
name: high_rate
stop_blocks: 2
sensors:
  sht31:
    temp_c: rand(-40, 85)
    humidity: rand(0, 100)
events:
  at: 1.2
  channels: [0, 12, 40, 63, 64, 100, 500, 1023]
gnss:
  fix_at: 1.0
  unix: 1789560000
```
