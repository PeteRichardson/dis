# Disassembler API Reshape + Test Harness — Design

*Date: 2026-07-28*
*Status: implemented. Layer 1 was redesigned mid-implementation — see "The replacement
oracle" — and two further upstream defects plus one CLI defect were found; both are
recorded below.*

---

## Problem

The goal is a `dis <addr> <len>` command in the PicoComputer REPL (RP6502, W65C02S RIA)
that dumps disassembly to stdout. This repository is the Mac-side proving ground for the
code that will ship to the RIA.

Two things block that today:

1. **Nothing tests the disassembler.** `src/test_memory.c` covers the memory model; the
   decode and formatting logic has no coverage at all.
2. **The function intended to ship is unreachable and mis-shaped.** `Dis6502()` in
   `src/disassemble.c` was reaching for the right idea — format one instruction from
   bytes, without going through emulator memory — but nothing calls it (`dis.c` uses
   `vrEmu6502DisassembleInstruction` instead), roughly half of it is dead code, and its
   "caller pre-fetches three bytes" contract is awkward at a range boundary: you cannot
   fetch three bytes for a one-byte instruction at the end of a range without reading
   past it.

---

## Premises corrected during design

Two assumptions this repository was built on turned out to be wrong. Both are recorded
here because they change what the right design is.

### The emulator does not allocate 64 KiB

`vrEmu6502New` (`src/vrEmu6502.c:207`) `malloc`s a single struct. Its largest member is
`const char* mnemonicNames[256]` — about 1 KB on the Pico's 32-bit pointers. All memory
access is external, through the `vrEmu6502MemRead`/`vrEmu6502MemWrite` callbacks the
caller supplies.

The README states that "the emulator allocates 64K for the 6502 address space," and that
claim motivated the sparse memory model in `memory.c`. It was solving a problem that did
not exist.

Consequence: a CPU handle is cheap enough to keep on the RIA, so the disassembler can
depend on one. `memory.c` remains useful as Mac-side CLI plumbing but does not ship.

### radare2 cannot serve as a 65C02 reference

The original plan was to differential-test against radare2. Verified against r2 6.1.8:

| Input | `rasm2 -a 6502` | `rasm2 -a 6502.cs` |
|---|---|---|
| NMOS bytes `00 ea a9 42 a5 10 ad 34 12 4c 00 20` | correct | correct, but decodes `BRK` as 2 bytes |
| 65C02 bytes (`BRA`, `PHX`, `STZ`, `TRB`, `LDA (zp)`, `INC A`, `BBR0`) | garbage — `nop #0x05`, `ora 0xda`, `slo`, `hlt`; stream desynchronizes | `invalid` |

Neither plugin decodes 65C02, which is precisely the variant that ships. r2 is therefore
useful only as a one-time offline cross-check of the NMOS subset, not as a test-time
oracle.

**The replacement oracle.** The first attempt was to differential-test against the
library's own `vrEmu6502DisassembleInstruction`. That was abandoned during
implementation: `Dis6502` turns out to be a near-verbatim copy of it, sharing the same
15 cases and the same `snprintf` defect, so the comparison would have passed trivially
while both reported the same wrong length for `BBR`/`BBS`.

The oracle used instead is the emulator's *execution* path — set the PC, run one
instruction, read where the PC landed. That advances the PC through the addressing-mode
functions (`ab`, `abx`, `zp`, ...), genuinely independent of the disassembly switch.

---

## Design

### API

`src/disassemble.h`:

```c
typedef uint8_t (*DisReadFn)(uint16_t addr);
typedef void    (*DisEmitFn)(uint16_t addr, const uint8_t *bytes,
                             uint8_t len, const char *text);

void     DisInit(vrEmu6502Model model);
uint16_t DisOne(uint16_t addr, DisReadFn read, int bufSize, char *buf);
void     DisRange(uint16_t addr, uint16_t len, DisReadFn read, DisEmitFn emit);
```

A **read callback** replaces the pre-fetched-bytes contract, so `DisOne` fetches exactly
as many bytes as the opcode needs and range boundaries stop being a special case.

`DisRange` takes an **emit callback rather than printing**. Tests capture output directly
without stdout redirection, and the REPL controls its own formatting.

Neither callback carries a `void *ctx`. The RIA's memory access is global and `memory.c`
is already global-state; context pointers would be YAGNI against the existing style.

`DisInit` exists because mnemonic and addressing-mode lookup are pure functions of
`(model, opcode)`, but the library exposes them only through a `VrEmu6502*`
(`vrEmu6502OpcodeToMnemonicStr`, `vrEmu6502GetOpcodeAddrMode`). The module holds one
static handle. Stub read/write functions are passed to `vrEmu6502New`, which asserts they
are non-NULL but never calls them during disassembly.

### What ships to the RIA

`disassemble.c`, `disassemble.h`, `vrEmu6502.c`, `vrEmu6502.h` — that is the entire set.
`memory.c` and `hexfile.c` stay Mac-side.

### Behavioral decisions

| Question | Decision | Rationale |
|---|---|---|
| Instruction straddles end of range (`dis $1FFF 1` where a 3-byte `JMP` starts at `$1FFF`) | Decode it fully, reading past the limit | `len` means "where to stop *starting* instructions." Matches how most debuggers behave. Noted risk: reads slightly past the window, which is harmless for RAM/ROM but relevant if ever pointed at memory-mapped I/O |
| Buffer too small | Truncate cleanly, always NUL-terminate, still return the true next address | The listing keeps scrolling even if one line clips. Keeps return value `0` meaning exactly one thing: undecodable |
| `labelMap` / `refAddr` | Delete | Currently locals hardcoded to `NULL`, so every branch guarded by them is unreachable. Sketch preserved in git at commit `1181eab` |

On deleting the label code: it is indexed directly by address (`labelMap[arg16]`), making
it a 65,536-entry pointer table — 256 KB on the Pico, more RAM than the RP6502 gives a
whole program. Real symbol support will need a sorted symbol array with binary search or
a small hash, so reviving this would be a redesign rather than a restore. Deleting now is
the cheaper path, not merely the tidier one.

---

## Known defects to fix

Found by reading, not by guessing. All four are in the code being rewritten.

1. **Buffer overflow on truncation** — `src/disassemble.c:20-22`. `offset` is `snprintf`'s
   *would-have-written* return value. On truncation, `buffer += offset` walks past the end
   and `bufferSize -= offset` goes negative, then is passed as a `size_t`.
2. **Wrong parameter type** — `src/disassemble.h:10`. `uint16_t byte2` should be `uint8_t`.
3. **`AddrModeAcc` emits no operand** — `src/disassemble.c:129-131`. `INC A` renders as
   `"INC "`. Implied-mode instructions also inherit a trailing space from line 20.
4. **`BBR`/`BBS` have no representation** — `vrEmu6502AddrMode` (`src/vrEmu6502.h:103-120`)
   has 15 modes, none carrying both a zero-page byte and a relative byte. These W65C02 and
   R65C02 instructions are 3 bytes (`$0F,$1F..$7F` and `$8F,$9F..$FF`). Since the RP6502's
   RIA is a W65C02S, this affects the actual target. `RMB`/`SMB` are 2-byte zero-page and
   should map to `AddrModeZP` correctly.

Defect 4 was a hypothesis from reading the enum. Confirmed: the tables declare
`{bbr0, zp, 5}` (`src/vrEmu6502.c:1958`), and disabling the fix produces 64 test
failures across W65C02 and R65C02.

Two further defects were found during implementation, both upstream in vrEmu6502:

5. **Accumulator mode is never reported.** `opcodeToAddrMode` (`src/vrEmu6502.c:2006`)
   has no case for the `acc` addressing function, so those opcodes fall through to its
   closing `return AddrModeImp` and `AddrModeAcc` is returned for nothing at all. The
   six affected opcodes are identified directly in `disassemble.c`, gated on mnemonic
   because `$1a`/`$3a` are NOPs on NMOS and only `INC A`/`DEC A` on CMOS.
6. **`JAM` execution advances the PC by 2** while its table says 1 — see Layer 1 below.

And one outside the disassembler, in the CLI:

7. **Intel HEX loading had never worked.** `readHexFile` writes through `MemWrite`,
   which silently drops writes to unmapped addresses (`src/memory.c:150`), and `dis.c`
   never mapped RAM first — so every loaded byte was discarded and any file disassembled
   as a run of `brk`s. Confirmed pre-existing by running commit `1181eab` against the
   same file. Fixed in `dis.c`; `hexfile.c` remains untouched.

---

## Test strategy

Three layers, cheapest and broadest first, in `src/test_disassemble.c`. Plain C with
`assert()`, matching `src/test_memory.c`, registered via `add_test` in
`src/CMakeLists.txt`.

### Layer 1 — execution oracle

For each of `CPU_6502`, `CPU_6502U`, `CPU_65C02`, `CPU_W65C02`, `CPU_R65C02` × all 256
opcodes, assert `DisInstLen` and `DisOne` agree with where the PC lands after
`vrEmu6502InstCycle` executes that opcode. 1,280 cases, milliseconds, no external tools.

Length is the invariant that matters most: a wrong length desynchronizes every
instruction after it.

Operand bytes are always `$00`, so a taken branch and an untaken branch land on the same
address and the measurement is unambiguous.

**Opcodes the oracle cannot measure.** Seven control-flow opcodes per model (`BRK`,
`JSR`, `RTI`, `JMP` abs/ind/indx, `RTS`) leave the PC somewhere other than
`addr + length`; these carry hand-audited lengths, matched on both opcode *and* mnemonic
so that `$7c` is only treated as `JMP (abs,x)` on the CMOS parts where it is one.

`JAM`/`KIL` needs an override for the opposite reason: it is the *emulator* that is
wrong. Its `jam()` calls `imm()`, consuming a byte and leaving the PC at `addr+2`, while
its own table declares `{jam, imp, 1}`. radare2 decodes `02 ea a9 42` as `hlt / nop /
lda #$42`, confirming 1 byte.

**Stated limitation:** the oracle shares the opcode *tables* with the disassembler, so a
wrong table entry that both paths read identically would still be invisible. It catches
disagreements between decoding and execution. Layers 2 and 3 cover the rest.

### Layer 2 — hand-audited golden strings

A curated table of `{model, bytes, expected_text, expected_len}` covering all 15
addressing modes plus the 65C02/W65C02 additions. This pins output *format*, about which
Layer 1 says nothing — it cannot distinguish `LDA $1234, x` from `lda $1234,X`.

The NMOS subset is cross-checked once, offline, with `rasm2 -a 6502 -b 8 -d <hex>`, then
hand-audited and committed as a C table. The regeneration command is recorded in a
comment. No test-time dependency on radare2.

### Layer 3 — edge cases

- Buffer truncation at every size from 1 to full, with a guard canary past the buffer end
- Relative branches forward and backward, including wrap at `$0000` and `$FFFF`
- `DisRange` with `len == 0`, and with `addr + len` wrapping past `$FFFF`
- Instruction straddling the end of the range

Verified under ASan/UBSan, since a guard canary alone may not catch every overrun.

---

## Out of scope

Deferred to their own spec → plan → implementation cycles:

- **Function discovery** — identifying function addresses and lengths in a binary
- **CLI subcommands** — `dis function list <binary>`, `dis -f main <binary>`
- **Symbol/label support** — see the note on `labelMap` above
- **Intel HEX or binary file loading changes** — `hexfile.c` is untouched here

---

## Verification

```sh
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/dis                  # demo output byte-identical to pre-change baseline
./build/bin/dis --cpu w65c02     # W65C02 path exercised

cmake -B build-asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

Success criteria: 1,280 length assertions pass, the golden table matches exactly, ASan
reports clean, and the built-in demo output is unchanged from the baseline captured
before any edits.
