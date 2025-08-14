#ifndef __MEMORY_H__
#define __MEMORY_H__
#include <stdint.h>
#include <stdbool.h>

uint8_t MemRead(uint16_t addr, bool isDbg);
void MemWrite(uint16_t addr, uint8_t val);

#endif
