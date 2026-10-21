# Atari 800XL and 130XE (platform independent; host build in host/)
idf_component_register(SRCS "xl.c" "antic.c" "gtia.c" "pokey.c" "sio.c" "disk.c" "space.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES m6502
                       PRIV_REQUIRES diskspace)

# Runs in the emulation loop. Jump tables for the switches (the IDF turns
# them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
