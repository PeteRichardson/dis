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



