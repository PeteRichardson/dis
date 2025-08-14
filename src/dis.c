#include <stdio.h>
#include "vrEmu6502.h"
#include "memory.h"


void BuildMemory(const uint16_t program_addr, const uint16_t program_len, const uint8_t program_image[]) {
    // Unmapped bytes read as 0x00 (optional; it's already default)
    MemSetDefaultFill(0x00);

    // Map the program as read-only
    MapROM(program_addr, program_image, program_len);

    // Provide a RESET vector pointing at program start
    MapResetVector(program_addr);

    // If you want the whole zero page to behave like RAM you can add:
    // static uint8_t zp[0x0100] = {0};
    // MapRAM(0x0000, zp, sizeof(zp));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char buffer[32];

    const uint16_t program_addr = 0x0200;
    const uint16_t program_len = 0x0C;
    const uint8_t program_image[] = {
        0x00,              // BRK
        0xea,              // NOP   
        0xa9, 0x42,        // LDA #$42
        0xa5, 0x10,        // LDA #$42
        0xad, 0x34, 0x12,  // LDA $1234
        0x4c, 0x00, 0x20   // JMP $2000  
    };

    BuildMemory(program_addr, program_len, program_image);

    DumpRegions();

    vrEmu6502Model cpuModel = CPU_65C02;

    VrEmu6502* vr6502 = vrEmu6502New(cpuModel, MemRead, MemWrite);
    uint16_t base = 0x200;
    uint16_t pc = base;
    while (pc < base + program_len) {
        uint8_t next_pc_offset = vrEmu6502DisassembleInstruction(vr6502, pc, sizeof(buffer), buffer, NULL, NULL);
        // address and instruction
        printf("%04x:  %-10s", pc, buffer);

        // hex bytes
        uint8_t opcode = MemRead(pc, 0);
        uint8_t next_byte = MemRead(pc + 1, 0);
        uint8_t third_byte = MemRead(pc + 2, 0);

        uint8_t instr_len = base + next_pc_offset - pc;
        if (instr_len == 1) {
            printf("; %02x\n", opcode);
        }
        else if (instr_len == 2) {
            printf("; %02x %01x\n", opcode, next_byte);
        }
        else if (instr_len == 3) {
            printf("; %02x %02x %02x\n", opcode, next_byte, third_byte);
        }
        pc = pc + instr_len;
    }
}
