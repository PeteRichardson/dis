#include <stdio.h>
#include <string.h>
#include "vrEmu6502.h"
#include "memory.h"
#include "hexfile.h"
#include "disassemble.h"

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
    fprintf(to, "usage: dis [--cpu MODEL] [program.hex]\n");
    fprintf(to, "  MODEL: 6502, 6502u, 65c02 (default), w65c02, r65c02\n");
    fprintf(to, "  with no file, disassembles a small built-in demo program\n");
}

int main(int argc, char** argv) {
    /* The RP6502's RIA is a W65C02S, but 65C02 stays the default so
       existing output is unchanged. */
    vrEmu6502Model model = CPU_65C02;
    const char* hexPath = NULL;

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
        else if (argv[i][0] == '-') {
            fprintf(stderr, "dis: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return 1;
        }
        else {
            hexPath = argv[i];
        }
    }

    MemSetDefaultFill(0x00);

    uint16_t base, end;
    if (hexPath) {
        /*
         * readHexFile writes through MemWrite, which silently drops
         * writes to unmapped addresses (memory.c). Without a RAM region
         * underneath, every loaded byte was discarded and the file
         * disassembled as a run of BRKs -- this path had never worked.
         *
         * The load extent is not known until the file is parsed, so back
         * the whole address space. Host-side only; the RIA supplies its
         * own memory and never links memory.c.
         */
        static uint8_t image[0xffff];
        MapRAM(0x0000, image, sizeof image);

        base = readHexFile(hexPath, &end);
        if (base == 0) return 1;
    }
    else {
        base = 0x0200;
        end = base + (uint16_t)sizeof(demo_image) - 1;
        MapROM(base, demo_image, sizeof(demo_image));
    }

    DisInit(model);
    DisRange(base, (uint16_t)(end - base + 1), readByte, printInstruction);

    return 0;
}
