#include <stdio.h>
#include "vrEmu6502.h"
#include "memory.h"

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
