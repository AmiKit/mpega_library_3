# Changelog

Fork of CISC's libmad-based `mpega.library` (7th release, 2004), turned into a
bugfixed, Hi-Fi decoder for **AmigaOS 3 / 68020**, built with a modern
`m68k-amigaos-gcc` (amiga-gcc, gcc 6.5). Dates are 2026. The pristine upstream
sources are preserved in `original-archives/`.

Plain `make` (or `make mpega.library_020_all`) builds
`mpega.library-bugfix` / `mpega.library-hifi` / `mpega.library-hifi-forced`
(copy the chosen one to `LIBS:` as `mpega.library`).

---

## Version 3.0 (25 July 2026, shipped only with AmiKit X13.1)

- Three flavours from one source tree: -bugfix (the original speed-optimized
  decode), -hifi (a full-precision decode that still follows the player's
  settings) and -hifi-forced (always the highest quality).
- -bugfix decodes as fast as the 2004 mpega.library 2.4 (faster in
  emulation).
- Players with a small stack (the AmigaDOS default is 4000 bytes) no longer
  crash the library.
- Streams read in short pieces (network, PIPE:, serial, CD) now play to the
  end. Before, they stopped almost at once.
- Every track now plays its last fraction of a second, and its length is no
  longer one frame short.
- Mono now mixes both channels. Before, the right channel was thrown away.
- Jumps now land on the exact position, not on a whole second. A jump within
  the first second used to restart the track.
- Jumping into the middle of some files no longer reports an error on
  intact audio.
- Files under 2 KB now open, and some ID3v2 tag sizes no longer corrupt
  memory.
- MP3s with a LAME ReplayGain tag no longer freeze the whole machine.
  ReplayGain works on a 68020 for the first time.
- A file that ends in an ID3v1 tag now ends cleanly instead of with an error.
- Two players can decode at the same time.
- Damaged or hostile MP1, MP2 and MP3 files can no longer make the decoder
  read past the end of a frame (the 2017 libmad vulnerabilities
  CVE-2017-8372, 8373 and 8374).
