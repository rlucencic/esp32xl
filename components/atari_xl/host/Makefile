# Host build of the Atari core: make -C components/atari_xl/host
#   build/xl_host <os.rom> [frames] [out.ppm]   (see host_main.c)
# The 6502 core is esp32emu's
XL        := ..
M6502     := ../../../../esp32emu/components/m6502
EMUDBG    := ../../../../esp32emu/components/emudbg
DISKSPACE := ../../../../esp32emu/components/diskspace
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DM6502_HOST_HOOK \
             -I$(XL)/include -I$(XL) -I$(M6502)/include -I$(EMUDBG)/include -I$(DISKSPACE)/include

XL_SRCS   := $(wildcard $(XL)/*.c)
XL_OBJS   := $(patsubst $(XL)/%.c,$(BUILD)/xl/%.o,$(XL_SRCS)) \
             $(BUILD)/m6502.o $(BUILD)/m6502_dbg.o $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o \
             $(BUILD)/diskspace_atari.o

all: $(BUILD)/xl_host

$(BUILD)/xl_host: host_main.c $(XL_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/xl/%.o: $(XL)/%.c $(wildcard $(XL)/*.h $(XL)/include/*.h) $(M6502)/include/m6502.h $(EMUDBG)/include/emudbg.h \
             $(DISKSPACE)/include/diskspace.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(M6502)/%.c $(M6502)/include/m6502.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# (only the Atari's counter of esp32emu's diskspace)
$(BUILD)/diskspace_atari.o: $(DISKSPACE)/atari.c $(DISKSPACE)/include/diskspace.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
