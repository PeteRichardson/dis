#include <assert.h>
#include <string.h>
#include "memory.h"

static void test_default_fill(void) {
    MemSetDefaultFill(0xEA);
    assert(MemRead(0x1000, 0) == 0xEA);
    assert(MemRead(0x0000, 0) == 0xEA);
    assert(MemRead(0xFFFF, 0) == 0xEA);
    MemSetDefaultFill(0x00);
}

static void test_rom_read(void) {
    static const uint8_t data[] = { 0x01, 0x02, 0x03 };
    MapROM(0x0300, data, sizeof(data));

    assert(MemRead(0x0300, 0) == 0x01);
    assert(MemRead(0x0301, 0) == 0x02);
    assert(MemRead(0x0302, 0) == 0x03);
}

static void test_rom_boundary(void) {
    static const uint8_t data[] = { 0xAB };
    MapROM(0x0400, data, sizeof(data));

    assert(MemRead(0x0400, 0) == 0xAB);
    assert(MemRead(0x03FF, 0) == 0x00); // just before: unmapped
    assert(MemRead(0x0401, 0) == 0x00); // just after: unmapped
}

static void test_rom_ignores_writes(void) {
    static const uint8_t data[] = { 0x55 };
    MapROM(0x0500, data, sizeof(data));

    MemWrite(0x0500, 0xFF);
    assert(MemRead(0x0500, 0) == 0x55); // unchanged
}

static void test_ram_read_write(void) {
    static uint8_t storage[4] = { 0 };
    MapRAM(0x0600, storage, sizeof(storage));

    MemWrite(0x0600, 0x11);
    MemWrite(0x0603, 0x22);

    assert(MemRead(0x0600, 0) == 0x11);
    assert(MemRead(0x0603, 0) == 0x22);
    assert(storage[0] == 0x11);
    assert(storage[3] == 0x22);
}

static void test_shadowing(void) {
    // Earlier region
    static const uint8_t base_data[] = { 0xAA, 0xBB };
    MapROM(0x0700, base_data, sizeof(base_data));

    // Later region overlaps the same address — should win
    static const uint8_t shadow_data[] = { 0xCC };
    MapROM(0x0700, shadow_data, sizeof(shadow_data));

    assert(MemRead(0x0700, 0) == 0xCC); // shadow wins
    assert(MemRead(0x0701, 0) == 0xBB); // only base covers this byte
}

static void test_fill_region(void) {
    MapFill(0x0800, 0x10, 0xFF);

    assert(MemRead(0x0800, 0) == 0xFF);
    assert(MemRead(0x080F, 0) == 0xFF);
    assert(MemRead(0x0810, 0) == 0x00); // just outside: unmapped
}

static void test_reset_vector(void) {
    MapResetVector(0x1234);

    assert(MemRead(0xFFFC, 0) == 0x34); // low byte
    assert(MemRead(0xFFFD, 0) == 0x12); // high byte
}

int main(void) {
    test_default_fill();
    test_rom_read();
    test_rom_boundary();
    test_rom_ignores_writes();
    test_ram_read_write();
    test_shadowing();
    test_fill_region();
    test_reset_vector();
    return 0;
}
