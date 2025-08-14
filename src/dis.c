#include <stdio.h>
#include "vrEmu6502.h"

uint8_t ram[0x10000] = {
    0xa0, 0x01,
    0x84, 0x0b,
    0xa9, 0x00,
    0x85, 0x00,
    0x85, 0x01,
    0xa5, 0x01,
    0x29, 0x0f
};

uint8_t MemRead(uint16_t addr, bool isDbg) {
    (void)isDbg;
    return ram[addr];
}

void MemWrite(uint16_t addr, uint8_t val) {
    ram[addr] = val;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char buffer[32];

    vrEmu6502Model cpuModel = CPU_65C02;

    VrEmu6502* vr6502 = vrEmu6502New(cpuModel, MemRead, MemWrite);
    uint16_t pc = 0x0;
    while (pc < 11) {
        pc = vrEmu6502DisassembleInstruction(vr6502, pc, sizeof(buffer), buffer, NULL, NULL);
        printf("%s\n", buffer);
    }
}
