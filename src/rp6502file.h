#ifndef __RP6502FILE_H__
#define __RP6502FILE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Reader for RP6502 ROM files (.rp6502), the format PicoComputer binaries
 * ship in.
 *
 * The format is a text/binary hybrid, mirrored from the RIA's own parser
 * in src/ria/mon/rom.c of picocomputer/rp6502:
 *
 *     #!RP6502                 shebang, matched case-insensitively
 *     #>len crc                optional group header (newer format);
 *                              len bounds the loadable chunk section
 *     # comment                lines starting with # are skipped
 *     addr len crc             chunk header, three unsigned integers
 *     <len raw binary bytes>   payload, immediately after the newline
 *
 * Numbers may be decimal, C-style hex (0xFF) or MOS-style hex ($FF).
 *
 * Addresses below $10000 are 6502 RAM. Addresses $10000-$1FFFF are XRAM,
 * which is not in the 6502 address space and holds data rather than code;
 * those chunks are counted and skipped.
 */

typedef struct {
    uint16_t entry;          /* where to start disassembling */
    uint16_t end;            /* highest 6502 address holding code */
    bool     hasResetVector; /* ROM supplied both $FFFC and $FFFD */
    unsigned ramChunks;      /* chunks loaded into the 6502 address space */
    unsigned xramChunks;     /* chunks skipped because they target XRAM */
    uint32_t xramBytes;      /* total size of those skipped chunks */
} Rp6502Rom;

/*
 * Load a ROM's 6502 RAM chunks through MemWrite and describe what was
 * found. Returns false on I/O errors, malformed headers, out-of-range
 * addresses, truncated payloads, or a chunk whose CRC does not match.
 *
 * A backing RAM region must be mapped first: MemWrite silently discards
 * writes to unmapped addresses.
 *
 * entry is the reset vector at $FFFC/$FFFD when the ROM supplies one --
 * that is where the CPU actually starts, and the RIA only treats a ROM as
 * runnable if both bytes are present -- otherwise the lowest loaded
 * address. Chunks lying entirely at or above $FFFA are 6502 hardware
 * vectors rather than code and are excluded from entry and end.
 */
bool readRp6502File(const char* path, Rp6502Rom* out);

/*
 * True if the file begins with the RP6502 shebang. Sniffs the contents
 * rather than the file name, so a ROM saved under any extension is still
 * recognised. Returns false if the file cannot be opened.
 */
bool isRp6502File(const char* path);

/*
 * CRC-32 as the RIA computes it. Its mem_crc32 is ~lfs_crc(~crc, ...),
 * and littlefs's lfs_crc is the reflected nibble-table CRC-32 with
 * polynomial $EDB88320 -- so this is standard IEEE/zlib CRC-32.
 * CRC-32 of "123456789" is $CBF43926.
 */
uint32_t Rp6502Crc32(const uint8_t* data, size_t len);

#endif
