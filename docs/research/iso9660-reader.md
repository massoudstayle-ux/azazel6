# Read-only ISO9660 reader

The Tekken 6 profile now has an isolated, read-only ISO9660 parser in
`profiles/tekken6/host/iso9660.cpp`. It is intentionally not connected to
`sceIoOpen`, `sceIoRead`, or UMD activation yet.

## Current behavior

- Finds a Primary Volume Descriptor starting at logical sector 16.
- Requires the ISO9660 identifier `CD001`, descriptor version 1, and 2048-byte
  logical blocks.
- Checks both little-endian and big-endian copies of 16-bit and 32-bit fields.
- Checks every extent against the supplied image byte span before accessing it.
- Lists directories and finds paths case-insensitively.
- Removes the common `;1` file-version suffix from identifiers.
- Rejects parent-directory traversal, unsupported identifier characters,
  malformed/cross-sector directory records, directory cycles, and multi-extent
  records.
- Reads individual files with a default 64 MiB limit. The caller owns and keeps
  the source image bytes alive for the lifetime of the `Image` object.

## Deliberate limitations

This is an initial ISO9660 Level 1-style reader, not a complete UMD implementation.
Joliet names, Rock Ridge extensions, multi-extent files, interleaving, and
writable filesystem operations are not implemented. It does not load a game,
provide a UMD device state, or prove that Tekken 6 can boot.

The reader is built as the standalone `azazel6_iso` library. It is not linked
into the startup HLE path. The next step is to validate the parser with synthetic
tests and real, user-owned disc images locally, then design a bounded host disc
source. HLE integration must wait until the game's observed I/O ABI and required
file access patterns are understood.

## Tests

The synthetic test target is `azazel6_iso_tests` and the CTest name is
`azazel6_iso9660`. It creates a tiny in-memory image; no commercial game data
is stored in the repository.
