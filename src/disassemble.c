#include "disassemble.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * One CPU handle for the lifetime of the module.
 *
 * Mnemonic and addressing-mode lookup are pure functions of (model,
 * opcode), but the library only exposes them through a VrEmu6502*. The
 * handle is cheap -- vrEmu6502New mallocs a single struct whose largest
 * member is const char* mnemonicNames[256], around 1KB on a 32-bit
 * target. It allocates no address space; memory reads go through the
 * caller's DisReadFn, not through the emulator.
 */
static VrEmu6502* disCpu = NULL;

/* Never called during disassembly, but vrEmu6502New asserts they exist. */
static uint8_t disStubRead(uint16_t addr, bool isDbg) {
    (void)addr; (void)isDbg;
    return 0;
}

static void disStubWrite(uint16_t addr, uint8_t val) {
    (void)addr; (void)val;
}

/*
 * BBR0-7 and BBS0-7 take a zero-page byte *and* a relative byte, so they
 * are 3 bytes long. vrEmu6502AddrMode has no mode for that pairing, and
 * the opcode tables declare them as plain zero page -- see the {bbr0, zp,
 * 5} entries in _wdc65c02 in vrEmu6502.c. Taking the addressing mode at
 * face value would report length 2 and desynchronise the listing.
 *
 * Detected by mnemonic rather than opcode number so this stays correct
 * for whichever models happen to carry these instructions.
 */
static bool disIsBitBranch(const char* mnemonic) {
    return mnemonic != NULL
        && mnemonic[0] == 'b'
        && mnemonic[1] == 'b'
        && (mnemonic[2] == 'r' || mnemonic[2] == 's');
}

/*
 * BRK is a 2-byte instruction.
 *
 * The CPU pushes addr+2 as the return address, skipping the byte after
 * the opcode -- see brk() in vrEmu6502.c, which does push((++pc) >> 8).
 * That byte is conventionally a signature identifying which software
 * interrupt fired.
 *
 * The opcode tables declare BRK as implied, which would report length 1
 * and decode the signature byte as an instruction in its own right. A
 * signature of $01 would become "ora ($nn,x)", consuming two further real
 * bytes, and the listing would drift from what the CPU executes for the
 * rest of the range. A signature of $ea decodes as a harmless "nop",
 * which is what hides the problem in the built-in demo program.
 *
 * da65 and radare2's native plugin report 1; Capstone reports 2. There is
 * no consensus to defer to, so this follows the hardware: a disassembler
 * driving a debugger has to stay in step with the CPU.
 */
static bool disIsBrk(uint8_t opcode, const char* mnemonic) {
    return opcode == 0x00 && mnemonic != NULL && strcmp(mnemonic, "brk") == 0;
}

/*
 * Accumulator-mode instructions.
 *
 * vrEmu6502 never reports AddrModeAcc for anything: opcodeToAddrMode in
 * vrEmu6502.c has no case for the acc addressing function, so those
 * opcodes fall through to its closing "return AddrModeImp". Without this
 * the operand is dropped and "inc a" prints as "inc", which reads as an
 * absolute-mode instruction with a missing operand.
 *
 * The set is small and fixed. Mnemonics are checked too because $1a and
 * $3a are NOPs on NMOS parts and only become INC A / DEC A on CMOS.
 */
static bool disIsAccumulator(uint8_t opcode, const char* mnemonic) {
    if (!mnemonic) return false;

    switch (opcode) {
    case 0x0a: return strcmp(mnemonic, "asl") == 0;
    case 0x2a: return strcmp(mnemonic, "rol") == 0;
    case 0x4a: return strcmp(mnemonic, "lsr") == 0;
    case 0x6a: return strcmp(mnemonic, "ror") == 0;
    case 0x1a: return strcmp(mnemonic, "inc") == 0;
    case 0x3a: return strcmp(mnemonic, "dec") == 0;
    default:   return false;
    }
}

void DisInit(vrEmu6502Model model) {
    if (disCpu) vrEmu6502Destroy(disCpu);
    disCpu = vrEmu6502New(model, disStubRead, disStubWrite);
}

uint8_t DisInstLen(uint8_t opcode) {
    if (!disCpu) return 0;

    const char* mnemonic = vrEmu6502OpcodeToMnemonicStr(disCpu, opcode);

    if (disIsBitBranch(mnemonic)) return 3;
    if (disIsBrk(opcode, mnemonic)) return 2;

    switch (vrEmu6502GetOpcodeAddrMode(disCpu, opcode)) {
    case AddrModeAbs:
    case AddrModeAbsX:
    case AddrModeAbsY:
    case AddrModeAbsInd:
    case AddrModeAbsIndX:
        return 3;

    case AddrModeImm:
    case AddrModeIndX:
    case AddrModeIndY:
    case AddrModeRel:
    case AddrModeZP:
    case AddrModeZPI:
    case AddrModeZPX:
    case AddrModeZPY:
        return 2;

    case AddrModeAcc:
    case AddrModeImp:
        return 1;
    }

    return 1;
}

uint16_t DisOne(uint16_t addr, DisReadFn read, int bufSize, char* buf) {
    if (buf && bufSize > 0) buf[0] = '\0';
    if (!disCpu || !read) return 0;

    uint8_t opcode = read(addr);
    const char* mnemonic = vrEmu6502OpcodeToMnemonicStr(disCpu, opcode);
    uint8_t len = DisInstLen(opcode);

    /* Fetch only what this instruction actually uses, so a 1-byte opcode
       at the end of a range never reads past it. */
    uint8_t arg8 = (len >= 2) ? read((uint16_t)(addr + 1)) : 0;
    uint8_t arg8b = (len >= 3) ? read((uint16_t)(addr + 2)) : 0;
    uint16_t arg16 = (uint16_t)(((uint16_t)arg8b << 8) | arg8);

    /* The operand is built separately and composed in a single snprintf.
       Composing in place -- advancing a cursor by snprintf's return value
       -- overruns the buffer on truncation, because snprintf returns what
       it *would* have written, not what it did. */
    char operand[32];
    operand[0] = '\0';

    if (disIsBitBranch(mnemonic)) {
        /* zp, rel -- the branch is relative to the following instruction */
        snprintf(operand, sizeof operand, "$%02x, $%04x",
                 arg8, (uint16_t)(addr + 3 + (int8_t)arg8b));
    }
    else if (disIsAccumulator(opcode, mnemonic)) {
        snprintf(operand, sizeof operand, "a");
    }
    else if (disIsBrk(opcode, mnemonic)) {
        /* Show the signature byte: at a breakpoint it is the thing you
           actually want to read, and leaving it out would print a blank
           where a real byte of memory sits. */
        snprintf(operand, sizeof operand, "$%02x", arg8);
    }
    else switch (vrEmu6502GetOpcodeAddrMode(disCpu, opcode)) {
    case AddrModeAbs:
        snprintf(operand, sizeof operand, "$%04x", arg16);
        break;
    case AddrModeAbsX:
        snprintf(operand, sizeof operand, "$%04x, x", arg16);
        break;
    case AddrModeAbsY:
        snprintf(operand, sizeof operand, "$%04x, y", arg16);
        break;
    case AddrModeAbsInd:
        snprintf(operand, sizeof operand, "($%04x)", arg16);
        break;
    case AddrModeAbsIndX:
        snprintf(operand, sizeof operand, "($%04x, x)", arg16);
        break;
    case AddrModeImm:
        snprintf(operand, sizeof operand, "#$%02x", arg8);
        break;
    case AddrModeIndX:
        snprintf(operand, sizeof operand, "($%02x, x)", arg8);
        break;
    case AddrModeIndY:
        snprintf(operand, sizeof operand, "($%02x), y", arg8);
        break;
    case AddrModeRel:
        snprintf(operand, sizeof operand, "$%04x",
                 (uint16_t)(addr + 2 + (int8_t)arg8));
        break;
    case AddrModeZP:
        snprintf(operand, sizeof operand, "$%02x", arg8);
        break;
    case AddrModeZPI:
        snprintf(operand, sizeof operand, "($%02x)", arg8);
        break;
    case AddrModeZPX:
        snprintf(operand, sizeof operand, "$%02x, x", arg8);
        break;
    case AddrModeZPY:
        snprintf(operand, sizeof operand, "$%02x, y", arg8);
        break;
    case AddrModeAcc:
        snprintf(operand, sizeof operand, "a");
        break;
    case AddrModeImp:
        break;
    }

    if (buf && bufSize > 0) {
        if (operand[0])
            snprintf(buf, (size_t)bufSize, "%s %s", mnemonic, operand);
        else
            snprintf(buf, (size_t)bufSize, "%s", mnemonic);
    }

    return (uint16_t)(addr + len);
}

void DisRange(uint16_t addr, uint16_t len, DisReadFn read, DisEmitFn emit) {
    if (!disCpu || !read || !emit || len == 0) return;

    uint16_t pc = addr;
    uint32_t consumed = 0;

    while (consumed < len) {
        char text[48];
        uint8_t bytes[3];

        uint16_t next = DisOne(pc, read, sizeof text, text);

        /*
         * Deliberately not "next == 0": a 1-byte instruction at $ffff
         * returns 0 legitimately, which would drop the last line of a
         * listing that runs to the top of memory. The length is the
         * unambiguous check, and DisInit was already verified above.
         */
        uint8_t n = (uint8_t)(next - pc);
        if (n < 1 || n > 3) return;
        for (uint8_t i = 0; i < n; ++i)
            bytes[i] = read((uint16_t)(pc + i));

        emit(pc, bytes, n, text);
        consumed += n;

        /* A wrap past $FFFF ends the listing rather than restarting at 0. */
        if (next < pc) return;
        pc = next;
    }
}

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
