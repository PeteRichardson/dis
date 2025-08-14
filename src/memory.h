#ifndef __MEMORY_H__
#define __MEMORY_H__
#include <stdint.h>
#include <stdbool.h>

uint8_t MemRead(uint16_t addr, bool isDbg);
void MemWrite(uint16_t addr, uint8_t val);
bool MapROM(uint16_t addr, const uint8_t* data, uint16_t len);
bool MapRAM(uint16_t addr, uint8_t* storage, uint16_t len);
bool MapFill(uint16_t addr, uint16_t len, uint8_t fill);
bool MapResetVector(uint16_t entry);
void MemSetDefaultFill(uint8_t fill);
void DumpRegions(void);
#endif
