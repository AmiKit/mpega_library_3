# Changelog

Fork of CISC's libmad-based `mpega.library` (7th release, 2004), turned into a
bugfixed, Hi-Fi decoder for **AmigaOS 3 / 68020**, built with a modern
`m68k-amigaos-gcc` (amiga-gcc, gcc 6.5). Dates are 2026. The pristine upstream
sources are preserved in `original-archives/`.

Plain `make` (or `make mpega.library_020_all`) builds
`mpega.library-bugfix` / `mpega.library-hifi` / `mpega.library-hifi-forced`
(copy the chosen one to `LIBS:` as `mpega.library`).

---

## Version 3.1

- Jumping around in a track no longer gives a short burst of noise right after
  the jump. The decoder used to play a few leftover scraps from where you had
  been before.
- Tracks cut from a longer recording, and internet radio streams, no longer
  start with a short burst of noise or an error.
- MP3s with a small ID3 tag now show the right length, and jumping in them lands
  where you asked.
- Volume control and ReplayGain now work at low and medium quality too. Before,
  the library quietly ignored them unless the player asked for high quality.
- The -bugfix flavour no longer crackles on loud passages when the volume is
  turned up high or ReplayGain boosts a quiet track; it clips cleanly instead.
- At 100 % volume the library no longer runs its volume code on every sample
  when there is nothing to change (no ReplayGain in the file), which saves CPU
  time.
- MP3s with a broken VBR header no longer show a length of 0:00, and seeking in
  them no longer jumps straight to the end.
- Streams that switch from mono to stereo (a mono jingle before a stereo song,
  for example) no longer play the stereo part at double speed when the player
  uses different settings for mono and stereo.
- At low quality, the library now always respects the highest playback rate the
  player asks for.
- Jumping in VBR MP3s without a seek table now lands close to where you asked.
  A quiet start used to send the jump far too early.
- MP3s with two tags in front (some taggers put a new one before the old one)
  now open. Before, a big second tag, such as cover art, stopped them from
  opening.
- When a player streams a file, its length now comes out right even with cover
  art in front, and a stream of unknown size no longer shows a length of about
  74 hours.
- If a read fails for a moment, such as a network hiccup, playback carries on
  from the right place instead of playing a scrap of old audio, also right
  after a jump.
- Players that open files without checking them first now play files with some
  junk in front, or whose first read fails, instead of stopping with an error.
- Opening a track takes a little less time on slow Amigas.
- A track that ends in an ID3v1 tag now ends cleanly also when the player
  streams it in small pieces (network, `PIPE:`). Before, it sometimes ended
  with an error, depending on where the last piece cut the tag.
- Two MP3s joined into one file (an audiobook or a mix made with `cat`, for
  example) now play to the end. Before, a player that stops on an error
  stopped after the first part.
- Jumping past the end of a track without a seek table now reports the time
  it reached (the end), not the one asked for.
- MP3s that decode louder than full scale (a loud track made louder with
  MP3Gain, for example) no longer crackle in the Hi-Fi flavours: they clip
  cleanly up to about three times full scale. Before, from about twice full
  scale, loud samples wrapped around. -bugfix keeps the original limit of
  about twice full scale.
- The Hi-Fi flavours are much faster, with the same accuracy: 2 to 3 times
  as fast as 3.0 in emulation, and about 20 % fewer CPU cycles on a 68020.
  -bugfix is unchanged.

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
