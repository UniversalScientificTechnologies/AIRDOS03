"""Example: what it takes to test firmware behaviour via the ust_format_checker package.

No extra setup - the `capture` fixture (from the pip package, see ../../README.md) finds the
built ELF on its own, simulates it against board.yaml and returns the output as text. A test
then just reads lines, or messages by prefix, and asserts what the firmware does.

Running it:
    pio run -c xdos_check.ini -e TFUNIPAYLOAD01_uart   # the ELF has to exist first
    pytest xdos/tests/test_sample.py -v
"""


def test_firmware_reports_its_identity(capture):
    # capture is text (str) - search it like any other string
    assert "$DOS," in capture


def test_measurement_block_has_a_start_and_a_stop(capture):
    # capture.messages(prefix) returns the matching messages already split on commas
    assert len(capture.messages("$START")) >= 1
    assert len(capture.messages("$STOP")) >= 1


def test_at_least_one_block_reports_an_event(capture):
    # the scenario (xdos/scenarios/timing.yaml) sends events into the first block only
    counts = [int(fields[4]) for fields in capture.messages("$STOP")]
    assert any(count >= 1 for count in counts), f"no $STOP with events: {counts}"
