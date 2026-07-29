# dis — Design Document

*Last updated: 2026-07-28*

---

## Overview

`dis` is a 6502/65C02 disassembler written in C11. It reads Intel HEX
files and RP6502 ROMs (`.rp6502`), detecting the format from the file
contents rather than the extension. Its real target is a
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
- Accept an Intel HEX file or an RP6502 ROM on the host for convenience
- Separate code from data by following control flow from the entry point,
  organizing the listing by function; `--linear` falls back to a flat sweep

**Non-Goals:**
- Execution or emulation — the CPU instance exists only to drive opcode
  decoding, not to run code
- Symbol/label resolution — `labelMap` is wired up but always `NULL`
- Writing back or patching the binary

---

## Architecture

The program is a thin pipeline: load → map → decode → print. With an
entry point known, an analysis pass runs between map and decode by
default — load → map → analyze → decode → print — separating code from
data before anything is formatted; `--linear` skips it and falls back to
the plain pipeline. There is no persistent state beyond the memory
regions, the CPU instance, and (when analysis runs) the flags array.

The source files divide cleanly into four layers:

**Input** (`hexfile.c`, `rp6502file.c`) parses a program file and writes
the raw bytes into the memory model, returning the address to start from
and the last address written. `dis.c` picks a reader by sniffing the file
contents for the `#!RP6502` shebang rather than trusting the extension, so
a ROM saved under any name still loads correctly.

**Memory** (`memory.c`) is a sparse address space. Rather than
allocating a 64 KiB flat buffer (most of which would be empty for a
typical 6502 program), it maintains an array of up to 16 named regions —
ROM, RAM, or constant FILL. `MemRead` and `MemWrite` are plain C
functions passed as callbacks to the CPU emulator; the emulator never
touches the memory directly.

**Analysis** (`analyze.c`) runs between Memory and Decode when an entry
point is known and `--linear` is not given. It walks control flow over
the mapped memory and marks every byte it reaches, so the decode step
below can separate code from data and group the listing by function
instead of just marching through addresses in order.

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

**`rp6502file.c`**
Reader for RP6502 ROM files, the format PicoComputer binaries ship in. The
format is a text/binary hybrid and this parser mirrors the RIA's own
(`src/ria/mon/rom.c` in picocomputer/rp6502): a `#!RP6502` shebang matched
case-insensitively, an optional `#>len crc` group header bounding the
loadable chunk section, `#` comment lines, and `addr len crc` chunk headers
each followed by that many raw bytes. Numbers may be decimal, `0xFF` or
`$FF`.

Three decisions distinguish it from `hexfile.c`. Chunks targeting XRAM
(`$10000`-`$1FFFF`) are counted and skipped, because XRAM is not in the
6502 address space and holds data rather than code. The entry point is the
reset vector at `$FFFC`/`$FFFD` when the ROM supplies one, falling back to
the lowest loaded address; chunks lying entirely at or above `$FFFA` are
hardware vectors and are excluded from the disassembly range, so a ROM that
writes `$FFFC` does not stretch the listing to the top of memory. And CRCs
are validated: each chunk carries a CRC-32 and a mismatch is an error, as
it is on the RIA.

The CRC is standard IEEE/zlib CRC-32. The RIA's `mem_crc32` is
`~lfs_crc(~crc, ...)`, and littlefs's `lfs_crc` is the reflected
nibble-table CRC-32 with polynomial `$EDB88320`; the implementation is
pinned by known vectors including the canonical `$CBF43926` for
`"123456789"`, cross-checked against Python's `zlib.crc32`.

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

**`analyze.c`**
Recursive-descent traversal. Seeded with entry points, it follows control
flow and marks every byte reached in a flat `uint8_t flags[0x10000]`;
whatever is unreached is data. Marking an address as queued before pushing
it means each enters the worklist at most once, so the traversal terminates
on any input. Targets outside the loaded range are counted, not followed.

Depends only on `disassemble.h` — never `memory.c` or the file readers — so
it keeps the same portability shape as `disassemble.c`. Host-side for now; a
Pico port would pack the flags into an 8KB bitmap plus a small function
table.

Control-flow classification lives in `disassemble.c` as `DisFlowOf`, because
the `VrEmu6502*` handle needed for mnemonic lookup is private there. Two
classifications are easy to get wrong: `BRA` is unconditional, so nothing
falls through to the next instruction, and `BBR`/`BBS` are 3-byte
conditional branches whose displacement is the third byte.

**`vrEmu6502.c/h`**
Third-party 6502/65C02 emulator by Troy Schrapel (vrEmu6502). Linked
statically (`-DVR_EMU_6502_STATIC`). Used here purely for its opcode
tables and disassembly helpers — no clock ticks are run.

---

## Data Flow

When the user runs `./dis prog.hex`:

1. `main` parses `--cpu` and maps a RAM region to receive the file. It
   then calls `isRp6502File` to pick a reader. For Intel HEX,
   `readHexFile("prog.hex", &end)` iterates records and calls `MemWrite`
   for every data byte, returning the first record's load address as
   `base`. For a ROM, `readRp6502File` loads the RAM chunks and reports
   the entry point, the code extent, and any XRAM chunks it skipped;
   `dis.c` prints those notes to stderr so the listing on stdout stays
   pipeable.

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

**HEX loading.** `src/test_hexfile.c` covers the parser: single and
multiple records, gaps between records, out-of-order records, comment
lines, the EOF record halting the parse, a missing file, a file with no
records, and the fact that checksums are never validated.

Those are unit tests, and they map RAM themselves — so they would *not*
have caught the bug that actually shipped, which was `dis.c` failing to
map RAM before loading. That is guarded separately by the `cli_hexfile`
test, which runs the built binary against `testdata/w65c02_demo.hex` and
requires `bbr0` in the output while forbidding `brk`. If the mapping
regresses, the file loads as zeros and every line becomes `brk`. Verified
by removing the mapping: the unit tests still passed and `cli_hexfile`
failed.

**RP6502 ROMs.** `src/test_rp6502file.c` generates ROM files at run time
and covers the CRC-32 against known vectors, the minimal ROM, all three
number formats, comments, XRAM chunks being skipped and counted, an
XRAM-only ROM being an error, the reset vector as entry point, a vector
pointing outside loaded code falling back, the `#>` group header bounding
the chunk section so a trailing named asset is not parsed as a chunk, CRC
mismatch, missing and lowercase shebangs, truncated payloads, address and
length validation, malformed headers, and content-based format detection.

`cli_rp6502` is the end-to-end guard. Its fixture's reset vector points at
`$0205` rather than the `$0200` load address, so requiring the listing to
begin at `0205` proves the entry point came from the vector and not from
the first chunk. The fixture also carries an XRAM chunk that must be
reported as skipped rather than disassembled.

Regenerate `testdata/w65c02_demo.rp6502` with:

```python
import zlib
def crc(b): return zlib.crc32(b) & 0xFFFFFFFF
code   = bytes([0x0f,0x12,0x05, 0x07,0x10, 0x1a, 0x80,0x05, 0xb2,0x10])
vector = bytes([0x05, 0x02])   # $0205
xram   = bytes(range(16))
with open("testdata/w65c02_demo.rp6502", "wb") as f:
    f.write(b"#!RP6502\n# see docs/design.md to regenerate\n")
    f.write(("%d %d %d\n" % (0x0200, len(code),   crc(code))).encode());   f.write(code)
    f.write(("%d %d %d\n" % (0x10000, len(xram),  crc(xram))).encode());   f.write(xram)
    f.write(("%d %d %d\n" % (0xFFFC, len(vector), crc(vector))).encode()); f.write(vector)
```

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

No environment variables or config files. The CPU model defaults to
`CPU_65C02` in `dis.c` but is selectable with `--cpu`. Build requires
CMake ≥ 3.16 and a C11 compiler.
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

- ~~`BRK` length~~ — resolved as 2 bytes, rendering the signature byte as
  an operand (`brk $ea`). The CPU pushes `addr+2` and skips that byte;
  `brk()` in `vrEmu6502.c` models this correctly in execution while the
  opcode table declares implied, i.e. length 1.

  This deliberately diverges from da65 and radare2's native plugin, which
  report 1 (Capstone reports 2). The deciding argument is that reporting 1
  does not merely display the signature byte oddly — it *decodes* it. Given
  `00 01 a9 42 a5 10`, length 1 produces `brk` / `ora ($a9, x)` /
  `ldd #$a5` / `bpl $0207`: both real instructions are lost and the listing
  never resynchronizes. A signature of `$ea` decodes as a harmless `nop`,
  which is why the built-in demo hid this.

  Cost: output no longer matches da65/r2 line-for-line on `BRK`, and
  zero-filled regions render as half as many lines. Both are acceptable
  against a debugger listing that stays in step with the CPU.

- ~~Function discovery~~ — `analyze.c`'s recursive-descent traversal finds
  function entries from `JSR` targets and organizes the listing by
  function, on by default; `--linear` gives the old flat sweep. See the
  `analyze.c` entry under Components.

Still open:

- [ ] No output format options (e.g., ca65 syntax, DASM syntax).
- [ ] REPL subcommands (`dis function list` as an interactive PicoComputer
  command, distinct from the discovery itself, which is resolved above).

---

## Document History

| Date | Change |
|------|--------|
| 2026-06-25 | Initial document generated from codebase |
| 2026-07-28 | RP6502 ROM (`.rp6502`) support: content-based format detection, XRAM chunks skipped, reset vector as entry point, CRC-32 validation. |
| 2026-07-28 | Callback API (`DisInit`/`DisOne`/`DisRange`), three-layer test harness, `--cpu` flag. Corrected the claim that vrEmu6502 allocates 64 KiB. Recorded the HEX-loading and upstream decode defects. |
| 2026-07-29 | Recursive-descent code/data analysis, function discovery, function-organized listing, `--linear`. Real ROM fixtures vendored from picocomputer/rp6502. |
