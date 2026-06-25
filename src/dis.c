#include <stdio.h>
#include "vrEmu6502.h"
#include "memory.h"
#include "hexfile.h"

static const uint8_t demo_image[] = {
    0x00,              // BRK
    0xea,              // NOP
    0xa9, 0x42,        // LDA #$42
    0xa5, 0x10,        // LDA $10
    0xad, 0x34, 0x12,  // LDA $1234
    0x4c, 0x00, 0x20   // JMP $2000
};

int main(int argc, char** argv) {
    char buffer[32];
    uint16_t base, end;

    MemSetDefaultFill(0x00);

    if (argc >= 2) {
        base = readHexFile(argv[1], &end);
        if (base == 0) return 1;
    } else {
        base = 0x0200;
        end  = base + (uint16_t)sizeof(demo_image) - 1;
        MapROM(base, demo_image, sizeof(demo_image));
    }
    MapResetVector(base);

    VrEmu6502* vr6502 = vrEmu6502New(CPU_65C02, MemRead, MemWrite);
    uint16_t pc = base;
    while (pc <= end) {
        uint16_t next_pc = vrEmu6502DisassembleInstruction(vr6502, pc, sizeof(buffer), buffer, NULL, NULL);

        uint8_t instr_len = (uint8_t)(next_pc - pc);
        if (instr_len == 0) break;

        printf("%04x: ", pc);

        uint8_t opcode     = MemRead(pc, 0);
        uint8_t next_byte  = MemRead(pc + 1, 0);
        uint8_t third_byte = MemRead(pc + 2, 0);

        if (instr_len == 1)
            printf("%02x        ", opcode);
        else if (instr_len == 2)
            printf("%02x %02x     ", opcode, next_byte);
        else if (instr_len == 3)
            printf("%02x %02x %02x  ", opcode, next_byte, third_byte);

        printf("%-10s\n", buffer);
        pc = next_pc;
    }

    vrEmu6502Destroy(vr6502);
    return 0;
}
