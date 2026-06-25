#ifndef __DISASSEMBLE_H__
#define __DISASSEMBLE_H__

#include <stdint.h>
#include "vrEmu6502.h"

uint16_t Dis6502(
    VrEmu6502* vr6502,
    uint16_t addr,
    uint8_t opcode, uint8_t byte1, uint16_t byte2,
    int bufferSize, char* buffer
);

#endif