#include "memory.h"

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
