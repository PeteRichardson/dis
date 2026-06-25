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
./build/bin/dis           # built-in demo program
./build/bin/dis prog.hex  # disassemble an Intel HEX file
```

## Test

```sh
ctest --test-dir build
```

## Architecture

This is a 6502 CPU disassembler written in C11. It uses the [vrEmu6502](https://github.com/visrealm/vrEmu6502) library for CPU emulation and disassembly support.

### Source files (`src/`)

- **`dis.c`** — Entry point. Sets up memory, creates a `VrEmu6502` instance, and walks instructions using `vrEmu6502DisassembleInstruction` to format output.
- **`memory.c/h`** — Sparse 6502 memory model. Instead of a flat 64 KiB buffer, it manages up to 16 named regions (ROM, RAM, FILL) in a linear array. Later-added regions shadow earlier ones. `MemRead`/`MemWrite` are passed as callbacks to the vrEmu6502 CPU.
- **`hexfile.c/h`** — Intel HEX file reader. Parses records and writes bytes into the memory model via `MemWrite`.
- **`disassemble.c/h`** — Custom `Dis6502()` function. Disassembles one instruction given a `VrEmu6502*`, the PC address, and pre-fetched opcode bytes, using `vrEmu6502GetOpcodeAddrMode` and `vrEmu6502OpcodeToMnemonicStr` from the library. `vrEmu6502Model` is sourced from `vrEmu6502.h` (not redefined here).
- **`vrEmu6502.c/h`** — Third-party 6502/65C02 emulator library (statically linked via `-DVR_EMU_6502_STATIC`).

### Memory model design

Regions are stored newest-first so a later `MapROM`/`MapRAM` call can shadow an earlier one. `MapResetVector` uses a static 2-byte buffer to place the `$FFFC`–`$FFFD` RESET vector without mapping the entire top page.

### Two disassembly paths

`dis.c` drives the main loop using `vrEmu6502DisassembleInstruction` directly. `disassemble.c` provides `Dis6502()` as a lower-level alternative that takes pre-fetched bytes — intended for callers that have already read memory and want to format a single instruction without re-reading via the CPU callbacks.
