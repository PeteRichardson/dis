#ifndef __HEXFILE_H__
#define __HEXFILE_H__
#include <stdint.h>
#include <stdio.h>
uint16_t readHexFile(const char* hexFilename, uint16_t* out_end);
#endif
