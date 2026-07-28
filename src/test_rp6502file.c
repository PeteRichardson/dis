/*
 * Tests for the RP6502 ROM reader.
 *
 * ROM files are generated at run time so each case sits next to the
 * assertion explaining it. The end-to-end fixture that exercises the CLI
 * lives in testdata/ instead, because that path needs a real file on disk.
 *
 * Negative cases print diagnostics from readRp6502File; that output is
 * expected and does not indicate failure.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "rp6502file.h"

static int failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
            ++failures;                                    \
        }                                                  \
    } while (0)

#define TMP_ROM "test_rp6502_tmp.rp6502"

/* MemWrite drops writes to unmapped addresses, so back the whole space. */
static uint8_t ram[0xffff];

static void resetMemory(void) {
    memset(ram, 0, sizeof ram);
    MapRAM(0x0000, ram, sizeof ram);
}

/* A short, recognisable 6502 program: lda #$42 / lda $10 / jmp $0200 */
static const uint8_t code[] = {
    0xa9, 0x42, 0xa5, 0x10, 0x4c, 0x00, 0x02
};

/* Writes "addr len crc" followed by the raw payload. */
static void writeChunk(FILE* f, uint32_t addr, const uint8_t* data, uint32_t len) {
    fprintf(f, "%u %u %u\n", (unsigned)addr, (unsigned)len,
            (unsigned)Rp6502Crc32(data, len));
    fwrite(data, 1, len, f);
}

/* ------------------------------------------------------------------ */

static void test_crc32_known_vectors(void) {
    /* Cross-checked against Python's zlib.crc32. $CBF43926 for "123456789"
       is the canonical CRC-32/ISO-HDLC check value. */
    CHECK(Rp6502Crc32((const uint8_t*)"", 0) == 0x00000000u,
          "crc32(\"\") = $%08x", Rp6502Crc32((const uint8_t*)"", 0));
    CHECK(Rp6502Crc32((const uint8_t*)"a", 1) == 0xe8b7be43u,
          "crc32(\"a\") = $%08x", Rp6502Crc32((const uint8_t*)"a", 1));
    CHECK(Rp6502Crc32((const uint8_t*)"123456789", 9) == 0xcbf43926u,
          "crc32(\"123456789\") = $%08x",
          Rp6502Crc32((const uint8_t*)"123456789", 9));
}

static void test_minimal_rom(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    writeChunk(f, 0x0200, code, sizeof code);
    fclose(f);

    Rp6502Rom rom;
    bool ok = readRp6502File(TMP_ROM, &rom);

    CHECK(ok, "minimal: load failed");
    if (!ok) return;

    CHECK(rom.entry == 0x0200, "minimal: entry $%04x, expected $0200", rom.entry);
    CHECK(rom.end == 0x0206, "minimal: end $%04x, expected $0206", rom.end);
    CHECK(rom.ramChunks == 1, "minimal: %u ram chunks, expected 1", rom.ramChunks);
    CHECK(rom.xramChunks == 0, "minimal: %u xram chunks, expected 0", rom.xramChunks);
    CHECK(!rom.hasResetVector, "minimal: reported a reset vector");

    for (unsigned i = 0; i < sizeof code; ++i)
        CHECK(MemRead((uint16_t)(0x0200 + i), true) == code[i],
              "minimal: byte at $%04x is $%02x, expected $%02x",
              0x0200 + i, MemRead((uint16_t)(0x0200 + i), true), code[i]);
}

/* Decimal, C-style hex and MOS-style hex are all accepted. */
static void test_number_formats(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "$0200 0x%x %u\n", (unsigned)sizeof code,
            (unsigned)Rp6502Crc32(code, sizeof code));
    fwrite(code, 1, sizeof code, f);
    fclose(f);

    Rp6502Rom rom;
    CHECK(readRp6502File(TMP_ROM, &rom), "number formats: load failed");
    CHECK(MemRead(0x0200, true) == 0xa9, "number formats: chunk not loaded");
}

static void test_comments_skipped(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "# built by the test suite\n");
    writeChunk(f, 0x0200, code, sizeof code);
    fprintf(f, "# trailing comment\n");
    fclose(f);

    Rp6502Rom rom;
    CHECK(readRp6502File(TMP_ROM, &rom), "comments: load failed");
    CHECK(MemRead(0x0200, true) == 0xa9, "comments: chunk not loaded");
}

/* XRAM is not 6502 address space: counted, reported, never loaded. */
static void test_xram_chunks_skipped(void) {
    resetMemory();

    static const uint8_t blob[16] = { 0xff };

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    writeChunk(f, 0x0200, code, sizeof code);
    writeChunk(f, 0x10000, blob, sizeof blob);
    fclose(f);

    Rp6502Rom rom;
    bool ok = readRp6502File(TMP_ROM, &rom);

    CHECK(ok, "xram: load failed");
    if (!ok) return;

    CHECK(rom.ramChunks == 1, "xram: %u ram chunks, expected 1", rom.ramChunks);
    CHECK(rom.xramChunks == 1, "xram: %u xram chunks, expected 1", rom.xramChunks);
    CHECK(rom.xramBytes == 16, "xram: %u bytes, expected 16", rom.xramBytes);
    CHECK(rom.end == 0x0206, "xram: end $%04x, expected $0206 (xram excluded)",
          rom.end);
}

/* A ROM with only XRAM has nothing to disassemble. */
static void test_xram_only_is_an_error(void) {
    resetMemory();

    static const uint8_t blob[4] = { 1, 2, 3, 4 };

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    writeChunk(f, 0x10000, blob, sizeof blob);
    fclose(f);

    Rp6502Rom rom;
    CHECK(!readRp6502File(TMP_ROM, &rom), "xram only: expected failure");
}

/*
 * The reset vector is the entry point, and a vector-only chunk at $FFFC
 * must not stretch end to the top of memory.
 */
static void test_reset_vector_is_entry_point(void) {
    resetMemory();

    static const uint8_t vector[2] = { 0x04, 0x02 }; /* $0204 */

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    writeChunk(f, 0x0200, code, sizeof code);
    writeChunk(f, 0xfffc, vector, sizeof vector);
    fclose(f);

    Rp6502Rom rom;
    bool ok = readRp6502File(TMP_ROM, &rom);

    CHECK(ok, "reset vector: load failed");
    if (!ok) return;

    CHECK(rom.hasResetVector, "reset vector: not detected");
    CHECK(rom.entry == 0x0204, "reset vector: entry $%04x, expected $0204", rom.entry);
    CHECK(rom.end == 0x0206, "reset vector: end $%04x, expected $0206 "
                             "(vector chunk must not extend the range)", rom.end);
}

/* A vector pointing outside the loaded code falls back rather than
   disassembling unmapped fill. */
static void test_reset_vector_out_of_range(void) {
    resetMemory();

    static const uint8_t vector[2] = { 0x00, 0x90 }; /* $9000, nothing there */

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    writeChunk(f, 0x0200, code, sizeof code);
    writeChunk(f, 0xfffc, vector, sizeof vector);
    fclose(f);

    Rp6502Rom rom;
    bool ok = readRp6502File(TMP_ROM, &rom);

    CHECK(ok, "vector out of range: load failed");
    if (!ok) return;

    CHECK(rom.entry == 0x0200,
          "vector out of range: entry $%04x, expected fallback to $0200", rom.entry);
}

/* The newer format bounds the chunk section; named assets follow it. */
static void test_group_header_bounds_chunks(void) {
    resetMemory();

    char chunkHeader[64];
    snprintf(chunkHeader, sizeof chunkHeader, "%u %u %u\n",
             0x0200u, (unsigned)sizeof code,
             (unsigned)Rp6502Crc32(code, sizeof code));
    unsigned sectionLen = (unsigned)(strlen(chunkHeader) + sizeof code);

    static const uint8_t asset[8] = { 0xde, 0xad };

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "#>%u 0\n", sectionLen);
    fputs(chunkHeader, f);
    fwrite(code, 1, sizeof code, f);
    /* A named asset after the section: must not be parsed as a chunk. */
    fprintf(f, "#>%u 0 splash\n", (unsigned)sizeof asset);
    fwrite(asset, 1, sizeof asset, f);
    fclose(f);

    Rp6502Rom rom;
    bool ok = readRp6502File(TMP_ROM, &rom);

    CHECK(ok, "group header: load failed");
    if (!ok) return;

    CHECK(rom.ramChunks == 1, "group header: %u ram chunks, expected 1", rom.ramChunks);
    CHECK(rom.entry == 0x0200, "group header: entry $%04x, expected $0200", rom.entry);
}

static void test_crc_mismatch_rejected(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "%u %u %u\n", 0x0200u, (unsigned)sizeof code, 0u); /* wrong crc */
    fwrite(code, 1, sizeof code, f);
    fclose(f);

    Rp6502Rom rom;
    CHECK(!readRp6502File(TMP_ROM, &rom), "crc mismatch: expected failure");
}

static void test_missing_shebang_rejected(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    writeChunk(f, 0x0200, code, sizeof code);
    fclose(f);

    Rp6502Rom rom;
    CHECK(!readRp6502File(TMP_ROM, &rom), "missing shebang: expected failure");
}

/* The RIA matches the shebang case-insensitively; so do we. */
static void test_shebang_case_insensitive(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!rp6502\n");
    writeChunk(f, 0x0200, code, sizeof code);
    fclose(f);

    Rp6502Rom rom;
    CHECK(readRp6502File(TMP_ROM, &rom), "lowercase shebang: expected success");
}

static void test_truncated_payload_rejected(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "%u %u %u\n", 0x0200u, (unsigned)sizeof code,
            (unsigned)Rp6502Crc32(code, sizeof code));
    fwrite(code, 1, sizeof code - 3, f); /* short */
    fclose(f);

    Rp6502Rom rom;
    CHECK(!readRp6502File(TMP_ROM, &rom), "truncated payload: expected failure");
}

static void test_address_range_validation(void) {
    resetMemory();

    /* Straddles the RAM/XRAM boundary at $10000. */
    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "%u %u %u\n", 0xffffu, 2u, 0u);
    fputc(0x00, f);
    fputc(0x00, f);
    fclose(f);

    Rp6502Rom rom;
    CHECK(!readRp6502File(TMP_ROM, &rom), "straddling chunk: expected failure");

    /* Past the end of XRAM. */
    f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "%u %u %u\n", 0x20000u, 1u, 0u);
    fputc(0x00, f);
    fclose(f);

    CHECK(!readRp6502File(TMP_ROM, &rom), "out-of-range address: expected failure");

    /* Zero length. */
    f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "%u %u %u\n", 0x0200u, 0u, 0u);
    fclose(f);

    CHECK(!readRp6502File(TMP_ROM, &rom), "zero-length chunk: expected failure");
}

static void test_malformed_chunk_header_rejected(void) {
    resetMemory();

    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    fprintf(f, "0x0200 notanumber 0\n");
    fclose(f);

    Rp6502Rom rom;
    CHECK(!readRp6502File(TMP_ROM, &rom), "malformed header: expected failure");
}

/* Detection sniffs the contents, so the extension is irrelevant. */
static void test_format_detection(void) {
    FILE* f = fopen(TMP_ROM, "wb");
    fprintf(f, "#!RP6502\n");
    writeChunk(f, 0x0200, code, sizeof code);
    fclose(f);
    CHECK(isRp6502File(TMP_ROM), "detection: ROM not recognised");

    f = fopen(TMP_ROM, "wb");
    fprintf(f, ":0A0200000F120507101A8005B21056\n:00000001FF\n");
    fclose(f);
    CHECK(!isRp6502File(TMP_ROM), "detection: Intel HEX misidentified as a ROM");

    CHECK(!isRp6502File("no_such_file_should_exist.rp6502"),
          "detection: missing file reported as a ROM");
}

int main(void) {
    MemSetDefaultFill(0x00);

    test_crc32_known_vectors();
    test_minimal_rom();
    test_number_formats();
    test_comments_skipped();
    test_xram_chunks_skipped();
    test_xram_only_is_an_error();
    test_reset_vector_is_entry_point();
    test_reset_vector_out_of_range();
    test_group_header_bounds_chunks();
    test_crc_mismatch_rejected();
    test_missing_shebang_rejected();
    test_shebang_case_insensitive();
    test_truncated_payload_rejected();
    test_address_range_validation();
    test_malformed_chunk_header_rejected();
    test_format_detection();

    remove(TMP_ROM);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all rp6502 file tests passed\n");
    return 0;
}
