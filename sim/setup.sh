#!/bin/bash
# One-time preparation of the AIRDOS03 simulator.
#
#  1. initialises the submodules the simulator needs (not the large hw/doc ones)
#  2. creates .venv/ with PlatformIO, pymavlink and pyserial
#  3. builds the simulator and both firmware variants
#
# System packages (Debian/Ubuntu):
#   sudo apt install build-essential pkg-config python3-venv libelf-dev
set -euo pipefail
cd "$(dirname "$0")"

missing=()
for tool in git gcc make pkg-config python3; do
	command -v $tool >/dev/null || missing+=($tool)
done
python3 -c "import venv, ensurepip" 2>/dev/null || missing+=(python3-venv)
if [ ${#missing[@]} -gt 0 ]; then
	echo "Missing tools: ${missing[*]}"
	echo "Debian/Ubuntu: sudo apt install build-essential pkg-config python3-venv libelf-dev"
	exit 1
fi

if ! pkg-config --exists libelf; then
	echo "libelf not found. Debian/Ubuntu: sudo apt install libelf-dev"
	exit 1
fi

echo "=== submodules"
if ! git -C .. submodule update --init sim/simavr hw/modules/TFUNIPAYLOAD01 fw/AIRDOS03_MAVLink/src/mavlink; then
	echo
	echo "Submodule checkout failed. The AIRDOS03 submodules use SSH URLs (git@github.com)."
	echo "Without a GitHub SSH key you can use HTTPS for this one command:"
	echo "  git -c url.https://github.com/.insteadOf=git@github.com: -C .. submodule update --init \\"
	echo "      sim/simavr hw/modules/TFUNIPAYLOAD01 fw/AIRDOS03_MAVLink/src/mavlink"
	exit 1
fi

echo "=== python environment (.venv)"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt

echo "=== build"
make
./build_fw.sh

echo
echo "Ready. Run the tests with:  make test"
