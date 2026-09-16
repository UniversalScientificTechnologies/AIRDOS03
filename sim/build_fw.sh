#!/bin/bash
# Build AIRDOS03 firmware variants as ELF files for the simulator.
#
#   ./build_fw.sh                    both variants
#   ./build_fw.sh AIRDOS03_USTDFF    one variant
#
# The sources in ../fw are compiled unchanged. Each variant gets an
# out-of-tree PlatformIO project in build/pio/ whose src_dir points to
# ../fw/<variant>/src and whose board definition is taken from the
# TFUNIPAYLOAD01 submodule, so nothing is written into ../fw.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
TFUNIPAYLOAD_DIR="$REPO/hw/modules/TFUNIPAYLOAD01"

if [ -n "${PIO:-}" ]; then :
elif [ -x "$HERE/.venv/bin/pio" ]; then PIO="$HERE/.venv/bin/pio"
elif command -v pio >/dev/null; then PIO=pio
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then PIO="$HOME/.platformio/penv/bin/pio"
else
	echo "PlatformIO not found: run ./setup.sh (installs it into .venv)" >&2
	exit 1
fi

if [ ! -f "$TFUNIPAYLOAD_DIR/fw/platformio.ini" ]; then
	echo "Missing $TFUNIPAYLOAD_DIR - run: git submodule update --init hw/modules/TFUNIPAYLOAD01" >&2
	exit 1
fi

variants=("$@")
[ ${#variants[@]} -gt 0 ] || variants=(AIRDOS03_USTDFF AIRDOS03_MAVLink)

mkdir -p "$HERE/build/fw"
for variant in "${variants[@]}"; do
	if [ "$variant" = AIRDOS03_MAVLink ] && [ ! -f "$REPO/fw/AIRDOS03_MAVLink/src/mavlink/common/mavlink.h" ]; then
		echo "Missing MAVLink headers - run: git submodule update --init fw/AIRDOS03_MAVLink/src/mavlink" >&2
		exit 1
	fi
	proj="$HERE/build/pio/$variant"
	mkdir -p "$proj"
	# canonical board configuration, only the paths are redirected
	sed -e "s#^boards_dir = .*#boards_dir = $TFUNIPAYLOAD_DIR/fw/boards\nsrc_dir = $REPO/fw/$variant/src\nbuild_dir = .pio/build#" \
	    "$TFUNIPAYLOAD_DIR/fw/platformio.ini" > "$proj/platformio.ini"

	echo "=== building $variant"
	"$PIO" run -d "$proj" -e TFUNIPAYLOAD01_uart | tail -n 4
	cp "$proj/.pio/build/TFUNIPAYLOAD01_uart/firmware.elf" "$HERE/build/fw/$variant.elf"
	echo "-> build/fw/$variant.elf"
done
