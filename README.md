# dis
## Using the vrEMU6502 emulator as just a static disassembler.

### Current Status
Not really working.  I just want a simple static disassembler that takes 3 bytes and returns a reasonable string for the opcode and arguments, and maybe the address of the next instruction.  The goal was (is?) to add a dis(assemble) command to the PicoComputer's built-in REPL running on the pi pico RIA.  

 vrEMU6502's built in disassembler function works great, but it's overkill.  It assumes the Emulator is running (to access memory, etc).  The emulator allocates 64K for the 6502 address space, which seems heavy for my expected usage (just looking at small 6502 programs loaded at a specific location in the PicoComputer's RAM)  

 So I started to extract all the unnecessary stuff out of the disassemble routine, but got distracted by the scaffolding.   I vibecoded a sparse memory manager to replace the full 64K byte array vrEMU6502 uses.  That turned out to be interesting, but not useful.  I could have just kept the 64K buffer in the mac-based test program, and switched to the PicoComputer's existing memory access routines when I updated the PicoComputer REPL.

 Then I started working on loading programs from intel hex files.  That would have helped the disassembler running on the mac, but would also not contribute to the PicoComputer implementation.   So it's not integrated.

 For now, it just has some representative 6502 instructions of various lengths hardcoded in the source, and it disassembles and prints them to stdout.  It works.

 Maybe I'll pick this up again someday.