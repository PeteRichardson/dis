#ifndef __DISASSEMBLE_H__
#define __DISASSEMBLE_H__

#include <stdint.h>

typedef enum {
    CPU_6502,     /* NMOS 6502/6510 with documented opcodes only */
    CPU_6502U,    /* NMOS 6502/6510 with undocumented opcodes */
    CPU_65C02,    /* Standard CMOS 65C02 */
    CPU_W65C02,   /* Western Design Centre CMOS 65C02 */
    CPU_R65C02,   /* Rockwell CMOS 65C02 */
    CPU_6510 = CPU_6502U,
    CPU_8500 = CPU_6510,
    CPU_8502 = CPU_8500,
    CPU_7501 = CPU_6502,
    CPU_8501 = CPU_6502
} vrEmu6502Model;

uint16_t Dis6502(
    vrEmu6502Model cpu_model,
    uint16_t addr,
    uint8_t opcode, uint8_t byte1, uint16_t byte2,
    int bufferSize, char* buffer
);

#endif