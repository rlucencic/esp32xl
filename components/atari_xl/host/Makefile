# Host build of the Atari core: make -C components/atari_xl/host
#   build/xl_host <os.rom> [frames] [out.ppm]   (see host_main.c)
# The 6502 core is esp32emu's
XL        := ..
M6502     := ../../../../esp32emu/components/m6502
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DM6502_HOST_HOOK \
             -I$(XL)/include -I$(XL) -I$(M6502)/include

XL_SRCS   := $(wildcard $(XL)/*.c)
XL_OBJS   := $(patsubst $(XL)/%.c,$(BUILD)/xl/%.o,$(XL_SRCS)) $(BUILD)/m6502.o

all: $(BUILD)/xl_host

$(BUILD)/xl_host: host_main.c $(XL_OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/xl/%.o: $(XL)/%.c $(wildcard $(XL)/*.h $(XL)/include/*.h) $(M6502)/include/m6502.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m6502.o: $(M6502)/m6502.c $(M6502)/include/m6502.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
