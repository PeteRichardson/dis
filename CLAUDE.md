# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

The build directory is pre-configured with CMake + Ninja. To build from source:

```sh
cmake --build build
```

To reconfigure from scratch:

```sh
cmake -B build -G Ninja && cmake --build build
```

The binary lands at `build/bin/dis`.

## Run

```sh
./build/bin/dis                        # built-in demo program
./build/bin/dis prog.hex               # disassemble an Intel HEX file
./build/bin/dis game.rp6502            # ...or an RP6502 ROM (format sniffed, not by extension)
./build/bin/dis --cpu w65c02 prog.hex  # 6502, 6502u, 65c02 (default), w65c02, r65c02
./build/bin/dis --linear game.rp6502   # flat sweep, no control-flow analysis
```

## Test

```sh
ctest --test-dir build
```

## Architecture

This is a 6502 CPU disassembler written in C11. It uses the [vrEmu6502](https://github.com/visrealm/vrEmu6502) library for CPU emulation and disassembly support.

### Source files (`src/`)

- **`dis.c`** — Entry point. Parses `--cpu`, loads a program, then runs `AnalyzeNew` → `AnalyzeRun` → `printAnalyzed` by default; `--linear` (or no known entry point) falls back to a flat `DisRange` sweep. Owns all output formatting.
- **`disassemble.c/h`** — The shippable core: `DisInit`, `DisOne`, `DisRange`. Reaches memory only through a `DisReadFn` callback. `vrEmu6502Model` is sourced from `vrEmu6502.h` (not redefined here).
- **`analyze.c/h`** — Recursive-descent code/data analysis. Depends only on `disassemble.h`. See `docs/design.md`.
- **`test_analyze.c`** — Analysis tests. Hand-assembled programs with known control flow.
- **`test_disassemble.c`** — Three-layer test suite. See `docs/design.md`.
- **`test_hexfile.c`** — Intel HEX parser tests. Generates its own `.hex` files at run time.
- **`memory.c/h`** — Sparse 6502 memory model. Up to 16 named regions (ROM, RAM, FILL) in a linear array; later regions shadow earlier ones. Host-side only.
- **`rp6502file.c/h`** — RP6502 ROM reader. Mirrors the RIA's parser in `src/ria/mon/rom.c` of picocomputer/rp6502. Validates CRC-32, skips XRAM chunks, uses the reset vector as the entry point.
- **`test_rp6502file.c`** — ROM parser tests. Generates its own `.rp6502` files at run time.
- **`hexfile.c/h`** — Intel HEX file reader. Parses records and writes bytes into the memory model via `MemWrite`.
- **`vrEmu6502.c/h`** — Third-party 6502/65C02 emulator library (statically linked via `-DVR_EMU_6502_STATIC`).

`testdata/` holds committed fixtures for tests that need a real file on disk — `w65c02_demo.hex` for `cli_hexfile` and `cli_linear`, `w65c02_demo.rp6502` for `cli_rp6502`, and two real PicoComputer ROMs, `adventure.rp6502` and `rtc.rp6502`, for `cli_adventure_rom`, `cli_adventure_data` and `cli_rtc_rom`. Tests that can generate their inputs do so at run time instead.

### What ships to the PicoComputer

`disassemble.c/h` + `vrEmu6502.c/h`. Nothing else. The RIA supplies its own byte-fetch function, so `memory.c` and `hexfile.c` stay host-side.

Note `vrEmu6502New` does **not** allocate 64 KiB — it mallocs one struct, ~1KB on the Pico. The README used to claim otherwise, and that claim is why `memory.c` exists.

### Gotchas

- `MemWrite` silently drops writes to unmapped addresses. Anything writing into the memory model must map a region first.
- Three decode bugs live upstream in `vrEmu6502.c` and are worked around in `disassemble.c`: `BBR`/`BBS` declared as 2-byte zero page when they are 3-byte `zp,rel`; `AddrModeAcc` never returned by `opcodeToAddrMode`; `JAM` advancing the PC by 2 during execution while its table says 1.
- The analysis has inherent blind spots on 6502: `JSR` with inline data, jump tables and the `RTS` dispatch trick, and self-modifying code. `src/test_analyze.c` documents the first as wrong-but-expected — flip that assertion rather than deleting it if it is ever fixed.
