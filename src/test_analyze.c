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
