# dis — Design Document

*Last updated: 2026-07-28*

---

## Overview

`dis` is a 6502/65C02 disassembler written in C11. Its real target is a
`dis <addr> <len>` command in the PicoComputer REPL running on the RP6502's
RIA (a W65C02S); the command-line tool here is the host-side proving ground
for that code.

The disassembler reaches memory only through a caller-supplied byte-fetch
callback, so the same code serves both: the CLI backs it with the sparse
memory model and an Intel HEX loader, while the RIA backs it with its own
memory access routines.

---

## Goals and Non-Goals

**Goals:**
- Disassemble a range given a start address and a length, emitting
  address, hex bytes, and mnemonic per instruction
- Correctly decode all 6502, 65C02 and W65C02 addressing modes and
  mnemonics, selectable at runtime
- Keep the shippable surface to `disassemble.c/h` + `vrEmu6502.c/h`, with
  no dependency on the host-side memory model or file loading
- Accept an Intel HEX file on the host for convenience

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
The shippable core. `DisInit(model)` selects the CPU variant and holds one
`VrEmu6502*` purely for mnemonic and addressing-mode lookup, which the
library exposes only through a handle. `DisOne` formats a single
instruction, fetching exactly the bytes its opcode needs through the
caller's `DisReadFn`. `DisRange` walks a range and hands each instruction
to a `DisEmitFn` rather than printing, so tests capture output directly
and the REPL controls its own formatting.

Two decoding cases cannot be taken from `vrEmu6502GetOpcodeAddrMode` and
are identified here instead. `BBR0-7`/`BBS0-7` take a zero-page byte *and*
a relative byte; `vrEmu6502AddrMode` has no mode for that pairing and the
opcode tables declare them plain zero page, which would report length 2
for a 3-byte instruction. Accumulator mode is never reported at all —
`opcodeToAddrMode` has no case for the `acc` addressing function, so those
opcodes fall through to `AddrModeImp`.

**`vrEmu6502.c/h`**
Third-party 6502/65C02 emulator by Troy Schrapel (vrEmu6502). Linked
statically (`-DVR_EMU_6502_STATIC`). Used here purely for its opcode
tables and disassembly helpers — no clock ticks are run.

---

## Data Flow

When the user runs `./dis prog.hex`:

1. `main` parses `--cpu`, maps a RAM region to receive the file, then
   calls `readHexFile("prog.hex", &end)`. The parser iterates records and
   calls `MemWrite` for every data byte, returning the load address of the
   first record as `base`.

2. `main` calls `DisInit(model)`, then
   `DisRange(base, end - base + 1, readByte, printInstruction)`.
   `readByte` adapts `MemRead` to `DisReadFn`.

3. `DisRange` calls `DisOne` per instruction, which reads the opcode,
   determines its length, fetches only the operand bytes that length
   requires, and formats mnemonic and operand into a buffer. `DisRange`
   passes address, raw bytes, length and text to `printInstruction`, which
   owns all output formatting.

When run with no arguments, step 1 is replaced by mapping a hardcoded
12-byte demo program as ROM at `$0200`.

---

## Testing

`src/test_disassemble.c` runs three layers, registered with CTest
alongside the existing `memory` suite.

**Layer 1 — execution oracle.** Instruction length for all 256 opcodes
across all 5 CPU models, 1,280 cases. The expected length comes from
setting the PC, executing one instruction, and reading where the PC
landed. That path advances the PC through the addressing-mode functions,
independent of the switch the disassembler uses.

This deliberately does *not* compare against
`vrEmu6502DisassembleInstruction`. `Dis6502` was originally copied from it
verbatim, so that comparison would have passed while both shared the same
bug — which is exactly what happened with `BBR`/`BBS`.

Seven control-flow opcodes per model land somewhere other than
`addr + length` and carry hand-audited lengths instead. `JAM`/`KIL` needs
an override for the opposite reason: the emulator's `jam()` consumes an
immediate byte while its own table says 1 byte, and radare2 confirms the
table is right.

**Layer 2 — golden strings.** 31 hand-audited expected strings covering
all 15 addressing modes plus the 65C02 and W65C02 additions. Layer 1 says
nothing about format. The NMOS subset was cross-checked offline against
radare2, which is a valid reference there but *not* for 65C02 — it decodes
those as NMOS illegals and desynchronizes.

**Layer 3 — edge cases.** Truncation at every buffer size from 0 up with a
canary past the end; relative branch wrapping at both ends of the address
space; `DisRange` with zero length, straddling the end, wrapping past
`$FFFF`, and operand reads that wrap.

Both the BBR/BBS and buffer-overflow fixes were verified by reverting them
and confirming the suite fails. The canary caught the overflow that ASan
did not — the write lands past the caller's `bufSize` but inside the
test's stack array.

---

## Key Design Decisions

**Sparse memory model over flat buffer.** This was adopted on a false
premise. The README stated that vrEmu6502 allocates 64 KiB for the address
space; it does not. `vrEmu6502New` mallocs a single struct whose largest
member is `const char* mnemonicNames[256]` — roughly 1 KB on a 32-bit
target — and all memory access already went through caller-supplied
callbacks. There was no 64 KiB buffer to avoid.

The region list is kept because it is written, tested and harmless, and it
is more honest about the 6502 memory map: ROM, RAM and open-bus regions
have different write semantics. But it is host-side plumbing only. The RIA
supplies its own memory access and never links `memory.c`.

One consequence bit: `MemWrite` silently drops writes to unmapped
addresses, and `dis.c` did not map RAM before `readHexFile` wrote into it,
so HEX loading discarded every byte and disassembled files as runs of
`brk`. `dis.c` now maps a backing region first.

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

Resolved since the last revision:

- ~~`Dis6502()` is never called from `main`~~ — the two paths are
  collapsed. `dis.c` drives `DisRange`, so CLI and REPL run identical code.
- ~~The CPU model is hardcoded~~ — selectable with `--cpu`. Default stays
  `65c02`; the RP6502 needs `w65c02`.
- ~~A multi-byte instruction straddling `end` is truncated silently~~ —
  `len` now bounds where instructions may *start*. One beginning inside the
  range is decoded in full, reading up to 2 bytes past the limit. That is
  safe for RAM and ROM but would matter against memory-mapped I/O.
- ~~Label/symbol support: what format?~~ — deferred, but the old
  `labelMap` shape is ruled out: indexed directly by address, it is a
  65,536-entry pointer table, 256 KB on the Pico. Needs a sorted symbol
  array with binary search.

Still open:

- [ ] `BRK` is a 2-byte instruction — the CPU skips the signature byte,
      and `vrEmu6502.c` models this correctly in execution — but the
      disassembler reports 1, as do da65 and radare2's native plugin.
      Capstone reports 2. Current behaviour follows the majority
      convention; worth revisiting for a debugger where the listing should
      match what the CPU actually does.
- [ ] No output format options (e.g., ca65 syntax, DASM syntax).
- [ ] Function discovery and REPL subcommands (`dis function list`).

---

## Document History

| Date | Change |
|------|--------|
| 2026-06-25 | Initial document generated from codebase |
| 2026-07-28 | Callback API (`DisInit`/`DisOne`/`DisRange`), three-layer test harness, `--cpu` flag. Corrected the claim that vrEmu6502 allocates 64 KiB. Recorded the HEX-loading and upstream decode defects. |
