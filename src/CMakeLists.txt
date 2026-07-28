cmake_minimum_required(VERSION 3.16)

include(CheckSymbolExists)

add_executable (dis 
  dis.c
  memory.c
  memory.h
  disassemble.c
  disassemble.h
  hexfile.c
  hexfile.h
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



