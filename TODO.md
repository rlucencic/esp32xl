# esp32xl roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

1. **Replay a game with both SIO paths.** Use `XL_SCRIPT` and `XL_NOPATCH` with
   a bootable game exercising player/missile graphics and input. Compare
   screenshots, sound and progress past the title, and measure any SIO timing
   difference separately from rendering.

2. **Scope a native XEX launch path.** Start with an existing DOS boot disk as
   the behavioral reference. Cover ordered segments, repeated INITAD and RUNAD,
   and malformed headers. Decide how a firmware launch should initialize the OS
   and RAM while preserving current boot and debugger behavior.

3. **Measure a mid-scanline ANTIC change.** Extend the graphics demo coverage
   with a controlled HSCROL or CHBASE write and compare its location against a
   reference. Include PAL and NTSC captures and use the existing unchanged demo
   images as renderer controls.
