#include "analyze.h"

#include <stdlib.h>

/* Per-address flags. One byte per address is wasteful but trivial on the
   host; see the porting note in analyze.h. */
#define F_INSTRUCTION 0x01u  /* an instruction starts here */
#define F_FUNCTION    0x02u  /* entry point or JSR target */
#define F_LABEL       0x04u  /* branch or JMP target */
#define F_QUEUED      0x08u  /* already pushed; prevents requeueing */

struct DisAnalysis {
    DisReadFn read;
    uint16_t  lo, hi;

    uint8_t*  flags;    /* 0x10000 entries */
    uint16_t* callers;  /* 0x10000 entries, saturating count */

    uint16_t* work;     /* worklist */
    size_t    workLen, workCap;

    unsigned  outOfRange;
};

static bool inRange(const DisAnalysis* a, uint16_t addr) {
    return addr >= a->lo && addr <= a->hi;
}

/* Push addr unless already queued. Marking before pushing is what makes
   the traversal terminate: each address enters the worklist at most once. */
static void push(DisAnalysis* a, uint16_t addr) {
    if (!inRange(a, addr)) {
        ++a->outOfRange;
        return;
    }
    if (a->flags[addr] & F_QUEUED) return;

    if (a->workLen == a->workCap) {
        /* Measured peak depth: 68 on adventure.rp6502, 23 on rtc.rp6502
           (the largest real ROMs available), against this 256 initial
           capacity. The growth branch below is unexercised in practice. */
        size_t cap = a->workCap ? a->workCap * 2 : 256;
        uint16_t* grown = realloc(a->work, cap * sizeof *grown);
        if (!grown) return; /* out of memory: drop this target rather than crash */
        a->work = grown;
        a->workCap = cap;
    }

    a->flags[addr] |= F_QUEUED;
    a->work[a->workLen++] = addr;
}

DisAnalysis* AnalyzeNew(DisReadFn read, uint16_t lo, uint16_t hi) {
    if (!read || lo > hi) return NULL;

    DisAnalysis* a = calloc(1, sizeof *a);
    if (!a) return NULL;

    a->read = read;
    a->lo = lo;
    a->hi = hi;
    a->flags = calloc(0x10000, sizeof *a->flags);
    a->callers = calloc(0x10000, sizeof *a->callers);

    if (!a->flags || !a->callers) {
        AnalyzeFree(a);
        return NULL;
    }
    return a;
}

void AnalyzeFree(DisAnalysis* a) {
    if (!a) return;
    free(a->flags);
    free(a->callers);
    free(a->work);
    free(a);
}

void AnalyzeAddEntry(DisAnalysis* a, uint16_t addr) {
    if (!a || !inRange(a, addr)) return;
    a->flags[addr] |= F_FUNCTION;
    push(a, addr);
}

void AnalyzeRun(DisAnalysis* a) {
    if (!a) return;

    while (a->workLen) {
        uint16_t pc = a->work[--a->workLen];

        for (;;) {
            if (!inRange(a, pc)) break;
            if (a->flags[pc] & F_INSTRUCTION) break; /* already traced */

            uint8_t len = DisInstLen(a->read(pc));
            if (len == 0) break;

            a->flags[pc] |= F_INSTRUCTION;

            uint16_t target = 0;
            DisFlow flow = DisFlowOf(pc, a->read, &target);

            if (flow == DisFlowCall) {
                if (inRange(a, target)) {
                    a->flags[target] |= F_FUNCTION;
                    if (a->callers[target] < 0xffff) ++a->callers[target];
                }
                push(a, target);
            }
            else if (flow == DisFlowJump || flow == DisFlowBranch) {
                if (inRange(a, target)) a->flags[target] |= F_LABEL;
                push(a, target);
            }

            if (flow == DisFlowJump || flow == DisFlowReturn ||
                flow == DisFlowIndirect)
                break; /* nothing falls through */

            uint16_t next = (uint16_t)(pc + len);
            if (next < pc) break; /* wrapped past $ffff */
            pc = next;
        }
    }
}

bool AnalyzeIsInstruction(const DisAnalysis* a, uint16_t addr) {
    return a && (a->flags[addr] & F_INSTRUCTION) != 0;
}

bool AnalyzeIsFunction(const DisAnalysis* a, uint16_t addr) {
    return a && (a->flags[addr] & F_FUNCTION) != 0;
}

bool AnalyzeIsLabel(const DisAnalysis* a, uint16_t addr) {
    return a && (a->flags[addr] & F_LABEL) != 0;
}

unsigned AnalyzeCallers(const DisAnalysis* a, uint16_t addr) {
    return a ? a->callers[addr] : 0;
}

unsigned AnalyzeOutOfRange(const DisAnalysis* a) {
    return a ? a->outOfRange : 0;
}
