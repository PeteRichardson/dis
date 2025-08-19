#include "disassemble.h"
#include <stddef.h>

uint16_t Dis6502(
    vrEmu6502Model cpu_model,
    uint16_t addr,
    uint8_t opcode, uint8_t byte1, uint16_t byte2,
    int bufferSize, char* buffer
) {
    uint16_t *refAddr = NULL;
    char *labelMap = NULL;
    uint8_t arg8 = byte1;
    uint16_t arg16 = (byte2 << 8) | byte1;
    const char* mnemonic = vrEmu6502OpcodeToMnemonicStr(vr6502, opcode);

    const char* addr8Label = NULL;    // labelMap ? labelMap[arg8] : NULL;
    const char* addr16Label = NULL;   // labelMap ? labelMap[arg16] : NULL;

    int offset = snprintf(buffer, bufferSize, "%s ", mnemonic);
    buffer += offset;
    bufferSize -= offset;

    switch (vrEmu6502GetOpcodeAddrMode(vr6502, opcode)) {
    case AddrModeAbs:
      if (addr16Label)
        snprintf(buffer, bufferSize, "%s", addr16Label);
      else
        snprintf(buffer, bufferSize, "$%04x", arg16);
      if (refAddr) *refAddr = arg16;
      return addr + 3;

    case AddrModeAbsX:
      if (addr16Label)
        snprintf(buffer, bufferSize, "%s, x", addr16Label);
      else
        snprintf(buffer, bufferSize, "$%04x, x", arg16);
      if (refAddr) *refAddr = arg16 + vr6502->ix;
      return addr + 3;

    case AddrModeAbsY:
      if (addr16Label)
        snprintf(buffer, bufferSize, "%s, y", addr16Label);
      else
        snprintf(buffer, bufferSize, "$%04x, y", arg16);
      if (refAddr) *refAddr = arg16 + vr6502->iy;
      return addr + 3;

    case AddrModeImm:
      if (addr8Label)
        snprintf(buffer, bufferSize, "#%s", addr8Label);
      else
        snprintf(buffer, bufferSize, "#$%02x", arg8);
      if (refAddr) *refAddr = addr + 1;
      return addr + 2;

    case AddrModeAbsInd:
      if (addr16Label)
        snprintf(buffer, bufferSize, "(%s)", addr16Label);
      else
        snprintf(buffer, bufferSize, "($%04x)", arg16);
      if (refAddr) *refAddr = arg16;
      return addr + 3;

    case AddrModeAbsIndX:
      if (addr16Label)
        snprintf(buffer, bufferSize, "(%s, x)", addr16Label);
      else
        snprintf(buffer, bufferSize, "($%04x, x)", arg16);
      if (refAddr) *refAddr = arg16 + vr6502->ix;
      return addr + 3;

    case AddrModeIndX:
      if (addr8Label)
        snprintf(buffer, bufferSize, "(%s, x)", addr8Label);
      else
        snprintf(buffer, bufferSize, "($%02x, x)", arg8);
      if (refAddr) *refAddr = arg8 + vr6502->ix;
      return addr + 2;

    case AddrModeIndY:
      if (addr8Label)
        snprintf(buffer, bufferSize, "(%s), y", addr8Label);
      else
        snprintf(buffer, bufferSize, "($%02x), y", arg8);
      if (refAddr) *refAddr = arg8;
      return addr + 2;

    case AddrModeRel:
      if (labelMap && labelMap[addr + (int8_t)arg8 + 2])
        snprintf(buffer, bufferSize, "%s", labelMap[addr + (int8_t)arg8 + 2]);
      else
        snprintf(buffer, bufferSize, "$%04x", addr + (int8_t)arg8 + 2);
      if (refAddr) *refAddr = addr + (int8_t)arg8 + 2;
      return addr + 2;

    case AddrModeZP:
      if (addr8Label)
        snprintf(buffer, bufferSize, "%s", addr8Label);
      else
        snprintf(buffer, bufferSize, "$%02x", arg8);
      if (refAddr) *refAddr = arg8;
      return addr + 2;

    case AddrModeZPI:
      if (addr8Label)
        snprintf(buffer, bufferSize, "(%s)", addr8Label);
      else
        snprintf(buffer, bufferSize, "($%02x)", arg8);
      if (refAddr) *refAddr = arg8;
      return addr + 2;

    case AddrModeZPX:
      if (addr8Label)
        snprintf(buffer, bufferSize, "%s, x", addr8Label);
      else
        snprintf(buffer, bufferSize, "$%02x, x", arg8);
      if (refAddr) *refAddr = arg8 + vr6502->ix;
      return addr + 2;

    case AddrModeZPY:
      if (addr8Label)
        snprintf(buffer, bufferSize, "%s, y", addr8Label);
      else
        snprintf(buffer, bufferSize, "$%02x, y", arg8);
      if (refAddr) *refAddr = arg8 + vr6502->iy;
      return addr + 2;

    case AddrModeAcc:
    case AddrModeImp:
      return addr + 1;

    default:
      break;

    }
  return 0;
}
    