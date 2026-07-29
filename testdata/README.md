# Test fixtures

`w65c02_demo.hex` and `w65c02_demo.rp6502` are generated; see
`docs/design.md` for the scripts that produce them.

## Vendored ROMs

`rtc.rp6502` and `adventure.rp6502` are copied unmodified from
[picocomputer/rp6502](https://github.com/picocomputer/rp6502), `tests/roms/`.
They are real toolchain output, which synthetic fixtures cannot substitute for:
`adventure.rp6502` carries 49 chunks, large ASCII blobs and 4 named assets.

Those files are covered by the rp6502 project's BSD 3-Clause licence:

> Copyright (c) 2023 Rumbledethumps
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the conditions of the BSD
> 3-Clause License are met. See https://github.com/picocomputer/rp6502
> for the full text.
