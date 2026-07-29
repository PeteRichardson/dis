# dis
## Using the vrEMU6502 emulator as just a static disassembler.

### Current Status

Working. The goal is a `dis` command in the PicoComputer's built-in REPL on
the pi pico RIA: give it an address and a length, get disassembly on stdout.
This repo is the Mac-side proving ground for the code that ships there.

The shippable core is three functions in `src/disassemble.c`:

```c
void     DisInit(vrEmu6502Model model);
uint16_t DisOne(uint16_t addr, DisReadFn read, int bufSize, char *buf);
void     DisRange(uint16_t addr, uint16_t len, DisReadFn read, DisEmitFn emit);
```

Memory is reached through a byte-fetch callback, so the RIA supplies its own
access routines and none of the host-side scaffolding goes with it.
`disassemble.c/h` plus `vrEmu6502.c/h` is the whole file set to copy over.

```sh
./build/bin/dis                          # built-in demo program
./build/bin/dis prog.hex                 # disassemble an Intel HEX file
./build/bin/dis game.rp6502              # ...or an RP6502 ROM
./build/bin/dis --cpu w65c02 prog.hex    # the RP6502's actual CPU
./build/bin/dis --linear game.rp6502     # flat sweep, no control-flow analysis
ctest --test-dir build                   # 1300+ assertions
```

The input format is detected from the file contents, not the extension.

### RP6502 ROMs

PicoComputer binaries ship as `.rp6502` files, a text/binary hybrid mirrored
from the RIA's own parser (`src/ria/mon/rom.c` in
[picocomputer/rp6502](https://github.com/picocomputer/rp6502)):

```
#!RP6502                 shebang, matched case-insensitively
#>len crc                optional group header bounding the chunk section
# comment                lines starting with # are skipped
addr len crc             chunk header; numbers may be decimal, 0xFF or $FF
<len raw binary bytes>   payload, immediately after the newline
```

Three behaviours worth knowing:

- **XRAM chunks are skipped.** Addresses `$10000`–`$1FFFF` are XRAM, which
  isn't in the 6502 address space and holds data rather than code. They're
  counted and reported on stderr, never disassembled.
- **The reset vector is the entry point.** If the ROM writes `$FFFC`/`$FFFD`,
  disassembly starts there — that's where the CPU begins, and the RIA only
  treats a ROM as runnable when both bytes are present. Otherwise it starts at
  the lowest loaded address. A vector pointing outside the loaded code falls
  back rather than disassembling unmapped fill.
- **CRCs are validated.** Each chunk carries a CRC-32 and a mismatch is an
  error, matching the RIA. This is deliberately stricter than the Intel HEX
  reader, which ignores its checksums.

Notes go to stderr, so the listing on stdout stays pipeable.

### Code vs data

A ROM is a memory image, not a code section, so a flat sweep decodes ASCII and
tables as instructions. `dis` follows control flow from the entry point instead,
marking what it reaches as code. Anything unreached is shown as a data marker:

    sub_0205:                  ; entry point
    0205: 1a        inc a
    0206: 20 30 02  jsr $0230

    ; 0210-022f  32 bytes data

Function entries come from `JSR` targets, so they carry a caller count. Branch
targets get `loc_XXXX` labels.

Three things this cannot see, inherent to the technique on 6502: `JSR` followed
by inline data (decoded as code), jump tables and the `RTS` dispatch trick (real
code that looks unreachable), and self-modifying code. Unreached bytes are shown
rather than dropped so you can spot the disagreement, and `--linear` always
gives the unfiltered sweep.

No `.rp6502` carries symbols, so labels are synthetic. Real names need a build
artifact — see issue #5.

### Two things I had wrong

**vrEMU6502 does not allocate 64K.** I assumed it did, and that assumption is
why this repo has a sparse memory manager. `vrEmu6502New` mallocs a single
struct whose largest member is `const char* mnemonicNames[256]` — about 1KB on
the Pico. All memory access already went through caller-supplied callbacks.
The sparse model in `memory.c` was solving a problem that didn't exist. It
survives as host-side plumbing for the CLI; it never ships to the RIA.

**Loading Intel HEX files had never worked.** `readHexFile` writes through
`MemWrite`, which silently drops writes to unmapped addresses, and nothing
mapped RAM first — so every loaded byte was discarded and any file
disassembled as a run of `brk`s. Fixed in `dis.c`.

### Bugs the test harness found

The disassembler is checked three ways: instruction length for all 256 opcodes
across all 5 CPU models, against an oracle that executes each instruction and
sees where the PC lands; a table of hand-audited expected strings; and edge
cases around truncation and address wrapping. See
`docs/specs/2026-07-28-disassembler-test-harness-design.md`.

That turned up five real defects, three of them upstream in vrEMU6502:

- `BBR0-7`/`BBS0-7` are 3-byte `zp,rel` instructions, but the opcode tables
  declare them `{bbr0, zp, 5}` — length 2, which desynchronizes every
  instruction after one. These are W65C02 instructions, so this hit the exact
  CPU the RP6502 uses.
- Accumulator mode was never rendered. `opcodeToAddrMode` has no case for the
  `acc` addressing function, so those opcodes fall through to `AddrModeImp`
  and `AddrModeAcc` is returned for nothing at all. `inc a` printed as `inc`.
- `JAM`/`KIL` advances the PC by 2 during execution while the opcode table
  says 1 byte. radare2 confirms 1; the execution path is wrong.
- Composing output by advancing a cursor by `snprintf`'s return value overran
  the buffer, since that return is what it *would* have written. Present in
  vrEMU6502's own disassembler too.
- Implied-mode instructions carried a trailing space.
- `BRK` was reported as 1 byte. The CPU pushes `addr+2`, skipping the
  signature byte after the opcode, so reporting 1 *decodes* that byte as an
  instruction. `00 01 a9 42 a5 10` came out as `brk` / `ora ($a9, x)` /
  `ldd #$a5` / `bpl $0207` — both real instructions lost, no resynchronization.
  Now 2 bytes, rendered `brk $01`. This diverges from da65 and radare2 on
  purpose; see `docs/design.md`.

### What's next

Function discovery (`dis function list`), symbol support, and REPL
integration. Symbol support needs a sorted symbol table with binary search —
the sketch that used to be in `disassemble.c` indexed a 65,536-entry pointer
array by address, which is 256KB on the Pico.
