# Recursive-Descent Analysis: Code/Data Separation and Function Discovery — Design

*Date: 2026-07-29*
*Status: approved, not yet implemented*

---

## Problem

Disassembling a `.rp6502` ROM works, but the output is mostly noise. A ROM is a
memory image, not a code section: `adventure.rp6502` carries 49KB across 49
chunks, much of it ASCII text and tables. A linear sweep decodes every byte as
an instruction, so the listing is dominated by garbage, and a multi-byte
"instruction" decoded out of a string desynchronizes the bytes that follow it.

Three related things are missing: a way to tell code from data, a way to know
where functions begin, and a way to know where they end.

---

## Approach

One algorithm answers all three: **recursive-descent disassembly**, also called
control-flow traversal. Seed a worklist with known entry points, follow control
flow, and mark every byte reached. What is never reached is data.

Traversal rules:

| Instruction | Action |
|---|---|
| `JSR $nnnn` | record `$nnnn` as a function entry, queue it, continue |
| `JMP $nnnn` | queue target, stop this trace |
| `Bxx $nnnn` (branches) | queue target, continue — the branch falls through |
| `RTS`, `RTI`, `BRK` | stop this trace — this is the end of a function |
| `JMP ($nnnn)`, `JMP ($nnnn,x)` | stop; an indirect target cannot be followed |
| anything else | continue to the next instruction |

Entry points are the 6502 hardware vectors: RESET `$FFFC`, NMI `$FFFA`, IRQ
`$FFFE`. `readRp6502File` already extracts the reset vector, and all eight real
PicoComputer ROMs examined supply one.

### Why not the alternatives

A **linear sweep with heuristics** (runs of `$00`, printable-ASCII detection)
needs no control-flow analysis but is guesswork, and it cannot produce function
boundaries at all. Worth adding later as a supplement for regions the traversal
never reaches, not as the primary mechanism.

**Following jump tables** would recover the largest class of code the traversal
misses, but table extent is unknowable in general. It needs a guessed length and
is more likely to introduce wrong code than to help. Explicitly out of scope.

---

## Known blind spots

Stated up front because they are inherent to the technique on 6502, not defects
to be fixed later:

- **`JSR` with inline data.** A common 6502 idiom: `JSR print` followed
  immediately by inline ASCII, with the callee popping the return address to
  find it. The traversal walks into the string and decodes it as code.
  Undetectable without knowing the callee's behaviour.
- **Jump tables and the `RTS` dispatch trick** (push address−1, `RTS`). Targets
  are invisible, so real code looks unreachable and is reported as data.
- **Self-modifying code**, common in tight 6502 loops.

Mitigation is honesty rather than cleverness: unreached bytes are *shown* as a
data marker rather than dropped, and `--linear` always gives the unfiltered
view. The user can always see that the analysis disagreed with them.

---

## Design

### Module

New `src/analyze.c/h`, depending only on `DisOne` and a `DisReadFn` — no
`memory.c`, no file readers. This is the same portability shape as
`disassemble.c`.

The analysis is host-side for now. The interface is callback-driven so that
porting to the RIA later is mechanical rather than a rewrite.

```c
typedef struct DisAnalysis DisAnalysis;

DisAnalysis* AnalyzeNew(DisReadFn read, uint16_t lo, uint16_t hi);
void         AnalyzeAddEntry(DisAnalysis*, uint16_t addr);
void         AnalyzeRun(DisAnalysis*);
void         AnalyzeFree(DisAnalysis*);

bool     AnalyzeIsInstruction(const DisAnalysis*, uint16_t addr);
bool     AnalyzeIsFunction(const DisAnalysis*, uint16_t addr);
bool     AnalyzeIsLabel(const DisAnalysis*, uint16_t addr);
unsigned AnalyzeCallers(const DisAnalysis*, uint16_t addr);
```

Internally a `uint8_t flags[0x10000]`, one byte per address, carrying
`INSTRUCTION_START`, `FUNCTION_START`, `LABEL` and `QUEUED`. 64KB is free on the
host; the header records that a Pico port packs this into an 8KB bitmap plus a
small function table.

`lo`/`hi` bound the traversal to loaded memory so a wild target cannot walk off
into unmapped fill.

### Termination

An address is marked `QUEUED` before being pushed, so each enters the worklist
at most once and the traversal always terminates — including on `bra` to itself.
Targets outside `[lo,hi]` are recorded as an out-of-range diagnostic and not
followed.

### Two label kinds

`JSR` targets become `sub_0205:` — those are functions. Branch and `JMP` targets
become `loc_0210:` — those are just places. The distinction is what makes the
listing readable, and it is what caller counts and any future call graph hang
off.

### Output

The function-organized listing is the **default** whenever an entry point is
known. `--linear` selects today's flat output. A bare address range has no entry
point to seed from, so the REPL's eventual `dis <addr> <len>` stays linear.

Unreached regions collapse to a one-line marker rather than a hex dump: the
listing stays short, the code/data split is obvious, and the extent is still
visible.

```
sub_0205:                      ; entry point (reset vector)
0205: 1a        inc a
0206: 80 05     bra $020d
loc_0208:
0208: b2 10     lda ($10)
020a: 60        rts

sub_020b:                      ; 3 callers
020b: a9 42     lda #$42
020d: 60        rts

; 0210-022f  32 bytes data
```

### Out of scope for this increment

- **Manual `--data` / `--code` overrides.** The standard fix for the blind spots
  above, and what da65's info files do — but speculative until the plain
  traversal has run against real ROMs. Add once it is known which override is
  actually wanted.
- **Symbol support** — issue #5, the agreed next increment. No `.rp6502` carries
  symbols, so real names must come from a build artifact.
- **Call graph / DOT output.** The `JSR` edges are recorded, so the graph is a
  later formatting exercise rather than new analysis. Caller counts deliver most
  of its practical value first.

---

## Testing

### Synthetic programs

Hand-written images with known structure, in the style of the existing suites:

- a function called from two sites → caller count is 2
- a deliberately unreachable block → reported as data
- a `JMP` ending a trace → bytes after it, if unreferenced, are data
- `bra` to itself → traversal terminates
- a branch target outside `[lo,hi]` → reported, not followed
- `JSR` with inline data → **documents the wrong-but-expected result**, so a
  future fix has a test to flip

### Real ROMs

Two fixtures vendored from [picocomputer/rp6502](https://github.com/picocomputer/rp6502)
(`tests/roms/`), BSD 3-Clause, with the copyright notice retained in
`testdata/`:

- **`rtc.rp6502`** (1.7KB, 3 chunks) — the routine regression fixture, fast
  enough to run on every `ctest`
- **`adventure.rp6502`** (97KB, 49 chunks, 4 named assets) — the realistic
  analysis subject, with large ASCII blobs. Also the first real exercise of the
  `#>len crc name` named-asset boundary, which today only synthetic tests cover.

Assertions against real ROMs must be structural, not exact-output. Exact
listings would be brittle and would encode current behaviour as if it were
correct. Specifically:

- disassembly starts at the reset vector
- both classifications are non-empty: some bytes are code, some are data. A run
  that classifies everything one way means the traversal is broken, not that the
  ROM is unusual
- no chunk is decoded past its end
- at least one function beyond the entry point is discovered

A *measured* data-fraction threshold is deliberately not specified here. Picking
a number before running the analysis would be inventing it. Once implemented,
record the observed figure for `adventure.rp6502` in a comment next to the
assertion, with a tolerance band, so a future regression that halves it is
visible.

### Facts established while designing this

Checked against all eight real ROMs in `picocomputer/rp6502` `tests/roms/`:

- all eight supply a RESET vector at `$FFFC`, so seeding from it is sound
- **none** contain XRAM chunks; the XRAM-skipping path added in the previous
  increment is exercised only by synthetic tests. These programs appear to
  populate XRAM at run time through the API instead.
- real ROMs use `$`-prefixed hex in chunk headers, confirming that supporting
  all three number forms was necessary
- `tests/roms/cc65.dbg` is **not** Adventure's debug info. It describes a
  4-symbol toy (`main.c`, `_main` at `$0239`, 48 bytes). It is useful as a
  format sample for issue #5, not as ground truth for any ROM here.

---

## Verification

```sh
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure

./build/bin/dis testdata/adventure.rp6502            # analyzed by default
./build/bin/dis --linear testdata/adventure.rp6502   # old flat output
./build/bin/dis testdata/rtc.rp6502

cmake -B build-asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

Success criteria: the analyzed listing of `adventure.rp6502` classifies a
substantial fraction of its 49KB as data rather than instructions, with the
measured figure recorded rather than assumed; discovered function entry points
are plausible on inspection; `--linear` output is byte-identical to today's;
sanitizers clean.

The honest measure is the first one — if the traversal does not visibly cut the
noise on a real ROM, the approach has not earned its place.
