# dis — Design Document

*Last updated: 2026-06-25*

---

## Overview

`dis` is a command-line 6502 CPU disassembler written in C11. It reads a
program in Intel HEX format, loads it into a sparse memory model, and
walks instructions from the entry point, printing each one as address,
hex bytes, and mnemonic. It is aimed at developers working with 6502
machine code who need to inspect a binary without a full debugger.

---

## Goals and Non-Goals

**Goals:**
- Accept an Intel HEX file and produce a linear disassembly from the
  load address
- Correctly decode all 6502 and 65C02 addressing modes and mnemonics
- Provide a low-level `Dis6502()` API for callers that pre-fetch bytes
  and want to format a single instruction without going through the
  CPU emulator callbacks

**Non-Goals:**
- Execution or emulation — the CPU instance exists only to drive opcode
  decoding, not to run code
- Control-flow-aware disassembly — the current loop is strictly linear
- Symbol/label resolution — `labelMap` is wired up but always `NULL`
- Writing back or patching the binary

---

## Architecture

The program is a thin pipeline: load → map → decode → print. There is
no persistent state beyond the memory regions and the CPU instance.

The five source files divide cleanly into three layers:

**Input** (`hexfile.c`) parses Intel HEX records and writes the raw
bytes into the memory model. It returns the load address of the first
data record and the last address written, giving the caller the exact
range to disassemble.

**Memory** (`memory.c`) is a sparse address space. Rather than
allocating a 64 KiB flat buffer (most of which would be empty for a
typical 6502 program), it maintains an array of up to 16 named regions —
ROM, RAM, or constant FILL. `MemRead` and `MemWrite` are plain C
functions passed as callbacks to the CPU emulator; the emulator never
touches the memory directly.

**Decode and output** (`dis.c`, `disassemble.c`, `vrEmu6502.c`) drive
the disassembly loop. `dis.c` holds `main` and the top-level loop.
`vrEmu6502.c` is the third-party emulator that decodes opcodes and
addressing modes. `disassemble.c` provides a standalone `Dis6502()`
function as an alternative formatting path.

---

## Components

**`hexfile.c`**
Parses Intel HEX records line by line. Type `00` (data) records are
written into memory via `MemWrite`; the first such record's load address
becomes the run address. Type `01` (EOF) terminates parsing. No other
record types are handled — in particular, the x86-specific entry-point
records (types `03` and `05`) are ignored, which is correct for 6502
files. Returns the start address and last written address to the caller
as a `uint16_t` return value and `out_end` out-parameter.

**`memory.c`**
Implements the sparse 6502 address space. Regions are stored in
insertion order and searched newest-first, so a later `MapROM` or
`MapRAM` call at an overlapping address silently shadows the earlier
one. This shadowing property is used by `MapResetVector`, which maps a
static 2-byte buffer at `$FFFC–$FFFD` without needing to map the entire
top page. Unmapped addresses return a configurable fill byte (default
`$00`). Writes to ROM and FILL regions are silently ignored.

**`disassemble.c`**
Provides `Dis6502()`, which formats a single instruction given a live
`VrEmu6502*`, a PC address, and three pre-fetched bytes. It calls the
library's `vrEmu6502GetOpcodeAddrMode` and `vrEmu6502OpcodeToMnemonicStr`
to decode the opcode, then switches on addressing mode to format
operands. It is not yet called from the main loop — `dis.c` currently
uses `vrEmu6502DisassembleInstruction` directly, which re-reads bytes
via the CPU callbacks. `Dis6502()` exists for callers that have already
fetched the bytes and want to avoid the re-read.

**`vrEmu6502.c/h`**
Third-party 6502/65C02 emulator by Troy Schrapel (vrEmu6502). Linked
statically (`-DVR_EMU_6502_STATIC`). Used here purely for its opcode
tables and disassembly helpers — no clock ticks are run.

---

## Data Flow

When the user runs `./dis prog.hex`:

1. `main` calls `readHexFile("prog.hex", &end)`. The parser opens the
   file, iterates records, and calls `MemWrite` for every data byte.
   It returns the load address of the first record as `base`.

2. `main` calls `MapResetVector(base)` to place a valid RESET vector,
   then creates a `VrEmu6502` instance with `MemRead`/`MemWrite` as
   its memory callbacks.

3. The loop runs `pc = base` to `pc <= end`. Each iteration calls
   `vrEmu6502DisassembleInstruction`, which reads the opcode and
   operand bytes via `MemRead`, formats the mnemonic and operands into
   a buffer, and returns the next PC. The loop prints the address, raw
   hex bytes, and formatted mnemonic, then advances `pc`.

When run with no arguments, step 1 is replaced by mapping a hardcoded
12-byte demo program as ROM at `$0200`.

---

## Key Design Decisions

**Sparse memory model over flat buffer.** A flat 64 KiB `uint8_t`
array would work but allocates memory for addresses the program never
touches. The region list approach is also more honest about the 6502
memory map: ROM, RAM, and open-bus regions are distinct things with
different write semantics, not just bytes at different addresses.

**vrEmu6502 for opcode decoding.** Building a full 6502 opcode and
addressing-mode table from scratch is straightforward but tedious and
error-prone across CPU variants (6502, 65C02, undocumented opcodes).
The vrEmu6502 library already encodes this correctly for multiple
variants; using it means variant support comes for free.

**`Dis6502()` takes pre-fetched bytes.** The library's own
`vrEmu6502DisassembleInstruction` re-reads memory through the CPU
callbacks, which is fine when memory is already loaded. `Dis6502()`
is intended for a future path where the caller controls byte fetching
(e.g., from a file buffer, not through the emulator's read function).
This separation keeps the formatting logic independent of the memory
access strategy.

**Start address from first data record.** The Intel HEX format has no
standard mechanism for a 6502 entry point. The x86 entry-point record
types (`03`, `05`) are meaningless on 6502. Using the first data
record's load address is the conventional approach for 6502 toolchains.

---

## External Dependencies

| Dependency | Purpose |
|------------|---------|
| vrEmu6502 | 6502/65C02 opcode tables, addressing mode decoding, and disassembly formatting. Vendored in `src/`. |

---

## Configuration and Environment

No environment variables or config files. The CPU model is hardcoded as
`CPU_65C02` in `dis.c`. Build requires CMake ≥ 3.16 and a C11 compiler.
A pre-configured Ninja build tree lives in `build/`.

---

## Open Questions

- [ ] `Dis6502()` is never called from `main` — what should trigger the
      switch from `vrEmu6502DisassembleInstruction` to `Dis6502()`?
- [ ] The CPU model (`CPU_65C02`) is hardcoded; should it be selectable
      via a CLI flag?
- [ ] Label/symbol support: `labelMap` is always `NULL`. What format
      should a label file take, and how is it passed in?
- [ ] Linear disassembly past the end of loaded data produces no output
      (the loop stops at `end`), but the end address comes from the last
      *written* byte, not necessarily the last instruction boundary — a
      multi-byte instruction straddling `end` will be truncated silently.
- [ ] No output format options (e.g., ca65 syntax, DASM syntax).

---

## Document History

| Date | Change |
|------|--------|
| 2026-06-25 | Initial document generated from codebase |
