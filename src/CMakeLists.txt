cmake_minimum_required(VERSION 3.16)

include(CheckSymbolExists)

add_executable (dis
  dis.c
  memory.c
  memory.h
  disassemble.c
  disassemble.h
  analyze.c
  analyze.h
  hexfile.c
  hexfile.h
  rp6502file.c
  rp6502file.h
  vrEmu6502.c
  vrEmu6502.h
)

check_symbol_exists(fopen_s "stdio.h" HAVE_FOPEN_S)
check_symbol_exists(strncpy_s "string.h" HAVE_STRNCPY_S)

if (HAVE_FOPEN_S)
    target_compile_definitions(dis PRIVATE -DHAVE_FOPEN_S)
endif()

if (HAVE_STRNCPY_S)
    target_compile_definitions(dis PRIVATE -DHAVE_STRNCPY_S)
endif()

add_executable(test_memory test_memory.c memory.c memory.h)
add_test(NAME memory COMMAND test_memory)

add_executable(test_disassemble
  test_disassemble.c
  disassemble.c
  disassemble.h
  vrEmu6502.c
  vrEmu6502.h
)
add_test(NAME disassemble COMMAND test_disassemble)

add_executable(test_hexfile
  test_hexfile.c
  hexfile.c
  hexfile.h
  memory.c
  memory.h
)
add_test(NAME hexfile COMMAND test_hexfile)

# End-to-end guard for the CLI wiring. readHexFile writes through MemWrite,
# which drops writes to unmapped addresses, so if dis.c stops mapping RAM
# first every byte is discarded and the file disassembles as a run of BRKs.
# Requiring bbr0 and forbidding brk catches that regression directly, and
# bbr0 only appears if --cpu selected w65c02.
add_test(NAME cli_hexfile
  COMMAND dis --cpu w65c02 ${CMAKE_SOURCE_DIR}/testdata/w65c02_demo.hex)
set_tests_properties(cli_hexfile PROPERTIES
  PASS_REGULAR_EXPRESSION "bbr0"
  FAIL_REGULAR_EXPRESSION "brk")

add_executable(test_rp6502file
  test_rp6502file.c
  rp6502file.c
  rp6502file.h
  memory.c
  memory.h
)
add_test(NAME rp6502file COMMAND test_rp6502file)

add_executable(test_analyze
  test_analyze.c
  analyze.c
  analyze.h
  disassemble.c
  disassemble.h
  vrEmu6502.c
  vrEmu6502.h
)
add_test(NAME analyze COMMAND test_analyze)

# End-to-end guard for .rp6502 support. The fixture's reset vector points
# at $0205 rather than the load address, so requiring the listing to start
# there proves the entry point came from the vector and not from the first
# chunk. Its XRAM chunk must be reported as skipped, never disassembled.
add_test(NAME cli_rp6502
  COMMAND dis --cpu w65c02 ${CMAKE_SOURCE_DIR}/testdata/w65c02_demo.rp6502)
set_tests_properties(cli_rp6502 PROPERTIES
  PASS_REGULAR_EXPRESSION "0205: "
  FAIL_REGULAR_EXPRESSION "ERROR")

# --linear must keep the pre-analysis behaviour exactly. bbr0 at $0200 is
# the first instruction of the fixture, so it only appears in a flat sweep
# from the load address -- the analyzed listing starts at the reset vector.
add_test(NAME cli_linear
  COMMAND dis --linear --cpu w65c02 ${CMAKE_SOURCE_DIR}/testdata/w65c02_demo.hex)
set_tests_properties(cli_linear PROPERTIES
  PASS_REGULAR_EXPRESSION "0200: 0f 12 05  bbr0"
  FAIL_REGULAR_EXPRESSION "sub_")

# Real toolchain output. Assertions are structural, not exact-output:
# exact listings would be brittle and would encode current behaviour as if
# it were correct.
add_test(NAME cli_rtc_rom
  COMMAND dis ${CMAKE_SOURCE_DIR}/testdata/rtc.rp6502)
set_tests_properties(cli_rtc_rom PROPERTIES
  FAIL_REGULAR_EXPRESSION "ERROR")

# Measured 2026-07-29: 242 functions discovered, 15794 bytes classified as
# data out of 48990 loaded. A large change in either direction is worth
# investigating.
#
# Split into two tests deliberately: CTest's PASS_REGULAR_EXPRESSION is
# OR-semantics, so a single test listing both patterns would pass on either
# one. Requiring a discovered function AND a data region is what proves the
# traversal actually separated code from data -- classifying everything one
# way means it is broken.
add_test(NAME cli_adventure_rom
  COMMAND dis ${CMAKE_SOURCE_DIR}/testdata/adventure.rp6502)
set_tests_properties(cli_adventure_rom PROPERTIES
  PASS_REGULAR_EXPRESSION "sub_"
  FAIL_REGULAR_EXPRESSION "ERROR")

add_test(NAME cli_adventure_data
  COMMAND dis ${CMAKE_SOURCE_DIR}/testdata/adventure.rp6502)
set_tests_properties(cli_adventure_data PROPERTIES
  PASS_REGULAR_EXPRESSION "bytes data"
  FAIL_REGULAR_EXPRESSION "ERROR")

