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
./build/bin/dis --cpu w65c02 prog.hex  # 6502, 6502u, 65c02 (default), w65c02, r65c02
```

## Test

```sh
ctest --test-dir build
```

## Architecture

This is a 6502 CPU disassembler written in C11. It uses the [vrEmu6502](https://github.com/visrealm/vrEmu6502) library for CPU emulation and disassembly support.

### Source files (`src/`)

- **`dis.c`** — Entry point. Parses `--cpu`, loads a program, calls `DisRange`. Owns all output formatting via its `DisEmitFn`.
- **`disassemble.c/h`** — The shippable core: `DisInit`, `DisOne`, `DisRange`. Reaches memory only through a `DisReadFn` callback. `vrEmu6502Model` is sourced from `vrEmu6502.h` (not redefined here).
- **`test_disassemble.c`** — Three-layer test suite. See `docs/design.md`.
- **`memory.c/h`** — Sparse 6502 memory model. Up to 16 named regions (ROM, RAM, FILL) in a linear array; later regions shadow earlier ones. Host-side only.
- **`hexfile.c/h`** — Intel HEX file reader. Parses records and writes bytes into the memory model via `MemWrite`.
- **`vrEmu6502.c/h`** — Third-party 6502/65C02 emulator library (statically linked via `-DVR_EMU_6502_STATIC`).

### What ships to the PicoComputer

`disassemble.c/h` + `vrEmu6502.c/h`. Nothing else. The RIA supplies its own byte-fetch function, so `memory.c` and `hexfile.c` stay host-side.

Note `vrEmu6502New` does **not** allocate 64 KiB — it mallocs one struct, ~1KB on the Pico. The README used to claim otherwise, and that claim is why `memory.c` exists.

### Gotchas

- `MemWrite` silently drops writes to unmapped addresses. Anything writing into the memory model must map a region first.
- Three decode bugs live upstream in `vrEmu6502.c` and are worked around in `disassemble.c`: `BBR`/`BBS` declared as 2-byte zero page when they are 3-byte `zp,rel`; `AddrModeAcc` never returned by `opcodeToAddrMode`; `JAM` advancing the PC by 2 during execution while its table says 1.
