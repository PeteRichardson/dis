#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "memory.h"

#ifndef MAX_REGIONS
#define MAX_REGIONS 16
#endif

typedef enum {
    REGION_ROM,   // read-only, backed by data pointer
    REGION_RAM,   // readable/writable, backed by data pointer
    REGION_FILL   // read-only, returns a constant fill byte
} RegionType;

static const char* RegionTypeStr(RegionType t) {
    switch (t) {
    case REGION_ROM:  return "ROM";
    case REGION_RAM:  return "RAM";
    case REGION_FILL: return "FILL";
    default:          return "?";
    }
}


typedef struct {
    uint16_t    start;     // inclusive
    uint16_t    end;       // inclusive
    RegionType  type;
    uint8_t* data;      // points to backing storage for ROM/RAM; NULL for FILL
    uint8_t     fill;      // used for FILL
    bool        writable;  // convenience: true for RAM
} Region;

static Region   g_regions[MAX_REGIONS];
static uint8_t  g_region_count = 0;
static uint8_t  g_default_fill = 0x00;  // returned for unmapped addresses

// ---- Public helpers to build the sparse map ----

static bool add_region(Region r) {
    if (g_region_count >= MAX_REGIONS) return false;
    g_regions[g_region_count++] = r;
    return true;
}

// Map a read-only blob (e.g., your program image) at 'addr'.
// 'data' must remain valid for the lifetime of the emulator.
bool MapROM(uint16_t addr, const uint8_t* data, uint16_t len) {
    Region r = {
        .start = addr,
        .end = (uint16_t)(addr + (len ? (uint16_t)(len - 1) : 0)),
        .type = REGION_ROM,
        .data = (uint8_t*)data,
        .fill = 0,
        .writable = false
    };
    return add_region(r);
}

// Map writable RAM backed by 'storage' (length len).
// 'storage' must be provided by the caller (stack/global/static).
bool MapRAM(uint16_t addr, uint8_t* storage, uint16_t len) {
    Region r = {
        .start = addr,
        .end = (uint16_t)(addr + (len ? (uint16_t)(len - 1) : 0)),
        .type = REGION_RAM,
        .data = storage,
        .fill = 0,
        .writable = true
    };
    return add_region(r);
}

// Map a constant-filled range that always reads as 'fill' (not writable).
bool MapFill(uint16_t addr, uint16_t len, uint8_t fill) {
    Region r = {
        .start = addr,
        .end = (uint16_t)(addr + (len ? (uint16_t)(len - 1) : 0)),
        .type = REGION_FILL,
        .data = NULL,
        .fill = fill,
        .writable = false
    };
    return add_region(r);
}

// Optional: set the default unmapped byte (open-bus substitute). Default is 0x00.
void MemSetDefaultFill(uint8_t fill) {
    g_default_fill = fill;
}

// Convenience: set the 6502 RESET vector at $FFFC-$FFFD to 'entry' (little-endian)
// without needing to map the whole top page. You can still override later by
// adding another region that overlaps.
bool MapResetVector(uint16_t entry) {
    static uint8_t vec[2]; // static so its address stays valid
    vec[0] = (uint8_t)(entry & 0xFF);
    vec[1] = (uint8_t)((entry >> 8) & 0xFF);
    return MapROM(0xFFFC, vec, 2);
}

// (Optional) If you want a generic vector page with BRK/NMI/RESET/IRQ placeholders:
bool MapVectorPage(uint16_t nmi, uint16_t reset, uint16_t irq) {
    static uint8_t vecpg[6]; // $FFFA..$FFFF: NMI, RESET, IRQ/BRK
    vecpg[0] = (uint8_t)(nmi & 0xFF);
    vecpg[1] = (uint8_t)((nmi >> 8) & 0xFF);
    vecpg[2] = (uint8_t)(reset & 0xFF);
    vecpg[3] = (uint8_t)((reset >> 8) & 0xFF);
    vecpg[4] = (uint8_t)(irq & 0xFF);
    vecpg[5] = (uint8_t)((irq >> 8) & 0xFF);
    return MapROM(0xFFFA, vecpg, 6);
}

// ---- Required interface ----

uint8_t MemRead(uint16_t addr, bool isDbg) {
    (void)isDbg; // your emulator can use this flag to bypass side effects if you later add MMIO

    // Search from newest to oldest so later mappings override earlier ones.
    for (int i = (int)g_region_count - 1; i >= 0; --i) {
        Region* r = &g_regions[i];
        if (addr >= r->start && addr <= r->end) {
            uint16_t off = (uint16_t)(addr - r->start);
            switch (r->type) {
            case REGION_ROM:
            case REGION_RAM:
                return r->data[off];
            case REGION_FILL:
                return r->fill;
            }
        }
    }
    return g_default_fill; // unmapped
}

void MemWrite(uint16_t addr, uint8_t val) {
    // Linear search is fine because we expect very few regions.
    for (int i = (int)g_region_count - 1; i >= 0; --i) {
        Region* r = &g_regions[i];
        if (addr >= r->start && addr <= r->end) {
            if (r->writable && r->type == REGION_RAM) {
                r->data[(uint16_t)(addr - r->start)] = val;
            }
            // Writes to ROM/FILL are ignored by design.
            return;
        }
    }
    // Unmapped writes are ignored. (Alternatively, you could track them.)
}


void DumpRegions(void) {
    printf("=== %u region(s) ===\n", (unsigned)g_region_count);

    for (unsigned i = 0; i < g_region_count; ++i) {
        const Region* r = &g_regions[i];
        uint32_t len = (uint32_t)r->end - (uint32_t)r->start + 1u; // inclusive range
        if (r->end < r->start) len = 0; // sanity guard

        printf("#%u: [%04X..%04X] len=%u type=%s fill=%02X writable=%s\n",
            i,
            (unsigned)r->start, (unsigned)r->end,
            (unsigned)len,
            RegionTypeStr(r->type),
            (unsigned)r->fill,
            r->writable ? "yes" : "no");

        // Print first up to 16 bytes in hex
        printf("     bytes:");
        unsigned to_show = len < 16u ? (unsigned)len : 16u;
        for (unsigned off = 0; off < to_show; ++off) {
            uint8_t b = 0x00;
            switch (r->type) {
            case REGION_ROM:
            case REGION_RAM:
                if (r->data) {
                    b = r->data[off];
                }
                else {
                    b = 0x00; // shouldn't happen
                }
                break;
            case REGION_FILL:
                b = r->fill;
                break;
            }
            printf(" %02X", b);
        }
        if (len > 16u) printf(" ...");
        printf("\n");
    }
}
