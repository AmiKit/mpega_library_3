# Regression harness

This harness answers exactly one question:

> **Did this change alter what the decoder produces?**

It is not a correctness proof and not a listening test. It is the safety net that
makes fixes reviewable: every fix is expected either to leave the
output bit-identical, or to change it in a way you can point at and justify.

## Running it

```sh
make check           # verify against the committed baselines
make check-asan      # same, but built with AddressSanitizer + UBSan
make check-m68k      # the built m68k flavours under vamos vs the host (below)
make check-update    # REWRITE the baselines -- only on a tree you have verified
make fixtures        # regenerate the fixtures (needs ffmpeg; they are committed)
```

No Amiga toolchain is required: the harness builds the real library sources
with the **host** compiler. It builds outside the repository (`/tmp` by default)
because the working copy may live on a cloud-synced drive where generating
hundreds of object files is slow and sync-hostile. `MPEGA_BUILD_DIR` names
another build folder; it must not exist yet, because it is removed at the end
(an existing one is refused, `make check-m68k` likewise).

## What it actually builds

The real sources (`wrap_mpega.c` plus all twelve `mad/*.c`) linked against a
small host shim, in each of the three shipped decode configurations:

| Config | Defines |
|---|---|
| `bugfix` | `-DFPM_DEFAULT -DOPT_SPEED -DOPT_SSO` |
| `hifi` | `-DFPM_64BIT -DOPT_ACCURACY` |
| `hifi-forced` | `-DFPM_64BIT -DOPT_ACCURACY -DMPEGA_FORCE_HIFI` |

`decode_ref` then drives the library the way a player does: `WRAP_MPEGA_open()`,
`WRAP_MPEGA_decode_frame()` in a loop, `WRAP_MPEGA_close()`. Each fixture is
decoded under every player-setting combination in `MATRIX` (`run.sh`), and both
the PCM and the reported stream metadata are hashed into
`expected/<config>.txt`.

Some rows must agree with each other (the hook rows, `bigbuf`, `scale100`,
`failread`, `nocheck`, the `short` row of every fixture and the three fixtures with the same
audio, all below). `run.sh` checks those pairs on every run, before it compares
or writes a baseline, so `make check-update` refuses to write one that breaks
them.

`mixq` gives mono and stereo different outputs (mono low quality at half rate,
stereo high quality at full rate); with `l3_44_monostereo.mp3` the output
configuration switches mid-stream, and a half-rate option left over from the
mono part used to halve the stereo part. `q0max` is low quality with an
automatic divider and a `freq_max` below the source rate, which low quality used
to ignore.

`hook` and `hookseek` read through a `bs_access` hook (`decode_ref --hook=N`, a
host hook over `FILE *`) instead of `dos.library`, the path streaming players
take. Their PCM hash must equal the `default` and `seek` row of the same file
(`run.sh` checks it). `hook` reports
`stream_size` 0, so the open falls back to its own size estimate. A hook seek
sent to a clearly wrong position (byte 0, say) changes `hookseek`; a few bytes
off is absorbed by the sync search and does not. The size fallback bug needs a file
WITHOUT a Xing/LAME header (that header corrects the size first), which is what
`l3_44_id3_pad_noxing.mp3` is: through `hook` it reports 0 ms; with the bug put back it
reported 268435449 ms.

`hooklate` seeks 1.5 s deep through a hook that reports `stream_size` 0, so
the size is only the open's guess (the first buffer). A seek used to be
limited to that guess and landed inside the first 8 KB while `MPEGA_time`
reported 1.5 s. Its PCM must equal `seeklate` (`run.sh` checks it);
with the old limit put back, four fixtures without a Xing/LAME size differ.

`l3_44_midstream.mp3` is a track cut at a frame boundary without
re-encoding, the way frame-exact cutters and stream recorders make files: its
first frame needs bit-reservoir bytes from before the cut. The open used to
leave its own leftovers in the reservoir, so the first frames decoded as noise
or failed with `MPEGA_ERR_BADFRAME` (this fixture: BADFRAME on the first
call). Its `short` row must equal its `default` row: the post-seek ride-out
used to give up at the end of a short buffer instead of refilling it, and the
open decoded from a first buffer of only two reads (with 400-byte reads it
failed outright; a 320 kbit/s stream failed even with 1000-byte reads).

`l3_44_stereo_id3.mp3` and `l3_44_id3_8192.mp3` carry the same audio as
`l3_44_stereo_cbr128.mp3` behind a 711- and an 8192-byte ID3v2 tag (45 bytes
in `cbr128`), and `l3_44_two_id3.mp3` behind two tags of 10000 bytes each, so
all four must give identical rows, except the byte count in `bigbuf` (the tags
are read too). Only the first of two tags used to be skipped: no frame in the
first buffer, and `l3_44_two_id3.mp3` did not open. A tag small enough to stay
in the first buffer used to be counted twice: shorter length, later seeks.
Through `hook` and `hooklate` (`stream_size` 0) the size comes from the LAME
music length, which leaves the tags out; the tag was then subtracted again and
the length came out short by it, 512 ms for `l3_44_id3_8192.mp3`. And the music
length was only taken when the size guess (the first tag's size) fitted the
first buffer: behind a 10000-byte tag a CBR file reported 0 ms.

`l3_44_notoc.mp3` is VBR with a Xing header but no TOC: half a second of
silence (32 kbit/s frames), then noise. A seek has to go by bitrate, and it took
the bitrate of the last decoded frame instead of the 146 kbit/s average: after
10 silent frames a seek to 1500 ms landed at 731 ms of audio while `MPEGA_time`
said 1500. Now it lands at 1697 ms (the cheap silence pulls the average down,
and the first frames after a seek are ridden out). The average itself was
bytes / 125 / whole seconds, rounded down: 148 here, up to twice too high in a
file under two seconds.

`scale4` sets 400 % volume (`MPEGA_scale`). The gain used to scale the subband
samples before synthesis, and `-bugfix` (`FPM_DEFAULT`) synthesis has no
headroom past about twice full scale: on loud music up to 8.6 % of the
samples came out with the wrong sign. It is now applied after synthesis and
held at full scale, so loud samples clip; no sample in any fixture flips its
sign in any config (checked against 4x the `default` row when the row was
added).

`scale100` sets 100 % volume and must equal `default`. A `+ 1` in
`MPEGA_scale` (since 2004) made 100 % a gain of 1.0000000037, so the gain loop
ran on every sample at full volume and moved about a third of them by 1-2 LSB.
Taking it out moved the `scale4` rows by at most 2 LSB, with no sign flips.

`failread` reads 400 bytes at a time, makes the 5th read after the open fail
once and decodes on, as a player does after a network hiccup; it must equal
`default` (a failed read inside the open fails the open, as it should).
The refill had already moved the unread rest of the buffer to its start, and
the retry decoded from the old offset, now other bytes: in a VBR stream, when a
small frame comes before a big one (`l3_48_stereo_vbr.mp3`; failing each read
from 1 to 140 in turn, the old code went wrong at 5 of them, the new at none).
`seekfail` makes the read right after the seek fail and decodes on. Seek used
to return before resetting anything: the audio went on from the old buffer
(261 ms on `loud_a`) and then from the new position, and the clock stayed at
261. Now both come from the target (1500 ms; the first frame differs, there was
no data to prime the filter with).

`nocheck` opens with `check_mpeg` 0, "open even if no frame turns up", and
must equal `default` (`run.sh` checks it). `l3_44_junk.mp3`, 9000 random bytes
in front of the `cbr128` audio, has no frame in its first 8 KB, so it opens only
in this row. That open used to skip the end of a normal one (empty reservoir,
`post_seek`), and its first decode returned `MPEGA_ERR_NO_SYNC`: 0 frames for a
player that stops on an error, while the same audio behind 4000 junk bytes
played without one. Now 79 frames, the first the silent Info frame.
`nocheckfail` (`--failfirst`) also makes the open's first read fail once, the
other way an open with `check_mpeg` 0 succeeds without a frame. No tag is
skipped there; the first decodes returned `MPEGA_ERR_NO_SYNC` (`BADFRAME` on
`l3_44_midstream.mp3`), 0 frames, and now ride it out like `nocheck`.

`bigbuf` asks for a 64 KB buffer with 400-byte reads and prints how many bytes
the open read (`--openbytes`). The open tops up only to 8 KB of audio (plus
any ID3 tag it skips); it used to wait for the whole 64 KB, seconds on a slow
stream. Its PCM must equal the `default` row.

`div2` and `resamp` look redundant and are not. `div2` asks for an exact 2:1
rate reduction, where `mad_f_fracpart(step)` is always zero and the linear
interpolation branch in `mad/resample.c` never runs. Only `resamp`'s
44100/24000 = 1.8375 exercises it, and therefore the resampler's cross-block
state. A resampler bug lived entirely in that branch: with only a 2:1 case in
the matrix, fixing it produced no hash change at all and looked like a no-op.

The seek rows target different failure modes and all of them are needed.
`seek` goes a third of the way in; `seek0` seeks to the very start of the audio,
where `pos == streamskip` and MPEGA_seek's junk-flush is skipped entirely; and
`seeklate` goes deep into the file. Each of the three caught a distinct bug that
the others did not: a false sync with the flush skipped (`MAD_ERROR_BADLAYER`),
a stale bit reservoir (`MAD_ERROR_BADHUFFDATA`), and repeated `MAD_ERROR_BUFLEN`
from a single-shot refill. `seekend` asks for 99999 ms: the target is held at
the end of the file, the next decode is the end of the stream, and the clock
must report the end, not the request (without a TOC it used to say 99999).

`short` caps every `Read()` at 100 bytes. `dos.doc` only promises that Read
*"usually"* fills the buffer; PIPE:, serial, CD and network handlers routinely
return less, and a plain file never does, so this is the only way the harness
reaches the partially-filled-buffer code at all. It is not a corner case:
before the fix, a stream delivered in short reads decoded **9
frames out of 77** and stopped. After it, short reads produce byte-identical
audio to a full-read decode, which is the property that actually matters: how
the bytes arrive must not change what comes out. So the whole `short` row must
equal `default`, end status included (`run.sh` checks it for every fixture).
It read 400 bytes until October 2026, and 400 happened to miss this: when
the ID3v1 tag at the end straddled a read, the file ended in
`MPEGA_ERR_NO_SYNC` instead of a clean end of file (`l3_44_stereo_id3v1.mp3`
at 100 bytes; 605 of the read sizes 1 to 8192).

`l3_44_joined.mp3` is two files joined into one, `l3_44_stereo_id3v1.mp3` and
then `l3_44_stereo_cbr128.mp3`, as `cat a.mp3 b.mp3` makes them: the first
one's ID3v1 tag and the second one's ID3v2 tag sit where the next frame should
be. The decode used to stop there with `MPEGA_ERR_NO_SYNC` after the first
file (78 of 157 frames); the tags are skipped now. 39 zero bytes after the
first file's ID3v2 tag move its frames so that, with the 100-byte reads of
`short`, only 8 bytes of the second tag's header are in the buffer when the
decoder meets it, fewer than the 10 the tag check needs: that row stopped at
the junction until the refill started from the first byte of the cut header.
The file ends in an ID3v1 tag cut short (63 of 128 bytes, a broken download).

`l3_44_stereo_hot.mp3` is `loud_a` with 7 added to every `global_gain`, as
MP3Gain makes a track 10.5 dB louder: it decodes to about three times full
scale. The long-block IMDCT and the synthesis DCT ran past Q28's +-8.0 there,
and 5969 samples of `-hifi` came out with the wrong sign (ffmpeg's float
decoder, clipped to 16 bits, as the reference); with two bits of headroom
(`MAD_F_HEADROOM`, `mad/fixed.h`) none do: rounded output stays within 1 LSB,
dithered output within 4 LSB, as much as the dither moves any track.
`-bugfix` keeps the 2004 limit of about twice full scale: its 32-bit
arithmetic overflows past it, which is undefined in C and fails ASan, so
neither `run.sh` nor `check_m68k.sh` runs this fixture for `-bugfix`.

`loop` plays to the end, seeks to 0 and plays again, as a player repeating a
track does. The skip of the cut tag at the end of `l3_44_joined.mp3` runs past
the end of the file; the seek kept it pending, and the repeat lost its first
two frames (312 frames instead of 314).

The manifest is plain text on purpose: when something changes you get a readable
diff naming the fixture and the setting, not one opaque hash.

## What it does NOT cover

Be honest about the gaps before trusting a green run:

- **`library_init.c` is not built.** All the Amiga glue (the ROMTag, the LVO
  table, `LibInit`/`Open`/`Close`/`Expunge`, and the `StackSwap()` entry
  points) is m68k-only. In particular **the stack-swap defect cannot be caught here**; it
  is a codegen defect. A check of every built library covers it instead (see
  below).
- **`support.c` is not built.** Its pool allocator, `bcopy`, `memset` and
  `malloc`/`free` are replaced by the shim and libc. This is deliberate: routing
  allocations through libc `malloc` is what lets ASan see real block boundaries
  and catch overruns. The cost is that `support.c`'s own implementations are
  untested here; `make check-m68k` runs them (see below).
- **Host is not m68k.** Same-tree-to-tree comparisons are valid; whether the
  m68k build decodes the same is what `make check-m68k` answers.
- **The 68k `CALLHOOK` is not exercised.** The `hook` rows run the hook path
  through the host `CALLHOOK` (`-D__AROS__`); the m68k inline assembly that
  loads a0/a1/a2/a4 runs only in `make check-m68k`.
- **Nothing here is a listening test.** A hash change tells you the output moved,
  not whether it moved in the right direction.

## Real music against ffmpeg (outside the harness)

Whether the output is right, not just unchanged, was checked for 3.1 on four
full tracks that are not in the repository: the host build of each flavour
against ffmpeg 6.1's float decoder (rounded and clipped to 16 bits) and its
fixed-point decoder, aligned by cross-correlation.

| Track | Kind | Length | Encoder, header | Tags | ReplayGain | Peak |
|---|---|---|---|---|---|---|
| AmiKit X13 theme | VBR 32-320 kb/s (173 on average), 48 kHz | 3:17, 8231 frames | Lavc60.31, Xing with TOC | ID3v2 169 KB with cover, ID3v1 | none | 0.65 FS |
| AmiKit X13 theme (Remix) | VBR 80-320 kb/s (205 on average), 48 kHz | 3:31, 8801 frames | Lavc60.31, Xing with TOC | ID3v2 179 KB with cover, ID3v1 | none | 1.02 FS |
| Sundial Aeon, "Startup Sequence" | CBR 320 kb/s, 44.1 kHz | 5:12, 11961 frames | LAME 3.99r, Info | ID3v2 116 KB with cover | radio -7.0 dB | 1.20 FS |
| The Future Sound of London, "Smokin Japanese Babe" | CBR 128 kb/s, 44.1 kHz | 4:59, 11464 frames | Lavc58.13, Info | ID3v2 87 KB | none | 0.96 FS |

Largest difference from the float decoder, in 16-bit LSB. The fixed-point
decoder gives the same numbers for the Hi-Fi flavours; on Sundial only for
`-hifi-forced`, because it clips the master at full scale before any
ReplayGain could bring it down:

| | X13 | Remix | Sundial | FSOL |
|---|---|---|---|---|
| `-hifi`, rounded / dithered | 1 / 4 | 1 / 4 | 1 / 4 after its -7 dB ReplayGain | 1 / 4 |
| `-hifi-forced` (dithered, no ReplayGain) | 4 | 4, clips 1 sample | 4, clips 3206 samples | 4 |
| `-bugfix`, dithered | 131 | 140 | 70 after its ReplayGain | 148 |
| samples with the wrong sign | 0 | 0 | 0 | 0 |
| `-hifi` against the build before `d4a4f2c`, rounded | 1 | 1 | 1 | 1 |

The clipped samples sit where the reference goes above full scale (1 and
3202 samples there), and they clip cleanly. Every flavour decodes every frame and ends with a clean EOF;
the 2004 original drops the last frame of both CBR tracks and ends the two
VBR tracks, whose ID3v1 tag follows the audio, with an error. The speed of
all four libraries on these tracks is in the main README.

## The stack-swap guard (outside the decode harness)

The stack-swap defect is a code-*generation* defect: at `-O2` gcc sinks the 12-byte
argument cleanup of the swapping entry points into the epilogue, where it runs
after `StackSwap()` has already restored the caller's stack pointer. The
Makefile compiles `library_init.c` with `-fno-defer-pop` to prevent it, but a
flag is easy to lose, so the property is asserted in the linked library:

```sh
python3 tests/check_swap_cleanup.py <library>
```

It runs in every `make` variant recipe, so a build that lost `-fno-defer-pop`
fails instead of shipping, and it checks a library pulled out of a release
archive with no sources needed. It fails unless all three entry points clean
up before their second `StackSwap`.

It was validated against known-good and known-bad inputs: the 2004
`mpega.library.040` from `original-archives/mpega_libmad.lha` (gcc 2.95, uses
`adda.w #12,sp`) passes, and the first 68020 rebuilds of this fork failed on
all three entry points in all three variants.

## The m68k check (vamos)

```sh
make            # the three flavours first
make check-m68k
```

Runs the three built flavours under vamos (amitools, an AmigaOS emulator for
CLI programs) through `tests/m68k_decode.c`, an m68k twin of `decode_ref`, and
compares PCM and the reported metadata byte for byte with the host
`decode_ref`: 22 fixtures x 12 settings x 3 flavours (21 for `-bugfix`, see
`l3_44_stereo_hot.mp3`), 780 runs in about five minutes. The `hookshort` setting reads through the m68k `CALLHOOK` in 400-byte
pieces and compares PCM only, because the size guess depends on the first read.
This covers what the host harness cannot: 68k byte order, the code gcc
generated for m68k, `support.c`, the library entry points and the stack swap.

vamos leaves `tc_SPLower` at 0, so by itself the library never sees a small
stack and never swaps. `lowopen` and `lowdecode` fake one (`m68k_decode
--lowstack`, 4000 bytes left): `lowopen` swaps from the open on, `lowdecode`
opens with plenty of stack and swaps only from the first decode, which then
gives the stream its swap block (it used to allocate 16 KB on every call). Both
read through a hook, and the hook checks from its own `sp` that it runs on the
library's stack; if not, `m68k_decode` prints `NOSWAP` and the run differs from
the host. Without that, a library that never swapped passed: vamos gives a real
64 KB stack, so the decoder never overflowed here (shown with
`STACK_THRESHOLD 0`), while on a 4000-byte stack it would crash. The
other settings run with a 64 KB stack (`-s 64`); with the default one the
decoder ran off its stack. It does not cover speed (vamos cycle counts are
approximate), AHI or players. Each run gets
its own directory and `--vols-base-dir`, because parallel vamos runs share a
temporary `ram:` volume otherwise. Needs `m68k-amigaos-gcc` and vamos
(`pip install amitools machine68k==0.3.0`, or `VAMOS=`); it fails without them
unless `MPEGA_ALLOW_SKIP=1`, and it refuses binaries older than the sources.

## Shim design notes

- **`-D__AROS__`** selects the portable `CALLHOOK` in `wrap_mpega.c` (the 68k
  one in the `#else` branch is inline assembly). It touches nothing else in
  the wrapper.
- **`-DSIZEOF_INT=4` used to be mandatory and is now gone.** `wrap_mpega.c` did
  not include `mad/config.h`, so `mad/fixed.h:25` saw `SIZEOF_INT` undefined,
  picked `signed long` for `mad_fixed_t`, and disagreed with every file in
  `mad/`. On m68k both are 32-bit so it was invisible; on this LP64 host it
  would have corrupted every sample. That bug is now fixed
  in the library itself, so the workaround is no longer needed. `-Wundef` is
  passed here and in the Makefile to catch the same shape again.
- **`BPTR` is a 32-bit integer**, as in the real NDK (`dos/dos.h:132`,
  `typedef long BPTR`), not a pointer. The shim maps BPTR values to host
  `FILE *` through a small table. Getting this wrong makes
  `dechandle->fh = CALLHOOK(...)` in `WRAP_MPEGA_open` (a `ULONG`) warn.

### Expected build warnings: none

**A warning here is a signal; look at why.** `run.sh` prints it and fails. The last one,
`assignment to 'BPTR' from 'void *'` from `dechandle->fh = NULL;` (`BPTR` is an
integer, `NULL` a pointer), went with the line itself: the `DecHandle` comes
from `calloc`, so it did nothing. The one before (`fastsdct` declared `y[18]`,
accessing 72 bytes in a region of 68) went away with the upstream `y[17]` fix.

## Fixtures

Mostly synthetic (tones, seeded pink noise, silence). The exception is the
three `loud` fixtures and `hot`, made from `loud_a`: 2.5 s excerpts of our
own music, the AmiKit X13 theme remix (see `make_fixtures.sh`), because only
real broadband music
reproduces the clipping and overflow classes they guard. They are committed; `make fixtures` only exists so their
provenance stays reproducible.

| Fixture | Path it exercises |
|---|---|
| `l3_44_stereo_cbr128.mp3` | the common case: MPEG-1 Layer III, 44.1 kHz, CBR |
| `l3_48_stereo_vbr.mp3` | VBR + Xing/LAME header: TOC seeking, duration math |
| `l3_22_mono_cbr64.mp3` | MPEG-2 LSF, mono |
| `l3_8_mono_cbr32.mp3` | MPEG-2.5 at 8 kHz, the rate the original decoder got wrong |
| `l2_44_stereo_cbr192.mp2` | Layer II (a different decoder path entirely) |
| `l3_44_stereo_id3.mp3` | ID3v2 tag in front: the tag-skip path |
| `l3_44_silence.mp3` | digital silence: dither behaviour on a null signal |
| `l3_44_monostereo.mp3` | mono part, then stereo part in one stream: mid-stream output reconfiguration |
| `l3_48_stereo_xing0.mp3` | Xing header with a zero frame count: duration fallback, TOC seeking |
| `l3_44_notoc.mp3` | VBR, Xing header without a TOC: seeking by the average bitrate |
| `l3_44_two_id3.mp3` | two 10000-byte ID3v2 tags in a row: both skipped, hook length from the LAME tag |
| `l3_44_junk.mp3` | 9000 random bytes in front of the audio: the open with `check_mpeg` 0 (`nocheck` row) |
| `l3_44_joined.mp3` | two files joined into one: the ID3v1 and ID3v2 tags between them are skipped; ends in a cut ID3v1 tag (`loop` row) |
| `l3_44_id3_pad_noxing.mp3` | 10000-byte ID3v2 + padding, no Xing header: the hook size fallback |
| `l3_44_midstream.mp3` | cut mid-stream, no Xing header: the first frame needs reservoir bytes from before the file |
| `l3_44_id3_8192.mp3` | ID3v2 tag whose total size is exactly 8192: the skip-loop path |
| `l3_44_stereo_id3v1.mp3` | ID3v1 tag at the end: clean EOF instead of NO_SYNC |
| `l3_48_stereo_rgain.mp3` | LAME ReplayGain tag: the fixed-point gain (the old FPU freeze) |
| `l3_44_stereo_loud_a/b/c.mp3` | loud broadband music: the Hi-Fi clipping class that tones cannot reproduce |
| `l3_44_stereo_hot.mp3` | `loud_a` 10.5 dB louder through `global_gain` (MP3Gain): decoding beyond full scale (Hi-Fi flavours only) |

Left and right carry **different** tones, so any channel mix-up or a wrong
`force_mono` downmix shows up as a hash change rather than passing silently.

### Rule for new fixes

> A fix without a fixture that **fails before it and passes after it** is not verified.

Each fix should come with the targeted fixture it needs, like
`l3_44_id3_8192.mp3` and `l3_44_id3_pad_noxing.mp3`. Still without one:

- A free-format Layer III / MPEG-2.5 long frame.
- The "frame larger than the whole buffer" guard in decode_frame. No
  test reaches it, and probably no input can: the largest frame libmad accepts
  is about 5760 bytes (free-format MPEG-2.5, see the main_data comment in
  `mad/layer3.c`) and the buffer is at least 8192. A file truncated mid-frame
  does not reach it either; it ends through the EOF path every fixture already
  takes (checked with a mutant).

## Marking experimental builds

When two builds are compared on an Amiga, give each a marker:

```sh
make mpega.library_020_all PHASE=2
```

`PHASE=N` stamps ` [PN]` into the `$VER` string, so an installed library always
identifies itself:

```
version LIBS:mpega.library
mpega.library 3.1 (29.10.26) [020] [Hi-Fi forced] [64bit] [P2] mpega.library clone v1.7 ...
```

Leave it unset for a release build.
