#!/usr/bin/env python3
"""
Decode a raw MAVLink capture (airdos03_sim -o capture.bin) to AIRDOS04 text
using the decoder from the AIRDOS03 repository.

    decode_mavlink_file.py capture.bin

mavlink_to_airdos.py --stdin does not work with current pymavlink
("file:/dev/stdin" is parsed as a UDP address), so this wrapper feeds
the bytes to pymavlink's parser directly and reuses process_message().
"""
import argparse
import importlib.util
import os
import sys

sys.dont_write_bytecode = True   # no __pycache__ next to fw/.../tools

from pymavlink.dialects.v20 import common as mavlink2


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--airdos03", default=os.path.join(here, "..", ".."),
                    help="AIRDOS03 repository root (default: this repository)")
    args = ap.parse_args()

    tool = os.path.join(args.airdos03, "fw", "AIRDOS03_MAVLink", "tools", "mavlink_to_airdos.py")
    spec = importlib.util.spec_from_file_location("mavlink_to_airdos", tool)
    dec = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dec)

    parser = mavlink2.MAVLink(None)
    parser.robust_parsing = True
    cycles = {}
    bad = 0
    with open(args.capture, "rb") as f:
        data = f.read()
    for i in range(0, len(data), 256):
        for msg in parser.parse_buffer(data[i:i + 256]) or []:
            if msg.get_type() == "BAD_DATA":
                bad += 1
                continue
            dec.process_message(msg, cycles)
    if bad:
        print(f"# {bad} bad MAVLink frames", file=sys.stderr)
    if cycles:
        print(f"# {len(cycles)} incomplete cycles at end of capture", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
