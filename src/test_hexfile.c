/*
 * Tests for the Intel HEX reader.
 *
 * Files are generated at run time rather than committed, so the cases stay
 * next to the assertions that explain them. The end-to-end fixture that
 * guards the CLI wiring lives in testdata/ instead, because that path needs
 * a real file on disk.
 *
 * Records below carry correct checksums even though readHexFile does not
 * verify them -- see test_checksum_not_validated.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hexfile.h"
#include "memory.h"

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

/*
 * MemWrite silently drops writes to unmapped addresses, so a backing
 * region has to exist before loading. Forgetting this is exactly the bug
 * that made "dis prog.hex" emit a page of BRKs.
 */
static uint8_t ram[0xffff];

#define TMP_HEX "test_hexfile_tmp.hex"

static void writeHexFile(const char* const* lines, unsigned count) {
    FILE* f = fopen(TMP_HEX, "w");
    if (!f) {
        printf("FAIL: cannot create %s\n", TMP_HEX);
        ++failures;
        return;
    }
    for (unsigned i = 0; i < count; ++i)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

static void resetMemory(void) {
    memset(ram, 0, sizeof ram);
    MapRAM(0x0000, ram, sizeof ram);
}

/* ------------------------------------------------------------------ */

static void test_single_record(void) {
    static const char* const lines[] = {
        ":0A0200000F120507101A8005B21056",  /* 10 bytes at $0200 */
        ":00000001FF",                      /* EOF */
    };

    resetMemory();
    writeHexFile(lines, 2);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0x0200, "single: base $%04x, expected $0200", base);
    CHECK(end == 0x0209, "single: end $%04x, expected $0209", end);

    static const uint8_t expect[] = {
        0x0f, 0x12, 0x05, 0x07, 0x10, 0x1a, 0x80, 0x05, 0xb2, 0x10
    };
    for (unsigned i = 0; i < sizeof expect; ++i)
        CHECK(MemRead((uint16_t)(0x0200 + i), true) == expect[i],
              "single: byte at $%04x is $%02x, expected $%02x",
              0x0200 + i, MemRead((uint16_t)(0x0200 + i), true), expect[i]);

    /* Nothing outside the record was touched. */
    CHECK(MemRead(0x01ff, true) == 0x00, "single: wrote below the load address");
    CHECK(MemRead(0x020a, true) == 0x00, "single: wrote above the load address");
}

static void test_multiple_records(void) {
    static const char* const lines[] = {
        ":04020000EAEAEAEA52",  /* 4 bytes at $0200 */
        ":03021000AD3412F8",    /* 3 bytes at $0210, leaving a gap */
        ":02030000A94210",      /* 2 bytes at $0300 */
        ":00000001FF",
    };

    resetMemory();
    writeHexFile(lines, 4);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0x0200, "multi: base $%04x, expected $0200", base);
    CHECK(end == 0x0301, "multi: end $%04x, expected $0301", end);

    CHECK(MemRead(0x0200, true) == 0xea, "multi: first record not loaded");
    CHECK(MemRead(0x0210, true) == 0xad, "multi: second record not loaded");
    CHECK(MemRead(0x0300, true) == 0xa9, "multi: third record not loaded");

    /* The gap between records stays at the default fill. */
    CHECK(MemRead(0x0208, true) == 0x00, "multi: gap between records not empty");
}

/*
 * base comes from the first data record encountered, not the lowest
 * address. With records out of order that makes base greater than the
 * start of loaded data -- worth pinning, since a caller computing a length
 * as end - base would get it wrong.
 */
static void test_records_out_of_order(void) {
    static const char* const lines[] = {
        ":02030000A94210",      /* $0300 first */
        ":04020000EAEAEAEA52",  /* then $0200 */
        ":00000001FF",
    };

    resetMemory();
    writeHexFile(lines, 3);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0x0300, "out of order: base $%04x, expected $0300 (first record)", base);
    CHECK(end == 0x0301, "out of order: end $%04x, expected $0301", end);
    CHECK(MemRead(0x0200, true) == 0xea, "out of order: later record not loaded");
}

/* Lines not starting with ':' are skipped, so hex files can carry comments. */
static void test_comments_and_blank_lines(void) {
    static const char* const lines[] = {
        "# a comment",
        "",
        ":04020000EAEAEAEA52",
        "   indented junk",
        ":00000001FF",
    };

    resetMemory();
    writeHexFile(lines, 5);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0x0200, "comments: base $%04x, expected $0200", base);
    CHECK(MemRead(0x0200, true) == 0xea, "comments: record not loaded");
}

static void test_stops_at_eof_record(void) {
    static const char* const lines[] = {
        ":04020000EAEAEAEA52",
        ":00000001FF",          /* EOF */
        ":02030000A94210",      /* must be ignored */
    };

    resetMemory();
    writeHexFile(lines, 3);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0x0200, "eof: base $%04x, expected $0200", base);
    CHECK(end == 0x0203, "eof: end $%04x, expected $0203", end);
    CHECK(MemRead(0x0300, true) == 0x00,
          "eof: record after the EOF record was loaded");
}

static void test_missing_file(void) {
    resetMemory();

    uint16_t end = 0xffff;
    uint16_t base = readHexFile("no_such_file_should_exist.hex", &end);

    CHECK(base == 0, "missing file: base $%04x, expected 0", base);
}

static void test_no_records(void) {
    static const char* const lines[] = {
        "this file has no hex records at all",
    };

    resetMemory();
    writeHexFile(lines, 1);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0, "no records: base $%04x, expected 0", base);
}

/*
 * Documents a real gap rather than asserting desired behaviour: the
 * checksum byte is never read, so a corrupt file loads silently. The final
 * byte here is $00 instead of the correct $52.
 */
static void test_checksum_not_validated(void) {
    static const char* const lines[] = {
        ":04020000EAEAEAEA00",  /* wrong checksum */
        ":00000001FF",
    };

    resetMemory();
    writeHexFile(lines, 2);

    uint16_t end = 0;
    uint16_t base = readHexFile(TMP_HEX, &end);

    CHECK(base == 0x0200 && MemRead(0x0200, true) == 0xea,
          "checksum: expected the bad-checksum record to load anyway "
          "(readHexFile does not validate checksums)");
}

int main(void) {
    MemSetDefaultFill(0x00);

    test_single_record();
    test_multiple_records();
    test_records_out_of_order();
    test_comments_and_blank_lines();
    test_stops_at_eof_record();
    test_missing_file();
    test_no_records();
    test_checksum_not_validated();

    remove(TMP_HEX);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all hexfile tests passed\n");
    return 0;
}
