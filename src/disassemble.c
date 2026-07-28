#include "disassemble.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

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

void DisInit(vrEmu6502Model model) {
    if (disCpu) vrEmu6502Destroy(disCpu);
    disCpu = vrEmu6502New(model, disStubRead, disStubWrite);
}

uint8_t DisInstLen(uint8_t opcode) {
    if (!disCpu) return 0;

    if (disIsBitBranch(vrEmu6502OpcodeToMnemonicStr(disCpu, opcode))) return 3;

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
        if (next == 0) return;

        uint8_t n = (uint8_t)(next - pc);
        for (uint8_t i = 0; i < n; ++i)
            bytes[i] = read((uint16_t)(pc + i));

        emit(pc, bytes, n, text);
        consumed += n;

        /* A wrap past $FFFF ends the listing rather than restarting at 0. */
        if (next < pc) return;
        pc = next;
    }
}
