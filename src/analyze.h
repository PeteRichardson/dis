#ifndef __ANALYZE_H__
#define __ANALYZE_H__

#include <stdbool.h>
#include <stdint.h>

#include "disassemble.h"

/*
 * Recursive-descent (control-flow) analysis.
 *
 * Seeded with entry points, the traversal follows control flow and marks
 * every byte it reaches. Whatever is never reached is data. This yields
 * code/data separation, function entry points, and function ends from one
 * pass.
 *
 * Known blind spots, inherent to the technique on 6502 rather than defects
 * to be fixed later:
 *
 *   - JSR followed by inline data: the traversal walks into the data and
 *     decodes it as code
 *   - jump tables and the RTS dispatch trick: real code looks unreachable
 *   - self-modifying code
 *
 * Callers must therefore show unreached bytes rather than dropping them,
 * so a reader can see where the analysis disagreed with them.
 *
 * Host-side for now. The interface is callback-driven so a Pico port is
 * mechanical: it would pack the flags array into an 8KB bitmap plus a
 * small function table, instead of the flat 64KB array used here.
 */

typedef struct DisAnalysis DisAnalysis;

/*
 * Create an analysis over the inclusive address range [lo,hi]. Targets
 * outside that range are counted but not followed, so a wild branch
 * cannot walk into unmapped memory. Returns NULL on allocation failure.
 *
 * DisInit must have been called first: classification depends on the CPU
 * model.
 */
DisAnalysis* AnalyzeNew(DisReadFn read, uint16_t lo, uint16_t hi);

/*
 * Seed a starting address -- typically the RESET, NMI and IRQ vectors.
 * Entry points are recorded as functions. Addresses outside [lo,hi] are
 * ignored. Call before AnalyzeRun.
 */
void AnalyzeAddEntry(DisAnalysis* a, uint16_t addr);

/* Run the traversal to completion. Terminates on any input. */
void AnalyzeRun(DisAnalysis* a);

void AnalyzeFree(DisAnalysis* a);

/* True if an instruction *starts* at addr. Operand bytes are false. */
bool AnalyzeIsInstruction(const DisAnalysis* a, uint16_t addr);

/* True if addr is an entry point or the target of a JSR. */
bool AnalyzeIsFunction(const DisAnalysis* a, uint16_t addr);

/* True if addr is the target of a branch or JMP. */
bool AnalyzeIsLabel(const DisAnalysis* a, uint16_t addr);

/* How many distinct JSR sites target addr. */
unsigned AnalyzeCallers(const DisAnalysis* a, uint16_t addr);

/* How many control-flow targets fell outside [lo,hi] and were not followed. */
unsigned AnalyzeOutOfRange(const DisAnalysis* a);

#endif
