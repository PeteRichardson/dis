# Test fixtures

`w65c02_demo.hex` and `w65c02_demo.rp6502` are generated; see
`docs/design.md` for the scripts that produce them.

## Vendored ROMs

`rtc.rp6502` and `adventure.rp6502` are copied unmodified from
[picocomputer/rp6502](https://github.com/picocomputer/rp6502), `tests/roms/`.
They are real toolchain output, which synthetic fixtures cannot substitute for:
`adventure.rp6502` carries 49 chunks, large ASCII blobs and 4 named assets.

Those files are covered by the rp6502 project's BSD 3-Clause licence
(copyright 2026, Rumbledethumps). The notice, list of conditions, and
disclaimer required by that licence are retained verbatim, unmodified from
upstream, in [`LICENSE.rp6502`](LICENSE.rp6502).
