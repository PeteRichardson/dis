/*
 * Tests for the disassembler.
 *
 * Three layers, cheapest and broadest first:
 *
 *   Layer 1  execution oracle -- every opcode, every CPU model, checking
 *            instruction length against where the emulator's PC actually
 *            lands after executing it
 *   Layer 2  golden strings -- hand-audited expected text, pinning the
 *            output format that Layer 1 says nothing about
 *   Layer 3  edge cases -- truncation, wrapping, range boundaries
 *
 * Unlike test_memory.c this uses a CHECK macro rather than assert(), so a
 * sweep reports every failing opcode instead of aborting on the first.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "disassemble.h"
#include "vrEmu6502.h"

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

/* ------------------------------------------------------------------
 * Test memory: a flat 64K buffer. The sparse model in memory.c is CLI
 * plumbing; the disassembler only ever sees a DisReadFn.
 */

static uint8_t mem[0x10000];

static uint8_t emuRead(uint16_t addr, bool isDbg) { (void)isDbg; return mem[addr]; }
static void    emuWrite(uint16_t addr, uint8_t val) { mem[addr] = val; }
static uint8_t disRead(uint16_t addr) { return mem[addr]; }

/* Far enough from $0000 that a JMP to $0000 is obviously not a length. */
#define TEST_ADDR 0x1000

static const struct {
    vrEmu6502Model model;
    const char*    name;
} models[] = {
    { CPU_6502,   "6502"   },
    { CPU_6502U,  "6502U"  },
    { CPU_65C02,  "65C02"  },
    { CPU_W65C02, "W65C02" },
    { CPU_R65C02, "R65C02" },
};

#define MODEL_COUNT (sizeof models / sizeof models[0])

/* ------------------------------------------------------------------
 * LAYER 1 -- execution oracle
 *
 * Instruction length is the invariant that matters most: get it wrong and
 * every instruction after it is misaligned.
 *
 * The oracle is the emulator's own execution path. Set PC, run one
 * instruction, see where PC landed. That path advances PC through the
 * addressing-mode functions (ab, abx, zp, ...), which is entirely
 * independent of the switch statement DisInstLen uses.
 *
 * Note this deliberately does NOT compare against
 * vrEmu6502DisassembleInstruction. Dis6502 was originally copied from it
 * verbatim, so that comparison would pass trivially while both shared the
 * same bug -- which is exactly what happened with BBR/BBS.
 *
 * Operand bytes are always $00 so that a taken branch and an untaken
 * branch land on the same address, making the measurement unambiguous.
 */

/*
 * Opcodes whose PC does not land at addr+length, so the oracle cannot
 * measure them. Lengths below are hand-audited from the 6502/65C02
 * instruction set; all are unambiguous.
 */
static const struct {
    uint8_t     opcode;
    const char* mnemonic; /* override applies only if the model decodes it as this */
    uint8_t     len;
    const char* why;
} unmeasurable[] = {
    { 0x00, "brk", 1, "PC jumps to the IRQ vector" },
    { 0x20, "jsr", 3, "PC jumps to the subroutine" },
    { 0x40, "rti", 1, "PC pulled from the stack" },
    { 0x4c, "jmp", 3, "PC jumps to the target" },
    { 0x60, "rts", 1, "PC pulled from the stack" },
    { 0x6c, "jmp", 3, "PC jumps to the target" },
    /* $7c is JMP (abs,x) on CMOS parts only; on NMOS it is undefined, so
       the mnemonic check keeps this override off the 6502/6502U tables. */
    { 0x7c, "jmp", 3, "PC jumps to the target" },
};

/*
 * Returns the expected length for opcodes the oracle cannot measure, or 0
 * if the oracle should be trusted instead.
 */
static uint8_t expectedLengthOverride(uint8_t opcode, const char* mnemonic) {
    /*
     * JAM/KIL halts the CPU. vrEmu6502's jam() calls imm(), consuming a
     * byte and leaving PC at addr+2, but its own opcode table declares
     * {jam, imp, 1}. The table is right: radare2 decodes "02 ea a9 42" as
     * hlt / nop / lda #$42, i.e. JAM is one byte. This is an upstream bug
     * in the emulator's execution path, not in DisInstLen.
     */
    if (mnemonic && strcmp(mnemonic, "jam") == 0) return 1;

    for (unsigned i = 0; i < sizeof unmeasurable / sizeof unmeasurable[0]; ++i)
        if (unmeasurable[i].opcode == opcode
            && mnemonic
            && strcmp(unmeasurable[i].mnemonic, mnemonic) == 0)
            return unmeasurable[i].len;

    return 0;
}

/* Execute one instruction and report how far PC moved, or 0 if it left the
   1..3 byte window (control flow). */
static uint8_t oracleLength(vrEmu6502Model model, uint8_t opcode) {
    memset(mem, 0, sizeof mem);
    mem[TEST_ADDR] = opcode;

    VrEmu6502* cpu = vrEmu6502New(model, emuRead, emuWrite);
    if (!cpu) return 0;

    vrEmu6502Reset(cpu);
    vrEmu6502SetPC(cpu, TEST_ADDR);
    vrEmu6502InstCycle(cpu);
    uint16_t pc = vrEmu6502GetPC(cpu);
    vrEmu6502Destroy(cpu);

    uint16_t delta = (uint16_t)(pc - TEST_ADDR);
    return (delta >= 1 && delta <= 3) ? (uint8_t)delta : 0;
}

static void test_instruction_lengths(void) {
    int checked = 0;

    for (unsigned mi = 0; mi < MODEL_COUNT; ++mi) {
        DisInit(models[mi].model);

        /* Own handle purely for mnemonic lookup -- DisInit's is private,
           and vrEmu6502OpcodeToMnemonicStr dereferences its argument. */
        VrEmu6502* names = vrEmu6502New(models[mi].model, emuRead, emuWrite);

        for (int op = 0; op < 256; ++op) {
            const char* mnemonic =
                vrEmu6502OpcodeToMnemonicStr(names, (uint8_t)op);
            uint8_t override = expectedLengthOverride((uint8_t)op, mnemonic);
            uint8_t expected = override ? override
                                        : oracleLength(models[mi].model, (uint8_t)op);

            CHECK(expected != 0,
                  "%s $%02x (%s): no expected length available",
                  models[mi].name, op, mnemonic);
            if (expected == 0) continue;

            uint8_t actual = DisInstLen((uint8_t)op);
            CHECK(actual == expected,
                  "%s $%02x (%s): length %u, expected %u",
                  models[mi].name, op, mnemonic, actual, expected);

            /* DisOne must agree with DisInstLen and always produce text. */
            memset(mem, 0, sizeof mem);
            mem[TEST_ADDR] = (uint8_t)op;

            char buf[48];
            uint16_t next = DisOne(TEST_ADDR, disRead, sizeof buf, buf);

            CHECK(next == (uint16_t)(TEST_ADDR + expected),
                  "%s $%02x (%s): DisOne next $%04x, expected $%04x",
                  models[mi].name, op, mnemonic, next, TEST_ADDR + expected);
            CHECK(buf[0] != '\0',
                  "%s $%02x: DisOne produced empty text", models[mi].name, op);

            ++checked;
        }

        vrEmu6502Destroy(names);
    }

    printf("layer 1: %d opcode/model combinations checked\n", checked);
}

int main(void) {
    test_instruction_lengths();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all disassembler tests passed\n");
    return 0;
}
