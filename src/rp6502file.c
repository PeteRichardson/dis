#include "rp6502file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"

/* Longest header line accepted. Real ones are a few dozen bytes. */
#define LINE_MAX 256

/* Chunks larger than the whole 6502 address space cannot be valid. */
#define CHUNK_MAX 0x10000u

/* 6502 hardware vectors: NMI $FFFA, RESET $FFFC, IRQ $FFFE. */
#define VECTOR_BASE 0xfffau

/* ------------------------------------------------------------------
 * CRC-32 (reflected, polynomial $EDB88320) -- the nibble-table form
 * littlefs uses, which is what the RIA's mem_crc32 wraps.
 */

uint32_t Rp6502Crc32(const uint8_t* data, size_t len) {
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };

    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 4) ^ table[(crc ^ (uint32_t)(data[i] >> 0)) & 0x0fu];
        crc = (crc >> 4) ^ table[(crc ^ (uint32_t)(data[i] >> 4)) & 0x0fu];
    }
    return ~crc;
}

/* ------------------------------------------------------------------
 * Line and number parsing
 */

/* Reads one line, stripping CR/LF. Returns false at EOF or on overflow. */
static bool readLine(FILE* f, char* buf, size_t size) {
    if (!fgets(buf, (int)size, f)) return false;

    size_t len = strlen(buf);
    if (len + 1 == size && buf[len - 1] != '\n') return false; /* too long */

    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    return true;
}

static bool matchesShebang(const char* line) {
    static const char* magic = "#!RP6502";
    for (int i = 0; i < 8; ++i) {
        char a = line[i], b = magic[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

/* Accepts decimal, 0xFF and $FF, as the RIA's str_parse_uint32 does. */
static bool parseUint32(const char** p, uint32_t* out) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') ++s;

    int base = 10;
    if (*s == '$') {
        base = 16;
        ++s;
    }
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    /* strtoul would accept a sign and its own 0x prefix; neither belongs
       here, and a negative would wrap silently. */
    if (*s == '-' || *s == '+') return false;

    char* endp = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &endp, base);
    if (endp == s || errno == ERANGE || v > 0xffffffffUL) return false;

    *out = (uint32_t)v;
    *p = endp;
    return true;
}

static bool parseEnd(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return *p == '\0';
}

/* ------------------------------------------------------------------ */

static void reportInvalid(const char* path, const char* why) {
    printf("ERROR: Invalid RP6502 ROM file: %s (%s)\n", path, why);
}

bool isRp6502File(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char magic[9] = { 0 };
    size_t n = fread(magic, 1, 8, f);
    fclose(f);

    return n == 8 && matchesShebang(magic);
}

bool readRp6502File(const char* path, Rp6502Rom* out) {
    memset(out, 0, sizeof *out);

    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("ERROR: Unable to open RP6502 ROM file: %s\n", path);
        return false;
    }

    char line[LINE_MAX];
    bool ok = false;

    /* Chunk section boundary. Chunks below it are loadable; anything after
       is a named asset, which is a filesystem entry rather than code. */
    long chunkSectionEnd = -1;

    /* Tracked separately from the vectors so a ROM that writes $FFFC does
       not stretch the disassembly range to the top of memory. */
    uint32_t codeLow = 0xffffffffu;
    uint32_t codeHigh = 0;
    bool haveCode = false;
    bool resetLo = false, resetHi = false;

    if (!readLine(f, line, sizeof line) || strlen(line) < 8 || !matchesShebang(line)) {
        reportInvalid(path, "missing #!RP6502 header");
        goto done;
    }

    long afterShebang = ftell(f);
    if (readLine(f, line, sizeof line) && line[0] == '#' && line[1] == '>') {
        const char* p = line + 2;
        uint32_t sectionLen, ignoredCrc;
        if (!parseUint32(&p, &sectionLen) || !parseUint32(&p, &ignoredCrc)) {
            reportInvalid(path, "malformed #> group header");
            goto done;
        }
        chunkSectionEnd = ftell(f) + (long)sectionLen;
    }
    else if (fseek(f, afterShebang, SEEK_SET) != 0) {
        reportInvalid(path, "seek failed");
        goto done;
    }

    for (;;) {
        if (chunkSectionEnd >= 0 && ftell(f) >= chunkSectionEnd) break;
        if (!readLine(f, line, sizeof line)) break;
        if (line[0] == '\0') continue;
        if (line[0] == '#') continue; /* comment, or a named asset header */

        uint32_t addr, len, crc;
        const char* p = line;
        if (!parseUint32(&p, &addr) || !parseUint32(&p, &len) ||
            !parseUint32(&p, &crc) || !parseEnd(p)) {
            reportInvalid(path, "malformed chunk header");
            goto done;
        }

        if (addr > 0x1ffffu || len == 0 || len > CHUNK_MAX ||
            (addr < 0x10000u && addr + len > 0x10000u) ||
            addr + len > 0x20000u) {
            reportInvalid(path, "chunk address or length out of range");
            goto done;
        }

        static uint8_t payload[CHUNK_MAX];
        if (fread(payload, 1, len, f) != len) {
            reportInvalid(path, "truncated chunk payload");
            goto done;
        }

        if (Rp6502Crc32(payload, len) != crc) {
            printf("ERROR: CRC mismatch in %s at $%05x (%u bytes)\n",
                   path, (unsigned)addr, (unsigned)len);
            goto done;
        }

        if (addr >= 0x10000u) {
            /* XRAM is not 6502 address space and holds data, not code. */
            ++out->xramChunks;
            out->xramBytes += len;
            continue;
        }

        ++out->ramChunks;
        for (uint32_t i = 0; i < len; ++i)
            MemWrite((uint16_t)(addr + i), payload[i]);

        if (addr <= 0xfffcu && addr + len > 0xfffcu) resetLo = true;
        if (addr <= 0xfffdu && addr + len > 0xfffdu) resetHi = true;

        /* Vector-only chunks are not code and must not extend the range. */
        if (addr < VECTOR_BASE) {
            uint32_t last = addr + len - 1;
            if (last >= VECTOR_BASE) last = VECTOR_BASE - 1;
            if (addr < codeLow) codeLow = addr;
            if (last > codeHigh) codeHigh = last;
            haveCode = true;
        }
    }

    if (!haveCode) {
        reportInvalid(path, "no 6502 code chunks");
        goto done;
    }

    out->hasResetVector = resetLo && resetHi;
    out->end = (uint16_t)codeHigh;

    if (out->hasResetVector) {
        uint16_t vector = (uint16_t)(MemRead(0xfffc, true) |
                                     ((uint16_t)MemRead(0xfffd, true) << 8));
        /* A vector pointing outside the loaded code would disassemble
           unmapped fill, so fall back rather than emit nonsense. */
        if (vector >= codeLow && vector <= codeHigh) {
            out->entry = vector;
        }
        else {
            printf("WARNING: reset vector $%04x is outside loaded code "
                   "($%04x-$%04x); starting at $%04x\n",
                   vector, (unsigned)codeLow, (unsigned)codeHigh,
                   (unsigned)codeLow);
            out->entry = (uint16_t)codeLow;
        }
    }
    else {
        out->entry = (uint16_t)codeLow;
    }

    ok = true;

done:
    fclose(f);
    return ok;
}
