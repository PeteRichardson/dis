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
    /* 2, not 1: the CPU pushes addr+2 and skips the signature byte. The
       oracle cannot see this because the PC jumps to the IRQ vector, but
       brk() in vrEmu6502.c does push((++pc) >> 8), which is the same
       claim. See disIsBrk in disassemble.c. */
    { 0x00, "brk", 2, "PC jumps to the IRQ vector" },
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

/* ------------------------------------------------------------------
 * LAYER 2 -- golden strings
 *
 * Layer 1 proves lengths but says nothing about format: it cannot tell
 * "lda $1234, x" from "lda 0x1234,X". This table pins the text.
 *
 * The NMOS entries were cross-checked once, offline, against radare2,
 * which is a valid reference for the documented NMOS opcode set (it is
 * NOT for 65C02 -- r2 decodes those as NMOS illegals and desynchronises).
 * Regenerate the reference with:
 *
 *   rasm2 -a 6502 -b 8 -d "ea0aa942a510b510b610ad3412bd3412b93412\
 *                          6c3412a110b11020341200"
 *   r2 -a 6502 -b 8 -qc 's 0x1000; wx d005d0fe; pd 2' malloc://8192
 *
 * r2 renders the same mnemonics, operand values and addressing-mode
 * shapes; only the numeric syntax differs ($ vs 0x, spacing before index
 * registers). The 65C02 and W65C02 entries are from the W65C02S data
 * sheet and hand-audited -- no tool available here decodes them.
 *
 * All cases are assembled at $1000, so branch targets are $1000 + len +
 * displacement.
 */

static const struct {
    vrEmu6502Model model;
    uint8_t        bytes[3];
    uint8_t        len;
    const char*    text;
} golden[] = {
    /* --- NMOS, cross-checked against radare2 --- */
    { CPU_6502, { 0xea },             1, "nop" },            /* implied */
    { CPU_6502, { 0x0a },             1, "asl a" },          /* accumulator */
    { CPU_6502, { 0xa9, 0x42 },       2, "lda #$42" },       /* immediate */
    { CPU_6502, { 0xa5, 0x10 },       2, "lda $10" },        /* zero page */
    { CPU_6502, { 0xb5, 0x10 },       2, "lda $10, x" },     /* zero page,x */
    { CPU_6502, { 0xb6, 0x10 },       2, "ldx $10, y" },     /* zero page,y */
    { CPU_6502, { 0xad, 0x34, 0x12 }, 3, "lda $1234" },      /* absolute */
    { CPU_6502, { 0xbd, 0x34, 0x12 }, 3, "lda $1234, x" },   /* absolute,x */
    { CPU_6502, { 0xb9, 0x34, 0x12 }, 3, "lda $1234, y" },   /* absolute,y */
    { CPU_6502, { 0x6c, 0x34, 0x12 }, 3, "jmp ($1234)" },    /* indirect */
    { CPU_6502, { 0xa1, 0x10 },       2, "lda ($10, x)" },   /* indexed indirect */
    { CPU_6502, { 0xb1, 0x10 },       2, "lda ($10), y" },   /* indirect indexed */
    { CPU_6502, { 0x20, 0x34, 0x12 }, 3, "jsr $1234" },
    /*
     * Deliberately disagrees with radare2, which reports BRK as 1 byte
     * (as does da65; Capstone reports 2). The CPU skips the signature
     * byte, so a debugger listing that reports 1 decodes it as an
     * instruction and drifts. Do not "correct" this to match r2.
     */
    { CPU_6502, { 0x00, 0xea },       2, "brk $ea" },
    { CPU_6502, { 0xd0, 0x05 },       2, "bne $1007" },      /* relative, forward */
    { CPU_6502, { 0xd0, 0xfe },       2, "bne $1000" },      /* relative, to self */

    /* --- 65C02 additions, from the data sheet --- */
    { CPU_65C02, { 0xb2, 0x10 },       2, "lda ($10)" },      /* zero page indirect */
    { CPU_65C02, { 0x7c, 0x34, 0x12 }, 3, "jmp ($1234, x)" }, /* absolute indexed indirect */
    { CPU_65C02, { 0x80, 0x05 },       2, "bra $1007" },
    { CPU_65C02, { 0xda },             1, "phx" },
    { CPU_65C02, { 0x5a },             1, "phy" },
    { CPU_65C02, { 0x64, 0x10 },       2, "stz $10" },
    { CPU_65C02, { 0x9c, 0x34, 0x12 }, 3, "stz $1234" },
    { CPU_65C02, { 0x14, 0x20 },       2, "trb $20" },
    { CPU_65C02, { 0x04, 0x30 },       2, "tsb $30" },
    /* inc a / dec a rendered as "inc " before AddrModeAcc was fixed */
    { CPU_65C02, { 0x1a },             1, "inc a" },
    { CPU_65C02, { 0x3a },             1, "dec a" },

    /* --- W65C02 bit operations, the RP6502's actual CPU --- */
    { CPU_W65C02, { 0x07, 0x10 },       2, "rmb0 $10" },
    { CPU_W65C02, { 0x87, 0x10 },       2, "smb0 $10" },
    /* 3-byte zp,rel: target is $1000 + 3 + displacement */
    { CPU_W65C02, { 0x0f, 0x12, 0x05 }, 3, "bbr0 $12, $1008" },
    { CPU_W65C02, { 0x8f, 0x12, 0xfd }, 3, "bbs0 $12, $1000" },
};

static void test_golden_strings(void) {
    for (unsigned i = 0; i < sizeof golden / sizeof golden[0]; ++i) {
        DisInit(golden[i].model);

        memset(mem, 0, sizeof mem);
        memcpy(&mem[TEST_ADDR], golden[i].bytes, golden[i].len);

        char buf[48];
        uint16_t next = DisOne(TEST_ADDR, disRead, sizeof buf, buf);

        CHECK(strcmp(buf, golden[i].text) == 0,
              "golden[%u]: got \"%s\", expected \"%s\"", i, buf, golden[i].text);
        CHECK(next == (uint16_t)(TEST_ADDR + golden[i].len),
              "golden[%u] (%s): next $%04x, expected $%04x",
              i, golden[i].text, next, TEST_ADDR + golden[i].len);
    }

    printf("layer 2: %zu golden strings checked\n",
           sizeof golden / sizeof golden[0]);
}

/* ------------------------------------------------------------------
 * LAYER 3 -- edge cases
 */

/*
 * Truncation must never overrun. The original code advanced a cursor by
 * snprintf's return value, which is the length it *would* have written --
 * so on truncation the cursor moved past the end of the buffer and the
 * remaining size went negative before being passed back in as a size_t.
 *
 * Every buffer size from 0 up is checked, with a canary filling the bytes
 * past the buffer.
 */
#define CANARY 0xa5

static void test_buffer_truncation(void) {
    DisInit(CPU_65C02);

    memset(mem, 0, sizeof mem);
    mem[TEST_ADDR + 0] = 0xbd;  /* lda $1234, x -- 12 chars, a long one */
    mem[TEST_ADDR + 1] = 0x34;
    mem[TEST_ADDR + 2] = 0x12;

    const char* full = "lda $1234, x";

    for (int size = 0; size <= (int)strlen(full) + 4; ++size) {
        char buf[64];
        memset(buf, CANARY, sizeof buf);

        uint16_t next = DisOne(TEST_ADDR, disRead, size, buf);

        /* A clipped line must not stop the listing. */
        CHECK(next == (uint16_t)(TEST_ADDR + 3),
              "truncation size %d: next $%04x, expected $%04x",
              size, next, TEST_ADDR + 3);

        bool clean = true;
        for (int i = size; i < (int)sizeof buf; ++i)
            if ((unsigned char)buf[i] != CANARY) clean = false;
        CHECK(clean, "truncation size %d: wrote past the end of the buffer", size);

        if (size == 0) continue;

        CHECK(memchr(buf, '\0', (size_t)size) != NULL,
              "truncation size %d: no NUL within the buffer", size);
        CHECK(strncmp(buf, full, (size_t)size - 1) == 0,
              "truncation size %d: got \"%s\", expected a prefix of \"%s\"",
              size, buf, full);
    }

    printf("layer 3: buffer truncation checked\n");
}

/* Branch targets are computed in 16-bit space and must wrap, not clamp. */
static void test_relative_wrap(void) {
    DisInit(CPU_6502);

    static const struct {
        uint16_t    addr;
        uint8_t     disp;
        const char* text;
    } cases[] = {
        { 0xfffe, 0x05, "bne $0005" }, /* forward past the top of memory */
        { 0x0000, 0xfe, "bne $0000" }, /* backward to itself */
        { 0x0000, 0xf0, "bne $fff2" }, /* backward past the bottom */
        { 0x1000, 0x7f, "bne $1081" }, /* largest forward displacement */
        { 0x1000, 0x80, "bne $0f82" }, /* largest backward displacement */
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(mem, 0, sizeof mem);
        mem[cases[i].addr] = 0xd0;
        mem[(uint16_t)(cases[i].addr + 1)] = cases[i].disp;

        char buf[48];
        DisOne(cases[i].addr, disRead, sizeof buf, buf);

        CHECK(strcmp(buf, cases[i].text) == 0,
              "rel wrap at $%04x disp $%02x: got \"%s\", expected \"%s\"",
              cases[i].addr, cases[i].disp, buf, cases[i].text);
    }

    printf("layer 3: relative branch wrapping checked\n");
}

/* ---- DisRange ---- */

#define MAX_EMIT 16

static struct {
    uint16_t addr;
    uint8_t  bytes[3];
    uint8_t  len;
    char     text[48];
} emitted[MAX_EMIT];

static int emitCount;

static void captureEmit(uint16_t addr, const uint8_t* bytes,
                        uint8_t len, const char* text) {
    if (emitCount >= MAX_EMIT) return;
    emitted[emitCount].addr = addr;
    emitted[emitCount].len = len;
    memcpy(emitted[emitCount].bytes, bytes, len);
    snprintf(emitted[emitCount].text, sizeof emitted[emitCount].text, "%s", text);
    ++emitCount;
}

static void test_range(void) {
    DisInit(CPU_65C02);

    /* Same instruction sequence as the built-in demo. */
    static const uint8_t image[] = {
        0x00,             /* brk       */
        0xea,             /* nop       */
        0xa9, 0x42,       /* lda #$42  */
        0xa5, 0x10,       /* lda $10   */
        0xad, 0x34, 0x12, /* lda $1234 */
        0x4c, 0x00, 0x20  /* jmp $2000 */
    };

    memset(mem, 0, sizeof mem);
    memcpy(&mem[TEST_ADDR], image, sizeof image);

    emitCount = 0;
    DisRange(TEST_ADDR, sizeof image, disRead, captureEmit);

    /* Five, not six: BRK absorbs the 0xea after it as its signature byte
       rather than leaving it to decode as a separate NOP. */
    CHECK(emitCount == 5, "range: %d instructions emitted, expected 5", emitCount);
    if (emitCount == 5) {
        CHECK(emitted[0].addr == TEST_ADDR && emitted[0].len == 2
                  && strcmp(emitted[0].text, "brk $ea") == 0,
              "range[0]: $%04x len %u \"%s\"",
              emitted[0].addr, emitted[0].len, emitted[0].text);
        CHECK(emitted[1].len == 2 && strcmp(emitted[1].text, "lda #$42") == 0,
              "range[1]: len %u \"%s\"", emitted[1].len, emitted[1].text);
        CHECK(emitted[4].addr == TEST_ADDR + 9 && emitted[4].len == 3
                  && strcmp(emitted[4].text, "jmp $2000") == 0,
              "range[4]: $%04x len %u \"%s\"",
              emitted[4].addr, emitted[4].len, emitted[4].text);
        /* Raw bytes must reach the callback intact for the hex column. */
        CHECK(emitted[4].bytes[0] == 0x4c && emitted[4].bytes[1] == 0x00
                  && emitted[4].bytes[2] == 0x20,
              "range[4]: raw bytes %02x %02x %02x",
              emitted[4].bytes[0], emitted[4].bytes[1], emitted[4].bytes[2]);
    }

    /* A zero-length range emits nothing. */
    emitCount = 0;
    DisRange(TEST_ADDR, 0, disRead, captureEmit);
    CHECK(emitCount == 0, "range len 0: %d emitted, expected 0", emitCount);

    /*
     * Straddling the end: len bounds where instructions may start, so a
     * 3-byte JMP beginning on the last in-range byte is decoded in full.
     */
    emitCount = 0;
    DisRange((uint16_t)(TEST_ADDR + 9), 1, disRead, captureEmit);
    CHECK(emitCount == 1 && emitted[0].len == 3
              && strcmp(emitted[0].text, "jmp $2000") == 0,
          "range straddle: %d emitted, len %u, \"%s\"",
          emitCount, emitted[0].len, emitted[0].text);

    /* Wrapping past $FFFF ends the listing rather than restarting at 0. */
    memset(mem, 0, sizeof mem);
    mem[0xffff] = 0xea;   /* nop */
    mem[0x0000] = 0xea;   /* would be next if the address wrapped */

    emitCount = 0;
    DisRange(0xffff, 4, disRead, captureEmit);
    CHECK(emitCount == 1 && emitted[0].addr == 0xffff,
          "range wrap: %d emitted at $%04x, expected 1 at $ffff",
          emitCount, emitCount ? emitted[0].addr : 0);

    /* Operand reads past $FFFF wrap rather than running off the buffer. */
    memset(mem, 0, sizeof mem);
    mem[0xffff] = 0xad;   /* lda abs -- operands live at $0000/$0001 */
    mem[0x0000] = 0x34;
    mem[0x0001] = 0x12;

    char buf[48];
    DisOne(0xffff, disRead, sizeof buf, buf);
    CHECK(strcmp(buf, "lda $1234") == 0,
          "operand read wrap: got \"%s\", expected \"lda $1234\"", buf);

    printf("layer 3: DisRange boundaries checked\n");
}

int main(void) {
    test_instruction_lengths();
    test_golden_strings();
    test_buffer_truncation();
    test_relative_wrap();
    test_range();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all disassembler tests passed\n");
    return 0;
}
