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
make check-update    # REWRITE the baselines -- only on a tree you have verified
make fixtures        # regenerate the fixtures (needs ffmpeg; they are committed)
```

No Amiga toolchain is required: the harness builds the real library sources
with the **host** compiler. It builds outside the repository (`/tmp` by default,
override with `MPEGA_BUILD_DIR`) because the working copy may live on a
cloud-synced drive where generating hundreds of object files is slow and
sync-hostile.

## What it actually builds

The real sources (`wrap_mpega.c` plus all thirteen `mad/*.c`) linked against a
small host shim, in each of the three shipped decode configurations:

| Config | Defines |
|---|---|
| `bugfix` | `-DFPM_DEFAULT -DOPT_SPEED -DOPT_SSO` |
| `hifi` | `-DFPM_64BIT -DOPT_ACCURACY` |
| `hifi-forced` | `-DFPM_64BIT -DOPT_ACCURACY -DMPEGA_FORCE_HIFI` |

`decode_ref` then drives the library the way a player does: `WRAP_MPEGA_open()`,
`WRAP_MPEGA_decode_frame()` in a loop, `WRAP_MPEGA_close()`. Each fixture is
decoded under six player-setting combinations (`default`, `mono`, `q0`, `div2`,
`resamp`, `seek`), and both the PCM and the reported stream metadata are hashed
into `expected/<config>.txt`.

`div2` and `resamp` look redundant and are not. `div2` asks for an exact 2:1
rate reduction, where `mad_f_fracpart(step)` is always zero and the linear
interpolation branch in `mad/resample.c` never runs. Only `resamp`'s
44100/24000 = 1.8375 exercises it, and therefore the resampler's cross-block
state. A resampler bug lived entirely in that branch: with only a 2:1 case in
the matrix, fixing it produced no hash change at all and looked like a no-op.

The three seek rows target different failure modes and all three are needed.
`seek` goes a third of the way in; `seek0` seeks to the very start of the audio,
where `pos == streamskip` and MPEGA_seek's junk-flush is skipped entirely; and
`seeklate` goes deep into the file. Each of the three caught a distinct bug that
the others did not: a false sync with the flush skipped (`MAD_ERROR_BADLAYER`),
a stale bit reservoir (`MAD_ERROR_BADHUFFDATA`), and repeated `MAD_ERROR_BUFLEN`
from a single-shot refill.

`short` caps every `Read()` at 1000 bytes. `dos.doc` only promises that Read
*"usually"* fills the buffer; PIPE:, serial, CD and network handlers routinely
return less, and a plain file never does, so this is the only way the harness
reaches the partially-filled-buffer code at all. It is not a corner case:
before the fix, a stream delivered in short reads decoded **9
frames out of 77** and stopped. After it, short reads produce byte-identical
audio to a full-read decode, which is the property that actually matters: how
the bytes arrive must not change what comes out.

The manifest is plain text on purpose: when something changes you get a readable
diff naming the fixture and the setting, not one opaque hash.

## What it does NOT cover

Be honest about the gaps before trusting a green run:

- **`library_init.c` is not built.** All the Amiga glue (the ROMTag, the LVO
  table, `LibInit`/`Open`/`Close`/`Expunge`, and the `StackSwap()` entry
  points) is m68k-only. In particular **the stack-swap defect cannot be caught here**; it
  is a codegen defect. Two separate checks cover it instead (see below).
- **`support.c` is not built.** Its pool allocator, `bcopy`, `bzero` and
  `malloc`/`free` are replaced by the shim and libc. This is deliberate: routing
  allocations through libc `malloc` is what lets ASan see real block boundaries
  and catch overruns. The cost is that `support.c`'s own implementations are
  untested here.
- **Host is not m68k.** Same-tree-to-tree comparisons are valid; absolute
  equality with a real Amiga decode is not claimed. Endianness-sensitive code
  would behave differently (the host is little-endian, m68k is big-endian).
- **The `bs_access` hook path is not exercised.** The fixtures use the
  `dos.library` path. `CALLHOOK` compiles but is never called. (Short reads,
  the other thing a hook-based client brings, *are* covered; see `--shortread`
  below.)
- **Nothing here is a listening test.** A hash change tells you the output moved,
  not whether it moved in the right direction.

## The stack-swap guards (outside the decode harness)

The stack-swap defect is a code-*generation* defect: at `-O2` gcc sinks the 12-byte
argument cleanup of the swapping entry points into the epilogue, where it runs
after `StackSwap()` has already restored the caller's stack pointer. The
Makefile compiles `library_init.c` with `-fno-defer-pop` to prevent it, but a
flag is easy to lose, so the property is asserted in the generated code:

```sh
make check-stackswap                          # build time: checks library_init.o
python3 tests/check_swap_cleanup.py <library> # release time: checks a linked binary
```

The first mirrors the Makefile flags, compiles, disassembles with
`m68k-amigaos-objdump`, and fails unless all three entry points clean up before
their second `StackSwap`. The second works on a finished library, useful for
checking something pulled out of a release archive, with no sources needed.

Both were validated against known-good and known-bad inputs: the 2004
`mpega.library.040` (gcc 2.95, uses `adda.w #12,sp`) passes, and builds made
without `-fno-defer-pop` fail on all three entry points in all three variants.

## Shim design notes

- **`-D__AROS__`** selects the portable `CALLHOOK` in `wrap_mpega.c:64` (the 68k
  one at `:138` is inline assembly) and makes `compiler.h` expand its m68k
  attributes to nothing. It touches nothing else in the wrapper.
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
  `wrap_mpega.c:802` (`fh = CALLHOOK(...)`, a `ULONG`) warn.

### Expected build warnings: exactly 2 per config

Both are host artifacts, not library defects. **If the count changes, look at
why**; that is a signal.

1. `wrap_mpega.c:749: assignment to 'BPTR' from 'void *'`, caused by
   `dechandle->fh = NULL;`. On AmigaOS `NULL` is `0L` (`exec/types.h:181`), an
   integer, so the real build is clean. glibc's `NULL` is `((void *)0)`, hence
   the warning here.
2. `mad/layer3.c:1680: 'fastsdct' accessing 72 bytes in a region of size 68`.
   `fastsdct` is declared `y[18]` but only ever writes even indices up to
   `y[16]`. Called as `fastsdct(tmp, &X[1])` it therefore touches at most `X[17]`,
   which is in bounds. The declaration overstates what the function needs; the
   access is safe. Cosmetic libmad inaccuracy, not an overflow.

## Fixtures

Synthetic (tones, seeded pink noise, silence), so they carry no third-party
copyright and can live in a GPL repository. They are committed;
`make fixtures` only exists so their provenance stays reproducible.

| Fixture | Path it exercises |
|---|---|
| `l3_44_stereo_cbr128.mp3` | the common case: MPEG-1 Layer III, 44.1 kHz, CBR |
| `l3_48_stereo_vbr.mp3` | VBR + Xing/LAME header: TOC seeking, duration math |
| `l3_22_mono_cbr64.mp3` | MPEG-2 LSF, mono |
| `l3_8_mono_cbr32.mp3` | MPEG-2.5 at 8 kHz, the rate the original decoder got wrong |
| `l2_44_stereo_cbr192.mp2` | Layer II (a different decoder path entirely) |
| `l3_44_stereo_id3.mp3` | ID3v2 tag in front: the tag-skip path |
| `l3_44_silence.mp3` | digital silence: dither behaviour on a null signal |

Left and right carry **different** tones, so any channel mix-up or a wrong
`force_mono` downmix shows up as a hash change rather than passing silently.

### Rule for new fixes

> A fix without a fixture that **fails before it and passes after it** is not verified.

Each fix should come with the targeted fixture it needs. Known ones still to
build:

- an ID3v2 tag whose total size is an exact multiple of 8192
- a free-format Layer III / MPEG-2.5 long frame
- a stream truncated mid-frame (the zero-progress guard)
- a stereo file concatenated with a mono file (mid-stream format change)

## Marking experimental builds

When two builds are compared on an Amiga, give each a marker:

```sh
make mpega.library_020_all PHASE=2
```

`PHASE=N` stamps ` [PN]` into the `$VER` string, so an installed library always
identifies itself:

```
version LIBS:mpega.library
mpega.library 3.0 (25.07.26) [020] [Hi-Fi forced] [P2] ...
```

If a marked build misbehaves on real hardware, rebuild the other one and
compare; the marker tells you which one you are listening to.
