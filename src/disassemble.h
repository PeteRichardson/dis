#ifndef __DISASSEMBLE_H__
#define __DISASSEMBLE_H__

#include <stdint.h>
#include "vrEmu6502.h"

/*
 * Fetch one byte for disassembly.
 *
 * Must be free of side effects: DisRange may read up to 2 bytes past the
 * end of the requested range when the last instruction straddles it.
 */
typedef uint8_t (*DisReadFn)(uint16_t addr);

/*
 * Receives one disassembled instruction.
 *
 * bytes points at len raw opcode bytes; text is the NUL-terminated
 * mnemonic and operand. Neither pointer is valid after the call returns.
 */
typedef void (*DisEmitFn)(uint16_t addr, const uint8_t* bytes,
                          uint8_t len, const char* text);

/*
 * Select the CPU variant. Must be called before DisOne or DisRange.
 * Safe to call repeatedly to switch models.
 */
void DisInit(vrEmu6502Model model);

/*
 * Byte length of the instruction starting with opcode: 1, 2 or 3.
 * Returns 0 if DisInit has not been called.
 */
uint8_t DisInstLen(uint8_t opcode);

/*
 * Disassemble the instruction at addr.
 *
 * Writes at most bufSize bytes to buf, always NUL-terminated when
 * bufSize > 0. Text longer than the buffer is truncated, not an error --
 * the returned address is still correct so a listing can continue.
 *
 * Returns the address of the next instruction, or 0 if DisInit has not
 * been called. Note 0 is ambiguous: a 1-byte instruction at $FFFF also
 * returns 0. Callers needing to tell them apart should check buf, which
 * is left empty only on failure.
 */
uint16_t DisOne(uint16_t addr, DisReadFn read, int bufSize, char* buf);

/*
 * Disassemble instructions from addr until len bytes have been consumed,
 * passing each to emit.
 *
 * len bounds where instructions may *start*. An instruction beginning
 * inside the range is decoded in full even if its operands fall outside,
 * so reads may run up to 2 bytes past addr + len. A len of 0 emits
 * nothing. Ranges wrapping past $FFFF stop at the wrap.
 */
void DisRange(uint16_t addr, uint16_t len, DisReadFn read, DisEmitFn emit);

/*
 * What an instruction does to control flow. Used to drive a
 * recursive-descent traversal; see analyze.c.
 */
typedef enum {
    DisFlowNormal,   /* falls through to the next instruction */
    DisFlowCall,     /* JSR: *target is a function entry, execution resumes after */
    DisFlowJump,     /* unconditional: *target is queued, this trace ends */
    DisFlowBranch,   /* conditional: *target is queued AND this trace continues */
    DisFlowReturn,   /* RTS/RTI/BRK/STP/JAM: this trace ends, no target */
    DisFlowIndirect, /* JMP (abs) / JMP (abs,x): trace ends, target unknowable */
} DisFlow;

/*
 * Classify the instruction at addr. *target is written only for Call,
 * Jump and Branch, and is left untouched otherwise.
 *
 * Returns DisFlowNormal if DisInit has not been called.
 */
DisFlow DisFlowOf(uint16_t addr, DisReadFn read, uint16_t* target);

#endif
