#include <stdio.h>
#include <string.h>
#include "vrEmu6502.h"
#include "memory.h"
#include "hexfile.h"
#include "rp6502file.h"
#include "disassemble.h"
#include "analyze.h"

static const uint8_t demo_image[] = {
    0x00,              // BRK
    0xea,              // NOP
    0xa9, 0x42,        // LDA #$42
    0xa5, 0x10,        // LDA $10
    0xad, 0x34, 0x12,  // LDA $1234
    0x4c, 0x00, 0x20   // JMP $2000
};

/*
 * The disassembler reaches memory only through this callback, so the
 * sparse model in memory.c stays a host-side concern. On the RP6502 the
 * same DisRange call is driven by the RIA's own memory access instead.
 *
 * isDbg is true: a disassembler must not disturb device state.
 */
static uint8_t readByte(uint16_t addr) {
    return MemRead(addr, true);
}

static void printInstruction(uint16_t addr, const uint8_t* bytes,
                             uint8_t len, const char* text) {
    printf("%04x: ", addr);

    switch (len) {
    case 1:  printf("%02x        ", bytes[0]); break;
    case 2:  printf("%02x %02x     ", bytes[0], bytes[1]); break;
    default: printf("%02x %02x %02x  ", bytes[0], bytes[1], bytes[2]); break;
    }

    printf("%-10s\n", text);
}

/*
 * Function-organized listing. Walks the range in address order, emitting a
 * label before any address that is a function or branch target, and
 * collapsing runs of unreached bytes to a single marker.
 *
 * Unreached bytes are shown rather than dropped: the traversal has known
 * blind spots (see analyze.h), so a reader must be able to see where it
 * disagreed with them.
 */
static void printAnalyzed(const DisAnalysis* a, uint16_t lo, uint16_t hi,
                          uint16_t entry) {
    uint32_t addr = lo;

    while (addr <= hi) {
        if (!AnalyzeIsInstruction(a, (uint16_t)addr)) {
            uint32_t start = addr;
            while (addr <= hi && !AnalyzeIsInstruction(a, (uint16_t)addr))
                ++addr;
            printf("\n; %04x-%04x  %u bytes data\n",
                   (unsigned)start, (unsigned)(addr - 1),
                   (unsigned)(addr - start));
            continue;
        }

        uint16_t at = (uint16_t)addr;

        if (AnalyzeIsFunction(a, at)) {
            unsigned callers = AnalyzeCallers(a, at);
            printf("\nsub_%04x:", at);
            if (at == entry)      printf("                  ; entry point");
            else if (callers == 1) printf("                  ; 1 caller");
            else                   printf("                  ; %u callers", callers);
            printf("\n");
        }
        else if (AnalyzeIsLabel(a, at)) {
            printf("loc_%04x:\n", at);
        }

        char text[48];
        uint16_t next = DisOne(at, readByte, sizeof text, text);
        uint8_t len = (uint8_t)(next - at);
        if (len == 0 || len > 3) break;

        uint8_t bytes[3];
        for (uint8_t i = 0; i < len; ++i)
            bytes[i] = readByte((uint16_t)(at + i));

        printInstruction(at, bytes, len, text);

        addr += len;
    }
}

static const struct {
    const char*    name;
    vrEmu6502Model model;
} cpuNames[] = {
    { "6502",   CPU_6502   },
    { "6502u",  CPU_6502U  },
    { "65c02",  CPU_65C02  },
    { "w65c02", CPU_W65C02 },
    { "r65c02", CPU_R65C02 },
};

static int parseCpu(const char* name, vrEmu6502Model* out) {
    for (unsigned i = 0; i < sizeof cpuNames / sizeof cpuNames[0]; ++i) {
        if (strcmp(cpuNames[i].name, name) == 0) {
            *out = cpuNames[i].model;
            return 1;
        }
    }
    return 0;
}

static void usage(FILE* to) {
    fprintf(to, "usage: dis [--cpu MODEL] [program]\n");
    fprintf(to, "  MODEL:   6502, 6502u, 65c02 (default), w65c02, r65c02\n");
    fprintf(to, "  program: an Intel HEX file or an RP6502 ROM (.rp6502);\n");
    fprintf(to, "           the format is detected from the contents\n");
    fprintf(to, "  with no file, disassembles a small built-in demo program\n");
    fprintf(to, "  --linear: flat sweep instead of following control flow\n");
}

int main(int argc, char** argv) {
    /* The RP6502's RIA is a W65C02S, but 65C02 stays the default so
       existing output is unchanged. */
    vrEmu6502Model model = CPU_65C02;
    const char* inputPath = NULL;
    bool linear = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cpu") == 0) {
            if (++i >= argc || !parseCpu(argv[i], &model)) {
                fprintf(stderr, "dis: --cpu needs one of: "
                                "6502, 6502u, 65c02, w65c02, r65c02\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        else if (strcmp(argv[i], "--linear") == 0) {
            linear = true;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "dis: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        }
        else {
            inputPath = argv[i];
        }
    }

    MemSetDefaultFill(0x00);

    uint16_t base, end;
    bool haveEntry = false;
    if (inputPath) {
        /*
         * Both readers write through MemWrite, which silently drops writes
         * to unmapped addresses (memory.c). Without a RAM region
         * underneath, every loaded byte is discarded and the file
         * disassembles as a run of BRKs.
         *
         * The load extent is not known until the file is parsed, so back
         * the whole address space. Host-side only; the RIA supplies its
         * own memory and never links memory.c.
         */
        static uint8_t image[0xffff];
        MapRAM(0x0000, image, sizeof image);

        /* Sniff the contents rather than the extension, so a ROM saved
           under any name is still recognised. */
        if (isRp6502File(inputPath)) {
            Rp6502Rom rom;
            if (!readRp6502File(inputPath, &rom)) return 1;

            base = rom.entry;
            end = rom.end;
            haveEntry = true;

            /* Notes go to stderr so the listing itself stays pipeable. */
            if (rom.xramChunks)
                fprintf(stderr, "note: skipped %u XRAM chunk%s (%u bytes); "
                                "XRAM is not 6502 address space\n",
                        rom.xramChunks, rom.xramChunks == 1 ? "" : "s",
                        (unsigned)rom.xramBytes);
            if (rom.hasResetVector)
                fprintf(stderr, "note: starting at reset vector $%04x\n", base);
        }
        else {
            base = readHexFile(inputPath, &end);
            if (base == 0) return 1;
            haveEntry = true;
        }
    }
    else {
        base = 0x0200;
        end = base + (uint16_t)sizeof(demo_image) - 1;
        MapROM(base, demo_image, sizeof(demo_image));
    }

    DisInit(model);

    if (linear || !haveEntry) {
        DisRange(base, (uint16_t)(end - base + 1), readByte, printInstruction);
        return 0;
    }

    DisAnalysis* analysis = AnalyzeNew(readByte, base, end);
    if (!analysis) {
        fprintf(stderr, "dis: out of memory; falling back to linear\n");
        DisRange(base, (uint16_t)(end - base + 1), readByte, printInstruction);
        return 0;
    }

    AnalyzeAddEntry(analysis, base);
    AnalyzeRun(analysis);

    if (AnalyzeOutOfRange(analysis))
        fprintf(stderr, "note: %u control-flow target%s outside the loaded "
                        "range %s not followed\n",
                AnalyzeOutOfRange(analysis),
                AnalyzeOutOfRange(analysis) == 1 ? "" : "s",
                AnalyzeOutOfRange(analysis) == 1 ? "was" : "were");

    printAnalyzed(analysis, base, end, base);
    AnalyzeFree(analysis);
    return 0;
}
