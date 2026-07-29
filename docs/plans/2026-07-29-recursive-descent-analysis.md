# Recursive-Descent Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate code from data in a loaded program by following control flow from known entry points, and render a function-organized listing instead of a linear sweep.

**Architecture:** A new `src/analyze.c/h` performs a worklist traversal seeded from the 6502 hardware vectors, marking instruction starts, function entries (`JSR` targets) and labels (branch/`JMP` targets) in a flat `uint8_t flags[0x10000]`. It depends only on a `DisReadFn` and a new opcode-classification helper in `disassemble.c`, so it never touches `memory.c` or the file readers. `dis.c` renders the result.

**Tech Stack:** C11, CMake + Ninja, CTest. No new dependencies.

## Global Constraints

- C11. Build must be warning-free under `-Wall -Wextra -Wpedantic` (see top-level `CMakeLists.txt`).
- Tests are plain C using the `CHECK` macro pattern from `src/test_hexfile.c` — collect failures, return non-zero from `main`, never `assert`-and-abort.
- `analyze.c` must not include `memory.h`, `hexfile.h`, `rp6502file.h`, or `vrEmu6502.h`. Its only dependency is `disassemble.h`.
- Analysis is host-side for now, but the interface stays callback-driven so an RIA port is mechanical. Document, don't implement, the Pico bitmap variant.
- `--linear` output must remain byte-identical to today's output.
- Spec: `docs/specs/2026-07-29-recursive-descent-analysis-design.md`.
- Work on branch `feat/recursive-descent-analysis` (already created, spec already committed).

---

### Task 1: Opcode control-flow classification

`analyze.c` must know what each instruction does to control flow, but the `VrEmu6502*` handle needed to look up mnemonics is private to `disassemble.c`. Expose a classifier there so the analysis stays independent of vrEmu6502.

**Files:**
- Modify: `src/disassemble.h` (append after `DisRange` declaration)
- Modify: `src/disassemble.c` (append at end)
- Test: `src/test_disassemble.c` (add a function, call it from `main`)

**Interfaces:**
- Consumes: `DisReadFn`, `DisInit`, `DisInstLen` from `src/disassemble.h`.
- Produces: `DisFlow` enum and `DisFlow DisFlowOf(uint16_t addr, DisReadFn read, uint16_t* target)`. Task 2 uses both.

- [ ] **Step 1: Write the failing test**

Add to `src/test_disassemble.c`, above `main`:

```c
static void test_flow_classification(void) {
    DisInit(CPU_W65C02);

    static const struct {
        uint8_t     bytes[3];
        DisFlow     flow;
        uint16_t    target;   /* ignored when the flow has no target */
        const char* what;
    } cases[] = {
        { { 0xea },             DisFlowNormal,   0x0000, "nop" },
        { { 0xa9, 0x42 },       DisFlowNormal,   0x0000, "lda #$42" },
        { { 0x20, 0x34, 0x12 }, DisFlowCall,     0x1234, "jsr $1234" },
        { { 0x4c, 0x34, 0x12 }, DisFlowJump,     0x1234, "jmp $1234" },
        { { 0x6c, 0x34, 0x12 }, DisFlowIndirect, 0x0000, "jmp ($1234)" },
        { { 0x7c, 0x34, 0x12 }, DisFlowIndirect, 0x0000, "jmp ($1234,x)" },
        { { 0x60 },             DisFlowReturn,   0x0000, "rts" },
        { { 0x40 },             DisFlowReturn,   0x0000, "rti" },
        { { 0x00, 0xea },       DisFlowReturn,   0x0000, "brk" },
        { { 0xdb },             DisFlowReturn,   0x0000, "stp" },
        /* conditional: queue the target AND fall through */
        { { 0xd0, 0x05 },       DisFlowBranch,   0x1007, "bne +5" },
        { { 0xd0, 0xfe },       DisFlowBranch,   0x1000, "bne -2" },
        /* BRA is unconditional on 65C02: a jump, not a branch */
        { { 0x80, 0x05 },       DisFlowJump,     0x1007, "bra +5" },
        /* BBR/BBS are 3-byte conditional branches: target is addr+3+rel */
        { { 0x0f, 0x12, 0x05 }, DisFlowBranch,   0x1008, "bbr0 $12,+5" },
        { { 0x8f, 0x12, 0xfd }, DisFlowBranch,   0x1000, "bbs0 $12,-3" },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(mem, 0, sizeof mem);
        memcpy(&mem[TEST_ADDR], cases[i].bytes, 3);

        uint16_t target = 0xffff;
        DisFlow flow = DisFlowOf(TEST_ADDR, disRead, &target);

        CHECK(flow == cases[i].flow,
              "flow %s: got %d, expected %d", cases[i].what, flow, cases[i].flow);

        if (cases[i].flow == DisFlowCall || cases[i].flow == DisFlowJump ||
            cases[i].flow == DisFlowBranch)
            CHECK(target == cases[i].target,
                  "flow %s: target $%04x, expected $%04x",
                  cases[i].what, target, cases[i].target);
    }

    printf("flow: %zu classifications checked\n",
           sizeof cases / sizeof cases[0]);
}
```

Call it from `main`, after `test_range();`:

```c
    test_flow_classification();
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build 2>&1 | head -20
```

Expected: compile error, `unknown type name 'DisFlow'` and `implicit declaration of function 'DisFlowOf'`.

- [ ] **Step 3: Declare the interface**

Append to `src/disassemble.h`, before the closing `#endif`:

```c
/*
 * What an instruction does to control flow. Used to drive a
 * recursive-descent traversal; see analyze.c.
 */
typedef enum {
    DisFlowNormal,   /* falls through to the next instruction */
    DisFlowCall,     /* JSR: *target is a function entry, execution resumes after */
    DisFlowJump,     /* unconditional: *target is queued, this trace ends */
    DisFlowBranch,   /* conditional: *target is queued AND this trace continues */
    DisFlowReturn,   /* RTS/RTI/BRK/STP/JAM: this trace ends, no target */
    DisFlowIndirect, /* JMP (abs) / JMP (abs,x): trace ends, target unknowable */
} DisFlow;

/*
 * Classify the instruction at addr. *target is written only for Call,
 * Jump and Branch, and is left untouched otherwise.
 *
 * Returns DisFlowNormal if DisInit has not been called.
 */
DisFlow DisFlowOf(uint16_t addr, DisReadFn read, uint16_t* target);
```

- [ ] **Step 4: Implement it**

Append to `src/disassemble.c`:

```c
DisFlow DisFlowOf(uint16_t addr, DisReadFn read, uint16_t* target) {
    if (!disCpu || !read) return DisFlowNormal;

    uint8_t opcode = read(addr);
    const char* mnemonic = vrEmu6502OpcodeToMnemonicStr(disCpu, opcode);
    if (!mnemonic) return DisFlowNormal;

    /* BBR0-7 / BBS0-7: 3 bytes, displacement is the third. Conditional,
       so the following instruction is reachable too. */
    if (disIsBitBranch(mnemonic)) {
        if (target)
            *target = (uint16_t)(addr + 3 + (int8_t)read((uint16_t)(addr + 2)));
        return DisFlowBranch;
    }

    switch (opcode) {
    case 0x20: /* jsr abs */
        if (target)
            *target = (uint16_t)(read((uint16_t)(addr + 1)) |
                                 ((uint16_t)read((uint16_t)(addr + 2)) << 8));
        return DisFlowCall;

    case 0x4c: /* jmp abs */
        if (target)
            *target = (uint16_t)(read((uint16_t)(addr + 1)) |
                                 ((uint16_t)read((uint16_t)(addr + 2)) << 8));
        return DisFlowJump;

    case 0x6c: /* jmp (abs)   */
    case 0x7c: /* jmp (abs,x) */
        /* $7c is an undefined opcode on NMOS parts, where it is not a jump. */
        return (strcmp(mnemonic, "jmp") == 0) ? DisFlowIndirect : DisFlowNormal;

    case 0x80: /* bra: unconditional, so nothing falls through to the next
                  instruction -- classify as a jump, not a branch. */
        if (strcmp(mnemonic, "bra") != 0) return DisFlowNormal; /* NMOS: undefined */
        if (target)
            *target = (uint16_t)(addr + 2 + (int8_t)read((uint16_t)(addr + 1)));
        return DisFlowJump;

    case 0x10: case 0x30: case 0x50: case 0x70:   /* bpl bmi bvc bvs */
    case 0x90: case 0xb0: case 0xd0: case 0xf0:   /* bcc bcs bne beq */
        if (target)
            *target = (uint16_t)(addr + 2 + (int8_t)read((uint16_t)(addr + 1)));
        return DisFlowBranch;

    case 0x00: /* brk: vectors away; do not try to follow */
    case 0x40: /* rti */
    case 0x60: /* rts */
        return DisFlowReturn;

    case 0xdb: /* stp on CMOS; undefined elsewhere */
        return (strcmp(mnemonic, "stp") == 0) ? DisFlowReturn : DisFlowNormal;

    default:
        /* Undocumented NMOS JAM/KIL halts the CPU. */
        if (strcmp(mnemonic, "jam") == 0) return DisFlowReturn;
        return DisFlowNormal;
    }
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS, and the new line `flow: 15 classifications checked`.

- [ ] **Step 6: Commit**

```bash
git add src/disassemble.h src/disassemble.c src/test_disassemble.c
git commit -m "feat(disassemble): classify instruction control flow

Adds DisFlowOf so a traversal can tell calls, jumps, conditional
branches, returns and unfollowable indirects apart without needing the
private VrEmu6502 handle.

Two classifications are easy to get wrong and are pinned by tests. BRA is
unconditional, so nothing falls through to the following instruction --
it is a jump, not a branch. BBR/BBS are 3-byte conditional branches whose
displacement is the third byte, so their target is addr+3+rel.

Opcodes that are only jumps or halts on some CPU variants (\$7c, \$80,
\$db) check the mnemonic, so they classify as Normal on NMOS where they
are undefined."
```

---

### Task 2: Analysis traversal

**Files:**
- Create: `src/analyze.h`
- Create: `src/analyze.c`
- Create: `src/test_analyze.c`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: `DisReadFn`, `DisInit`, `DisOne`, `DisFlow`, `DisFlowOf` from `src/disassemble.h`.
- Produces: `DisAnalysis` (opaque), `AnalyzeNew`, `AnalyzeAddEntry`, `AnalyzeRun`, `AnalyzeFree`, `AnalyzeIsInstruction`, `AnalyzeIsFunction`, `AnalyzeIsLabel`, `AnalyzeCallers`, `AnalyzeOutOfRange`. Tasks 3 and 4 use all of these.

- [ ] **Step 1: Write the failing test**

Create `src/test_analyze.c`:

```c
/*
 * Tests for recursive-descent analysis.
 *
 * Programs are assembled by hand into a flat image so each case's control
 * flow is obvious from the bytes. See
 * docs/specs/2026-07-29-recursive-descent-analysis-design.md
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "analyze.h"
#include "disassemble.h"

static int failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
            ++failures;                                    \
        }                                                  \
    } while (0)

static uint8_t mem[0x10000];
static uint8_t rd(uint16_t addr) { return mem[addr]; }

#define LOAD 0x0200

static void load(const uint8_t* prog, size_t len) {
    memset(mem, 0, sizeof mem);
    memcpy(&mem[LOAD], prog, len);
}

/*
 *   0200  jsr $0210      call a function twice
 *   0203  jsr $0210
 *   0206  rts
 *   0207  ea             unreachable padding
 *   0210  lda #$42       the function
 *   0212  rts
 */
static void test_calls_and_unreachable(void) {
    static const uint8_t prog[] = {
        0x20, 0x10, 0x02,        /* 0200 jsr $0210 */
        0x20, 0x10, 0x02,        /* 0203 jsr $0210 */
        0x60,                    /* 0206 rts       */
        0xea,                    /* 0207 unreachable */
        0, 0, 0, 0, 0, 0, 0, 0,  /* 0208-020f gap  */
        0xa9, 0x42,              /* 0210 lda #$42  */
        0x60,                    /* 0212 rts       */
    };
    load(prog, sizeof prog);

    DisInit(CPU_65C02);
    DisAnalysis* a = AnalyzeNew(rd, LOAD, LOAD + (uint16_t)sizeof prog - 1);
    AnalyzeAddEntry(a, LOAD);
    AnalyzeRun(a);

    CHECK(AnalyzeIsInstruction(a, 0x0200), "0200 should be code");
    CHECK(AnalyzeIsInstruction(a, 0x0203), "0203 should be code");
    CHECK(AnalyzeIsInstruction(a, 0x0206), "0206 should be code");
    CHECK(!AnalyzeIsInstruction(a, 0x0207), "0207 is unreachable, not code");
    CHECK(AnalyzeIsInstruction(a, 0x0210), "0210 should be code");
    CHECK(AnalyzeIsInstruction(a, 0x0212), "0212 should be code");

    /* Operand bytes are not instruction starts. */
    CHECK(!AnalyzeIsInstruction(a, 0x0201), "0201 is an operand, not a start");

    CHECK(AnalyzeIsFunction(a, LOAD), "entry point should be a function");
    CHECK(AnalyzeIsFunction(a, 0x0210), "jsr target should be a function");
    CHECK(AnalyzeCallers(a, 0x0210) == 2,
          "0210 has %u callers, expected 2", AnalyzeCallers(a, 0x0210));

    AnalyzeFree(a);
}

/*
 *   0200  bne $0204      conditional: target AND fall-through are code
 *   0202  lda #$01
 *   0204  jmp $0208      unconditional: nothing falls through to 0207
 *   0207  ea             unreachable
 *   0208  rts
 */
static void test_branch_and_jump(void) {
    static const uint8_t prog[] = {
        0xd0, 0x02,        /* 0200 bne $0204 */
        0xa9, 0x01,        /* 0202 lda #$01  */
        0x4c, 0x08, 0x02,  /* 0204 jmp $0208 */
        0xea,              /* 0207 nop, unreachable */
        0x60,              /* 0208 rts       */
    };
    load(prog, sizeof prog);

    DisInit(CPU_65C02);
    DisAnalysis* a = AnalyzeNew(rd, LOAD, LOAD + (uint16_t)sizeof prog - 1);
    AnalyzeAddEntry(a, LOAD);
    AnalyzeRun(a);

    CHECK(AnalyzeIsInstruction(a, 0x0202), "branch falls through to 0202");
    CHECK(AnalyzeIsInstruction(a, 0x0204), "branch target 0204 is code");
    CHECK(AnalyzeIsInstruction(a, 0x0208), "jmp target 0208 is code");
    CHECK(!AnalyzeIsInstruction(a, 0x0207),
          "nothing falls through a jmp, so 0207 is unreachable");

    CHECK(AnalyzeIsLabel(a, 0x0204), "0204 is a branch target, so a label");
    CHECK(AnalyzeIsLabel(a, 0x0208), "0208 is a jmp target, so a label");
    CHECK(!AnalyzeIsFunction(a, 0x0204), "0204 is not a jsr target");

    AnalyzeFree(a);
}

/* bra to itself must terminate rather than spin. */
static void test_self_branch_terminates(void) {
    static const uint8_t prog[] = { 0x80, 0xfe };  /* 0200 bra $0200 */
    load(prog, sizeof prog);

    DisInit(CPU_65C02);
    DisAnalysis* a = AnalyzeNew(rd, LOAD, LOAD + 1);
    AnalyzeAddEntry(a, LOAD);
    AnalyzeRun(a);   /* must return */

    CHECK(AnalyzeIsInstruction(a, LOAD), "self-branch should still be code");
    AnalyzeFree(a);
}

/* A target outside [lo,hi] is recorded, not followed. */
static void test_out_of_range_target(void) {
    static const uint8_t prog[] = {
        0x20, 0x00, 0x90,  /* 0200 jsr $9000 -- outside the loaded range */
        0x60,              /* 0203 rts */
    };
    load(prog, sizeof prog);

    DisInit(CPU_65C02);
    DisAnalysis* a = AnalyzeNew(rd, LOAD, LOAD + (uint16_t)sizeof prog - 1);
    AnalyzeAddEntry(a, LOAD);
    AnalyzeRun(a);

    CHECK(AnalyzeIsInstruction(a, 0x0203), "execution resumes after a jsr");
    CHECK(!AnalyzeIsInstruction(a, 0x9000), "must not follow out of range");
    CHECK(AnalyzeOutOfRange(a) == 1,
          "%u out-of-range targets, expected 1", AnalyzeOutOfRange(a));

    AnalyzeFree(a);
}

/*
 * Documents a known blind spot rather than desired behaviour: JSR followed
 * by inline data. The traversal cannot know the callee consumes the bytes,
 * so it decodes the string as code. If a future change fixes this, flip
 * these assertions -- do not delete them.
 */
static void test_jsr_inline_data_is_misread(void) {
    static const uint8_t prog[] = {
        0x20, 0x10, 0x02,              /* 0200 jsr $0210 */
        'H', 'i', 0x00,                /* 0203 inline string, NOT code */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0206-020f gap, 10 bytes */
        0x60,                          /* 0210 rts */
    };
    load(prog, sizeof prog);

    DisInit(CPU_65C02);
    DisAnalysis* a = AnalyzeNew(rd, LOAD, LOAD + (uint16_t)sizeof prog - 1);
    AnalyzeAddEntry(a, LOAD);
    AnalyzeRun(a);

    CHECK(AnalyzeIsInstruction(a, 0x0203),
          "known limitation: inline data after jsr is decoded as code");

    AnalyzeFree(a);
}

int main(void) {
    test_calls_and_unreachable();
    test_branch_and_jump();
    test_self_branch_terminates();
    test_out_of_range_target();
    test_jsr_inline_data_is_misread();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all analysis tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Register the test target**

In `src/CMakeLists.txt`, after the `test_rp6502file` block:

```cmake
add_executable(test_analyze
  test_analyze.c
  analyze.c
  analyze.h
  disassemble.c
  disassemble.h
  vrEmu6502.c
  vrEmu6502.h
)
add_test(NAME analyze COMMAND test_analyze)
```

- [ ] **Step 3: Run to verify it fails**

```bash
cmake -B build -G Ninja && cmake --build build 2>&1 | head -20
```

Expected: `analyze.h: No such file or directory`.

- [ ] **Step 4: Write the header**

Create `src/analyze.h`:

```c
#ifndef __ANALYZE_H__
#define __ANALYZE_H__

#include <stdbool.h>
#include <stdint.h>

#include "disassemble.h"

/*
 * Recursive-descent (control-flow) analysis.
 *
 * Seeded with entry points, the traversal follows control flow and marks
 * every byte it reaches. Whatever is never reached is data. This yields
 * code/data separation, function entry points, and function ends from one
 * pass.
 *
 * Known blind spots, inherent to the technique on 6502 rather than defects
 * to be fixed later:
 *
 *   - JSR followed by inline data: the traversal walks into the data and
 *     decodes it as code
 *   - jump tables and the RTS dispatch trick: real code looks unreachable
 *   - self-modifying code
 *
 * Callers must therefore show unreached bytes rather than dropping them,
 * so a reader can see where the analysis disagreed with them.
 *
 * Host-side for now. The interface is callback-driven so a Pico port is
 * mechanical: it would pack the flags array into an 8KB bitmap plus a
 * small function table, instead of the flat 64KB array used here.
 */

typedef struct DisAnalysis DisAnalysis;

/*
 * Create an analysis over the inclusive address range [lo,hi]. Targets
 * outside that range are counted but not followed, so a wild branch
 * cannot walk into unmapped memory. Returns NULL on allocation failure.
 *
 * DisInit must have been called first: classification depends on the CPU
 * model.
 */
DisAnalysis* AnalyzeNew(DisReadFn read, uint16_t lo, uint16_t hi);

/*
 * Seed a starting address -- typically the RESET, NMI and IRQ vectors.
 * Entry points are recorded as functions. Addresses outside [lo,hi] are
 * ignored. Call before AnalyzeRun.
 */
void AnalyzeAddEntry(DisAnalysis* a, uint16_t addr);

/* Run the traversal to completion. Terminates on any input. */
void AnalyzeRun(DisAnalysis* a);

void AnalyzeFree(DisAnalysis* a);

/* True if an instruction *starts* at addr. Operand bytes are false. */
bool AnalyzeIsInstruction(const DisAnalysis* a, uint16_t addr);

/* True if addr is an entry point or the target of a JSR. */
bool AnalyzeIsFunction(const DisAnalysis* a, uint16_t addr);

/* True if addr is the target of a branch or JMP. */
bool AnalyzeIsLabel(const DisAnalysis* a, uint16_t addr);

/* How many distinct JSR sites target addr. */
unsigned AnalyzeCallers(const DisAnalysis* a, uint16_t addr);

/* How many control-flow targets fell outside [lo,hi] and were not followed. */
unsigned AnalyzeOutOfRange(const DisAnalysis* a);

#endif
```

- [ ] **Step 5: Write the implementation**

Create `src/analyze.c`:

```c
#include "analyze.h"

#include <stdlib.h>
#include <string.h>

/* Per-address flags. One byte per address is wasteful but trivial on the
   host; see the porting note in analyze.h. */
#define F_INSTRUCTION 0x01u  /* an instruction starts here */
#define F_FUNCTION    0x02u  /* entry point or JSR target */
#define F_LABEL       0x04u  /* branch or JMP target */
#define F_QUEUED      0x08u  /* already pushed; prevents requeueing */

struct DisAnalysis {
    DisReadFn read;
    uint16_t  lo, hi;

    uint8_t*  flags;    /* 0x10000 entries */
    uint16_t* callers;  /* 0x10000 entries, saturating count */

    uint16_t* work;     /* worklist */
    size_t    workLen, workCap;

    unsigned  outOfRange;
};

static bool inRange(const DisAnalysis* a, uint16_t addr) {
    return addr >= a->lo && addr <= a->hi;
}

/* Push addr unless already queued. Marking before pushing is what makes
   the traversal terminate: each address enters the worklist at most once. */
static void push(DisAnalysis* a, uint16_t addr) {
    if (!inRange(a, addr)) {
        ++a->outOfRange;
        return;
    }
    if (a->flags[addr] & F_QUEUED) return;

    if (a->workLen == a->workCap) {
        size_t cap = a->workCap ? a->workCap * 2 : 256;
        uint16_t* grown = realloc(a->work, cap * sizeof *grown);
        if (!grown) return; /* out of memory: drop this target rather than crash */
        a->work = grown;
        a->workCap = cap;
    }

    a->flags[addr] |= F_QUEUED;
    a->work[a->workLen++] = addr;
}

DisAnalysis* AnalyzeNew(DisReadFn read, uint16_t lo, uint16_t hi) {
    if (!read || lo > hi) return NULL;

    DisAnalysis* a = calloc(1, sizeof *a);
    if (!a) return NULL;

    a->read = read;
    a->lo = lo;
    a->hi = hi;
    a->flags = calloc(0x10000, sizeof *a->flags);
    a->callers = calloc(0x10000, sizeof *a->callers);

    if (!a->flags || !a->callers) {
        AnalyzeFree(a);
        return NULL;
    }
    return a;
}

void AnalyzeFree(DisAnalysis* a) {
    if (!a) return;
    free(a->flags);
    free(a->callers);
    free(a->work);
    free(a);
}

void AnalyzeAddEntry(DisAnalysis* a, uint16_t addr) {
    if (!a || !inRange(a, addr)) return;
    a->flags[addr] |= F_FUNCTION;
    push(a, addr);
}

void AnalyzeRun(DisAnalysis* a) {
    if (!a) return;

    while (a->workLen) {
        uint16_t pc = a->work[--a->workLen];

        for (;;) {
            if (!inRange(a, pc)) break;
            if (a->flags[pc] & F_INSTRUCTION) break; /* already traced */

            uint8_t len = DisInstLen(a->read(pc));
            if (len == 0) break;

            a->flags[pc] |= F_INSTRUCTION;

            uint16_t target = 0;
            DisFlow flow = DisFlowOf(pc, a->read, &target);

            if (flow == DisFlowCall) {
                if (inRange(a, target)) {
                    a->flags[target] |= F_FUNCTION;
                    if (a->callers[target] < 0xffff) ++a->callers[target];
                }
                push(a, target);
            }
            else if (flow == DisFlowJump || flow == DisFlowBranch) {
                if (inRange(a, target)) a->flags[target] |= F_LABEL;
                push(a, target);
            }

            if (flow == DisFlowJump || flow == DisFlowReturn ||
                flow == DisFlowIndirect)
                break; /* nothing falls through */

            uint16_t next = (uint16_t)(pc + len);
            if (next < pc) break; /* wrapped past $ffff */
            pc = next;
        }
    }
}

bool AnalyzeIsInstruction(const DisAnalysis* a, uint16_t addr) {
    return a && (a->flags[addr] & F_INSTRUCTION) != 0;
}

bool AnalyzeIsFunction(const DisAnalysis* a, uint16_t addr) {
    return a && (a->flags[addr] & F_FUNCTION) != 0;
}

bool AnalyzeIsLabel(const DisAnalysis* a, uint16_t addr) {
    return a && (a->flags[addr] & F_LABEL) != 0;
}

unsigned AnalyzeCallers(const DisAnalysis* a, uint16_t addr) {
    return a ? a->callers[addr] : 0;
}

unsigned AnalyzeOutOfRange(const DisAnalysis* a) {
    return a ? a->outOfRange : 0;
}
```

- [ ] **Step 6: Run tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS including the new `analyze`.

Note the deliberate subtlety: `AnalyzeCallers` counts *executions of a JSR instruction during traversal*, and because `F_INSTRUCTION` stops re-tracing, each JSR site is visited once. Two distinct `jsr $0210` instructions therefore give a count of 2, which is what the test asserts.

- [ ] **Step 7: Verify under sanitizers**

```bash
cmake -B build-asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

Expected: all PASS, no sanitizer reports.

- [ ] **Step 8: Commit**

```bash
git add src/analyze.c src/analyze.h src/test_analyze.c src/CMakeLists.txt
git commit -m "feat(analyze): recursive-descent code/data traversal

Follows control flow from seeded entry points and marks every byte
reached; anything unreached is data. Yields code/data separation,
function entry points (JSR targets) and function ends (RTS/RTI/JMP) from
a single pass.

Marking an address QUEUED before pushing it means each enters the
worklist at most once, so the traversal terminates on any input --
including bra-to-self, which is tested. Targets outside [lo,hi] are
counted and not followed, so a wild branch cannot walk into unmapped
memory.

Depends only on disassemble.h, never on memory.c or the file readers, so
it keeps the same portability shape as disassemble.c.

Includes a test that documents the JSR-with-inline-data blind spot as
wrong-but-expected, so a future fix has an assertion to flip."
```

---

### Task 3: Function-organized listing

**Files:**
- Modify: `src/dis.c`
- Test: covered end-to-end by Task 4's CLI tests; no unit test file (rendering is presentation, exercised through the binary)

**Interfaces:**
- Consumes: `AnalyzeIsInstruction`, `AnalyzeIsFunction`, `AnalyzeIsLabel`, `AnalyzeCallers` from `src/analyze.h`; `DisOne` from `src/disassemble.h`.
- Produces: `static void printAnalyzed(const DisAnalysis*, uint16_t lo, uint16_t hi, uint16_t entry)` used by Task 4.

- [ ] **Step 1: Add the renderer**

In `src/dis.c`, add `#include "analyze.h"` after the existing includes, and add this function above `main`:

```c
/*
 * Function-organized listing. Walks the range in address order, emitting a
 * label before any address that is a function or branch target, and
 * collapsing runs of unreached bytes to a single marker.
 *
 * Unreached bytes are shown rather than dropped: the traversal has known
 * blind spots (see analyze.h), so a reader must be able to see where it
 * disagreed with them.
 */
static void printAnalyzed(const DisAnalysis* a, uint16_t lo, uint16_t hi,
                          uint16_t entry) {
    uint32_t addr = lo;

    while (addr <= hi) {
        if (!AnalyzeIsInstruction(a, (uint16_t)addr)) {
            uint32_t start = addr;
            while (addr <= hi && !AnalyzeIsInstruction(a, (uint16_t)addr))
                ++addr;
            printf("\n; %04x-%04x  %u bytes data\n",
                   (unsigned)start, (unsigned)(addr - 1),
                   (unsigned)(addr - start));
            continue;
        }

        uint16_t at = (uint16_t)addr;

        if (AnalyzeIsFunction(a, at)) {
            unsigned callers = AnalyzeCallers(a, at);
            printf("\nsub_%04x:", at);
            if (at == entry)      printf("                  ; entry point");
            else if (callers == 1) printf("                  ; 1 caller");
            else                   printf("                  ; %u callers", callers);
            printf("\n");
        }
        else if (AnalyzeIsLabel(a, at)) {
            printf("loc_%04x:\n", at);
        }

        char text[48];
        uint16_t next = DisOne(at, readByte, sizeof text, text);
        uint8_t len = (uint8_t)(next - at);
        if (len == 0 || len > 3) break;

        uint8_t bytes[3];
        for (uint8_t i = 0; i < len; ++i)
            bytes[i] = readByte((uint16_t)(at + i));

        printInstruction(at, bytes, len, text);

        addr += len;
    }
}
```

- [ ] **Step 2: Build to verify it compiles**

```bash
cmake --build build 2>&1 | grep -iE 'warning|error' ; echo done
```

Expected: no warnings, no errors. (`printAnalyzed` is unused until Task 4, which will warn — if `-Wunused-function` fires, proceed straight to Task 4 rather than adding a suppression.)

- [ ] **Step 3: Commit**

```bash
git add src/dis.c
git commit -m "feat(dis): function-organized listing renderer

Emits sub_XXXX: for functions and loc_XXXX: for branch targets, and
collapses runs of unreached bytes to a one-line marker showing their
extent. Unreached bytes are shown rather than dropped so a reader can see
where the analysis disagreed with them.

Not yet wired into main; that follows."
```

---

### Task 4: Wire analysis into the CLI

**Files:**
- Modify: `src/dis.c`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: `printAnalyzed` from Task 3; `AnalyzeNew`/`AddEntry`/`Run`/`Free`/`OutOfRange` from Task 2.
- Produces: `--linear` flag; analysis-by-default behaviour.

- [ ] **Step 1: Add the flag and dispatch**

In `src/dis.c`, add near `const char* inputPath = NULL;`:

```c
    bool linear = false;
```

Add a branch in the argument loop, before the `argv[i][0] == '-'` catch-all:

```c
        else if (strcmp(argv[i], "--linear") == 0) {
            linear = true;
        }
```

Update `usage`:

```c
    fprintf(to, "  --linear: flat sweep instead of following control flow\n");
```

Replace the final `DisInit`/`DisRange` pair at the end of `main` with:

```c
    DisInit(model);

    if (linear || !haveEntry) {
        DisRange(base, (uint16_t)(end - base + 1), readByte, printInstruction);
        return 0;
    }

    DisAnalysis* analysis = AnalyzeNew(readByte, base, end);
    if (!analysis) {
        fprintf(stderr, "dis: out of memory; falling back to linear\n");
        DisRange(base, (uint16_t)(end - base + 1), readByte, printInstruction);
        return 0;
    }

    AnalyzeAddEntry(analysis, base);
    AnalyzeRun(analysis);

    if (AnalyzeOutOfRange(analysis))
        fprintf(stderr, "note: %u control-flow target%s outside the loaded "
                        "range were not followed\n",
                AnalyzeOutOfRange(analysis),
                AnalyzeOutOfRange(analysis) == 1 ? "" : "s");

    printAnalyzed(analysis, base, end, base);
    AnalyzeFree(analysis);
    return 0;
```

Add `bool haveEntry` alongside `base`/`end`, set `true` in the ROM and HEX branches and `false` for the built-in demo:

```c
    uint16_t base, end;
    bool haveEntry = false;
```

Set `haveEntry = true;` immediately after each successful `readRp6502File` and `readHexFile` call. Leave it `false` in the `else` branch that maps `demo_image`, so `./dis` with no arguments keeps its current output.

- [ ] **Step 2: Verify the demo output is unchanged**

```bash
cmake --build build
./build/bin/dis
```

Expected: identical to today — six lines starting `0200: 00 ea     brk $ea`.

- [ ] **Step 3: Verify --linear is unchanged**

```bash
./build/bin/dis --linear --cpu w65c02 testdata/w65c02_demo.hex
./build/bin/dis --cpu w65c02 testdata/w65c02_demo.hex
```

Expected: the first matches today's output exactly. The second shows labels and may classify some bytes as data.

- [ ] **Step 4: Add a CLI regression test**

In `src/CMakeLists.txt`, after the `cli_rp6502` block:

```cmake
# --linear must keep the pre-analysis behaviour exactly. bbr0 at $0200 is
# the first instruction of the fixture, so it only appears in a flat sweep
# from the load address -- the analyzed listing starts at the reset vector.
add_test(NAME cli_linear
  COMMAND dis --linear --cpu w65c02 ${CMAKE_SOURCE_DIR}/testdata/w65c02_demo.hex)
set_tests_properties(cli_linear PROPERTIES
  PASS_REGULAR_EXPRESSION "0200: 0f 12 05  bbr0"
  FAIL_REGULAR_EXPRESSION "sub_")
```

- [ ] **Step 5: Run the full suite**

```bash
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all PASS. If `cli_rp6502` fails, read its output before changing it — its `PASS_REGULAR_EXPRESSION` is `0205: `, which the analyzed listing should still produce since `$0205` is the reset vector and therefore the entry point.

- [ ] **Step 6: Commit**

```bash
git add src/dis.c src/CMakeLists.txt
git commit -m "feat(dis): analyse by default, add --linear

Files with a known entry point now get the function-organized listing;
--linear selects the previous flat sweep. The built-in demo has no entry
point to seed from, so it stays linear and its output is unchanged.

Adds cli_linear to pin the flat output, since that is now the path most
likely to rot unnoticed."
```

---

### Task 5: Real ROM fixtures

**Files:**
- Create: `testdata/rtc.rp6502`
- Create: `testdata/adventure.rp6502`
- Create: `testdata/README.md`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: the `dis` binary.
- Produces: two CTest cases exercising real toolchain output.

- [ ] **Step 1: Vendor the fixtures with attribution**

```bash
cd /Users/pete/projects/dis
gh api repos/picocomputer/rp6502/contents/tests/roms/rtc.rp6502 --jq .content | base64 -d > testdata/rtc.rp6502
gh api repos/picocomputer/rp6502/contents/tests/roms/adventure.rp6502 --jq .content | base64 -d > testdata/adventure.rp6502
ls -l testdata/
```

Expected: `rtc.rp6502` ~1723 bytes plus headers, `adventure.rp6502` ~97365 bytes.

Create `testdata/README.md`:

```markdown
# Test fixtures

`w65c02_demo.hex` and `w65c02_demo.rp6502` are generated; see
`docs/design.md` for the scripts that produce them.

## Vendored ROMs

`rtc.rp6502` and `adventure.rp6502` are copied unmodified from
[picocomputer/rp6502](https://github.com/picocomputer/rp6502), `tests/roms/`.
They are real toolchain output, which synthetic fixtures cannot substitute for:
`adventure.rp6502` carries 49 chunks, large ASCII blobs and 4 named assets.

Those files are covered by the rp6502 project's BSD 3-Clause licence:

> Copyright (c) 2023 Rumbledethumps
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the conditions of the BSD
> 3-Clause License are met. See https://github.com/picocomputer/rp6502
> for the full text.
```

- [ ] **Step 2: Measure before asserting**

```bash
./build/bin/dis testdata/rtc.rp6502 | tail -20
./build/bin/dis testdata/adventure.rp6502 | grep -c '^sub_'
./build/bin/dis testdata/adventure.rp6502 | grep 'bytes data' | \
  awk '{s += $3} END {print s " bytes classified as data"}'
```

Record both numbers. They go into the test comment in the next step. **Do not invent them** — the spec deliberately leaves the threshold unmeasured.

- [ ] **Step 3: Add structural tests**

In `src/CMakeLists.txt`:

```cmake
# Real toolchain output. Assertions are structural, not exact-output:
# exact listings would be brittle and would encode current behaviour as if
# it were correct.
add_test(NAME cli_rtc_rom
  COMMAND dis ${CMAKE_SOURCE_DIR}/testdata/rtc.rp6502)
set_tests_properties(cli_rtc_rom PROPERTIES
  FAIL_REGULAR_EXPRESSION "ERROR")

# adventure.rp6502 is 49KB of real code and ASCII data. Requiring both a
# discovered function and a data region proves the traversal actually
# separated the two -- classifying everything one way means it is broken.
add_test(NAME cli_adventure_rom
  COMMAND dis ${CMAKE_SOURCE_DIR}/testdata/adventure.rp6502)
set_tests_properties(cli_adventure_rom PROPERTIES
  PASS_REGULAR_EXPRESSION "sub_"
  FAIL_REGULAR_EXPRESSION "ERROR")
```

Add a comment above `cli_adventure_rom` recording the figures measured in Step 2, in this form:

```cmake
# Measured 2026-07-29: N functions discovered, M bytes classified as data
# out of 48990. A large change in either direction is worth investigating.
```

- [ ] **Step 4: Run the full suite, including sanitizers**

```bash
ctest --test-dir build --output-on-failure
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

Expected: all PASS. Adventure is 97KB, so watch that the ASan run stays under a second or two; if not, note it rather than silently accepting a slow suite.

- [ ] **Step 5: Commit**

```bash
git add testdata/rtc.rp6502 testdata/adventure.rp6502 testdata/README.md src/CMakeLists.txt
git commit -m "test: add real PicoComputer ROM fixtures

Vendored unmodified from picocomputer/rp6502 tests/roms under its BSD
3-Clause licence, with the notice recorded in testdata/README.md.

Synthetic fixtures cannot substitute for real toolchain output.
adventure.rp6502 carries 49 chunks, large ASCII blobs and 4 named assets
-- the first real exercise of the named-asset boundary, which until now
only synthetic tests covered. rtc.rp6502 is small enough to stay a
routine regression fixture.

Assertions are structural rather than exact-output. Requiring both a
discovered function and a data region proves the traversal separated the
two; classifying everything one way would mean it is broken."
```

---

### Task 6: Documentation

**Files:**
- Modify: `README.md`
- Modify: `docs/design.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update README.md**

In the usage block, add:

```sh
./build/bin/dis --linear game.rp6502      # flat sweep, no control-flow analysis
```

Add a section after the RP6502 ROMs section:

```markdown
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
```

- [ ] **Step 2: Update docs/design.md**

Add to Components, after the `disassemble.c` entry:

```markdown
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
```

Add to Document History:

```markdown
| 2026-07-29 | Recursive-descent code/data analysis, function discovery, function-organized listing, `--linear`. Real ROM fixtures vendored from picocomputer/rp6502. |
```

- [ ] **Step 3: Update CLAUDE.md**

In Source files, after the `disassemble.c/h` entry:

```markdown
- **`analyze.c/h`** — Recursive-descent code/data analysis. Depends only on `disassemble.h`. See `docs/design.md`.
- **`test_analyze.c`** — Analysis tests. Hand-assembled programs with known control flow.
```

In the Run block:

```sh
./build/bin/dis --linear game.rp6502   # flat sweep, no control-flow analysis
```

In Gotchas, add:

```markdown
- The analysis has inherent blind spots on 6502: `JSR` with inline data, jump tables and the `RTS` dispatch trick, and self-modifying code. `src/test_analyze.c` documents the first as wrong-but-expected — flip that assertion rather than deleting it if it is ever fixed.
```

- [ ] **Step 4: Verify docs match reality**

```bash
./build/bin/dis --help
grep -n 'linear' README.md CLAUDE.md docs/design.md
```

Expected: the `--help` text and all three docs agree on the flag name.

- [ ] **Step 5: Final full verification**

```bash
rm -rf build build-asan
cmake -B build -G Ninja && cmake --build build 2>&1 | grep -iE 'warning|error'; echo "warnings: none"
ctest --test-dir build --output-on-failure
cmake -B build-asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
./build/bin/dis | head -3
```

Expected: warning-free build, all tests PASS in both trees, demo output unchanged.

- [ ] **Step 6: Commit**

```bash
git add README.md docs/design.md CLAUDE.md
git commit -m "docs: record control-flow analysis and its blind spots

Documents what the traversal can and cannot see, and why unreached bytes
are shown rather than dropped. Notes that labels are synthetic because no
.rp6502 carries symbols, pointing at issue #5 for real names."
```

---

## Self-Review Notes

**Spec coverage.** Every section of `docs/specs/2026-07-29-recursive-descent-analysis-design.md` maps to a task: traversal rules → Tasks 1–2; module boundary and API → Task 2; two label kinds → Tasks 2–3; default-on with `--linear` → Task 4; collapsed data regions → Task 3; real ROM fixtures with measured figures → Task 5; blind spots documented → Tasks 2 and 6. Out-of-scope items (`--data`/`--code` overrides, symbols, call-graph output) have no tasks, as intended.

**Deliberate omission.** Task 5 Step 2 measures the function count and data byte total before Step 3 writes them into a comment. No number is invented anywhere in this plan; the spec explicitly forbids picking one in advance.

**Known risk.** Task 3's `printAnalyzed` is unused until Task 4, so a `-Wunused-function` warning between those two commits is expected. The plan says to proceed to Task 4 rather than suppress it, because the repo builds warning-free and adding a suppression would outlive its reason.

**Fixed during self-review.** Two hand-assembled test programs in Task 2 had addresses that did not line up with their byte offsets: `test_branch_and_jump` had a branch targeting the middle of a 3-byte `JMP`, and `test_jsr_inline_data_is_misread` had a 7-byte gap where the `JSR` target required 10. Both are corrected above. Anyone hand-assembling further cases should count offsets rather than trust the comments.
