# airdos03_sim - virtual AIRDOS03B board on simavr
#
#   make            build the simulator          -> build/airdos03_sim
#   make firmware   build both firmware variants -> build/fw/*.elf
#   make test       build everything and run the automated checks
#   make clean      remove build/ (make distclean also cleans simavr)

SIMAVR   := simavr
BUILD    := build
MACHINE  := $(shell $(CC) -dumpmachine)
SIMLIB   := $(SIMAVR)/simavr/obj-$(MACHINE)/libsimavr.a

ELF_LIBS := $(shell pkg-config --libs libelf 2>/dev/null)

CFLAGS   += -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
            -I$(SIMAVR)/simavr/sim -I$(SIMAVR)/simavr/cores -I$(SIMAVR)/examples/parts -Isrc \
            $(shell pkg-config --cflags libelf 2>/dev/null)
LDLIBS   += $(SIMLIB) $(ELF_LIBS) -lpthread -lutil -lm

SRC = src/airdos03_sim.c src/i2c_devices.c src/ustsipin03.c src/gnss.c \
      $(SIMAVR)/examples/parts/uart_pty.c

FIRMWARE = $(BUILD)/fw/AIRDOS03_USTDFF.elf $(BUILD)/fw/AIRDOS03_MAVLink.elf

all: $(BUILD)/airdos03_sim

check-deps:
	@test -f $(SIMAVR)/Makefile.common || { echo "simavr submodule missing: run ./setup.sh" \
		"(or git submodule update --init sim/simavr)"; exit 1; }
	@pkg-config --exists libelf || { echo "libelf not found: sudo apt install libelf-dev"; exit 1; }

$(SIMLIB): | check-deps
	$(MAKE) -C $(SIMAVR)/simavr CC=gcc

$(BUILD)/airdos03_sim: $(SRC) src/board.h $(SIMLIB) | check-deps
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

firmware:
	./build_fw.sh

$(BUILD)/fw/%.elf: ../fw/%/src/main.cpp
	./build_fw.sh $*

test: $(BUILD)/airdos03_sim $(FIRMWARE)
	./tests/run_tests.sh

clean:
	rm -rf $(BUILD)

distclean: clean
	-$(MAKE) -C $(SIMAVR)/simavr clean

.PHONY: all check-deps firmware test clean distclean
