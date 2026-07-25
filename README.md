# mpega.library

![mpega.library Hi-Fi Edition (AmiKit)](mpega-hifi-box.png)

A faster, bugfixed version of S. Skjæret's & G. Steger's [libmad edition of
mpega.library](https://aminet.net/util/libs/mpega_libmad.lha) for AmigaOS 3
(68020), with an optional **Hi-Fi** decode. It is a drop-in replacement,
so every player just works. Copy one file to `LIBS:` and you are done.

## Installation

Pick **one** of the three editions below and copy it to `LIBS:` under the name
`mpega.library`, overwriting the old one (back it up first if you want to keep
it), then **reboot your Amiga**:

```
Copy mpega.library-hifi-forced LIBS:mpega.library
```

That is all. Start your favourite MP3 player and listen: there is nothing to
configure, and you do not need to touch your AHI or player settings.

Check what is installed with `version LIBS:mpega.library`; the edition is shown
in brackets, e.g. `[020] [Hi-Fi] [64bit]`.

## What is new in this release

CISC's original libmad-based clone is a faithful, bugfixed decoder for
AmigaOS 3. This fork keeps that foundation and goes through the
whole 020 build: two dozen real bugs fixed, an optional full-precision Hi-Fi
decode, and hardening against damaged or malicious files, without giving up
any speed. The three editions all share one source tree; only two build-time
switches separate them.

### IT IS FASTER

Same API, same players, same files: just faster. Swap one file in `LIBS:`
and the `-bugfix` edition decodes **10-25 % more audio per CPU second** than
the 2004 `mpega.library` 2.4.

### AN OPTIONAL HI-FI DECODE

`-hifi` carries every multiply at full 64-bit precision through libmad's
`FPM_64BIT` path: an **~18 dB lower arithmetic noise floor** than the
speed-optimized decode. It is the most accurate decode this library can
produce.

That precision costs real CPU: the Hi-Fi editions are **4.5-6× slower than
the 2004 original**, still above 15× realtime on AmiKit, so emulated and
accelerated machines will not notice, but a stock 68020 will. `-hifi-forced`
uses the identical decoder but always decodes at maximum fidelity, ignoring
any player request that would lower quality; see below for exactly what that
means.

**Measure before you commit to it.** `tests/bench/mpegabench` prints a
realtime factor, and anything near 1.00× means the machine cannot keep up.

### EVERY EDITION CONTAINS EVERY FIX

The three editions differ *only* in decode policy; the fixes are in all of
them. The ones you are most likely to have actually run into:

- **It crashed with players that use a small stack.** Any caller with less
  than ~8 KB free (and the AmigaDOS default is 4000 bytes) hit corrupted
  registers and a garbage return address. A rebuild-induced fault: the 2004
  binary was correct, and every modern rebuild since was not.
- **Anything that was not a plain file stopped almost immediately.** Streams
  fed through a hook that returns short reads (network, `PIPE:`, serial, CD)
  decoded **9 frames out of 77** and stopped. They now decode identically to
  a local file.
- **Every track lost its last fraction of a second** (~26 ms), and the
  reported duration was one frame short.
- **Mono threw half the music away.** `force_mono` emitted the left channel
  and discarded the right, so anything panned hard right (a guitar, a
  backing vocal) simply vanished. Both channels are now mixed.
- **Seeking under one second restarted the track**, because the position was
  truncated to whole seconds. Sub-second scrubbing did nothing at all.
- **Seeking into the middle of a file failed on some streams**, reporting an
  error on perfectly intact audio.
- **Files under 2 KB would not open**, and certain ID3v2 tag sizes corrupted
  memory.
- **A whole-machine freeze on MP3s carrying a LAME ReplayGain tag.** Every
  68020 build ever made of this fork froze hard inside `MPEGA_open()`, before
  a single frame decoded. ReplayGain is computed in fixed point now and
  actually works on 68020 for the first time.
- **A file ending in an ID3v1 tag** (nearly all of them) **reported an error
  instead of a clean end of file.** The audio always decoded correctly; only
  the final status was wrong.

**Two players can decode at the same time.** Each open stream keeps entirely
separate state, so a second player is unaffected by the first, and so is a
single stream entered from two tasks.

### HARDENED AGAINST DAMAGED AND MALICIOUS FILES

Layer I, Layer II and Layer III now check that the bits they are about to
read actually exist, which they largely did not before: a malformed or
hostile `.mp1`, `.mp2` or `.mp3` could read past the end of a frame instead of
failing cleanly. This is the fix for the 2017 libmad vulnerability family
(CVE-2017-8372 / 8373 / 8374), taken from the Debian and Fedora patches that
upstream libmad folded in at 0.16.0. Every valid stream still decodes
bit-for-bit identically; the new checks are reached only by a broken file.

See [`CHANGELOG.md`](CHANGELOG.md) for the full change list. Verified in AmiKit (AmigaAMP,
AmPlifier, TKPlayer), including two players decoding concurrently.

## Editions

One source tree builds three flavours of the compiled library; check which
one is installed with `version LIBS:mpega.library` (the edition is shown in
brackets, e.g. `[020] [Hi-Fi forced]`):

| Edition | Decoder | Player settings | Intended for |
|---|---|---|---|
| `mpega.library-bugfix` | original speed-optimized (`OPT_SPEED`/`OPT_SSO`) | honoured | real, low-power Amigas: original performance, just bugfixed |
| `mpega.library-hifi` | full 64-bit precision (`FPM_64BIT` + `OPT_ACCURACY`) | honoured | machines with CPU headroom that still want player control |
| `mpega.library-hifi-forced` | full 64-bit precision | **ignored: always maximum** | emulation / fast machines (e.g. AmiKit) |

All three contain every bug fix, and future fixes flow into all editions
automatically (the differences are two build-time switches).

## Speed: measured, not estimated

The obvious question before installing anything is *what does this cost me*.
Here is the answer, measured on the real thing rather than reasoned about:
AmiKit (68020), `tests/bench/mpegabench`, two different tracks in a single
session, with a copied-binary null control bounding the noise at under 4 %:

| Library | 3:17 VBR 48 kHz | 3:54 CBR 320 kb/s | vs the 2004 original |
|---|---|---|---|
| **2004 original** (2.4, gcc 2.95) | 100.8× | 80.2× | - |
| **`-bugfix`** | **120.5×** | **89.3×** | **10-25 % faster** |
| **`-hifi`**, **`-hifi-forced`** | 17.5× | 17.1× | **4.5-6× slower** |

*Realtime factor: seconds of audio decoded per second of CPU; below 1.00× a
machine cannot keep up. All rows come from one session so the ratios are
comparable; the Hi-Fi row is the shipping `FPM_64BIT` configuration. The
`-bugfix` gain over the original ranged from 10 to 25 % across sessions and
tracks; the Hi-Fi cost likewise varies with the material, which is why both
are ranges rather than one flattering number.*

**What the extra CPU buys.** All three editions decode the same MP3
correctly and contain the same fixes. `-hifi` spends that time on arithmetic
precision: every multiply is carried at full 64-bit width and rounded once,
rather than shifted and truncated 512 times per sample. Measured against a
high-precision reference, that is an ~18 dB lower arithmetic noise floor. On
a 16-bit output the audible difference is subtle: a quieter noise floor, not
a different mix.

**Which to pick.** On a real 68020, start with `-bugfix`: it is faster than
what you have now and fixes everything. Try `-hifi` only if `mpegabench`
shows you have the headroom to spare.

## Hi-Fi behaviour (the `-hifi-forced` edition)

Built for **AmigaOS 3 / 68020** with a modern `m68k-amigaos-gcc` (amiga-gcc,
gcc 6.5). On top of the bug fixes it always decodes at maximum fidelity,
ignoring player requests that would lower quality:

- libmad built with `FPM_64BIT` + `OPT_ACCURACY`: full-precision 64-bit
  fixed-point multiplies, and no `OPT_SSO` shortcut in the synthesis
  filterbank (libmad silently forces `OPT_SSO` when `FPM_DEFAULT` is used, so
  the Hi-Fi editions avoid it).
- Always-on **dithered** (noise-shaped) 16-bit output.
- **Full native bandwidth**: no `freq_div`/`freq_max` downsampling; the
  library decodes at the source's native sample rate and reports it via
  `dec_frequency`, so players/AHI play it at the correct rate and speed.
- Broken CRCs are always tolerated, and recoverable Layer III frame errors
  are played through (as the original did at LOW quality) instead of
  returned as errors; robustness is decoupled from the forced HIGH fidelity,
  so any file plays.
- ReplayGain is not applied (unity gain) for a faithful, clip-free decode;
  `MPEGA_scale` (player volume control) still works. `force_mono` is
  honoured.

## Requirements

**AmigaOS 3** on a **68020** or better. No FPU is required; the library is
entirely integer/fixed-point, including the ReplayGain gain calculation.

## Building

One source tree serves all platforms via `#ifdef`. For **AmigaOS 3 / 68020**,
build all three editions with:

```sh
make                              # same as: make mpega.library_020_all
```

(or individually: `mpega.library_020_bugfix` / `_020_hifi` /
`_020_hifi_forced`). This produces **`mpega.library-bugfix`**,
**`mpega.library-hifi`** and **`mpega.library-hifi-forced`**; copy the chosen
one to the Amiga as `LIBS:mpega.library`. The 68k cross toolchain (amiga-gcc,
`m68k-amigaos-gcc`) must be in `PATH`. Use GNU make (`gmake`); the `Makefile`
uses GNU pattern rules. The `-bugfix` edition builds with the original
`FPM_DEFAULT`/`OPT_SPEED`/`OPT_SSO` flags; the Hi-Fi editions build with
`FPM_64BIT` + `OPT_ACCURACY` (full-precision math, no SSO); `-hifi-forced`
additionally defines `MPEGA_FORCE_HIFI` (the only code-level switch).

The original `_040` / `_PPC` / `_OS4` / AROS targets are kept in the
`Makefile` for reference only. The PPC ones have **never been built in this
fork and cannot be built here**: `_PPC` (MorphOS + PowerUP) and `_OS4` need
`ppc-morphos-gcc` / `ppc-amigaos-gcc`, the phase5 PowerUP and AmigaOS 4 SDKs,
the `Data2Object` utility, and the 2004 Amiga include paths their recipes
hardcode (`/GCCInclude`, `/PPCInclude`, `/V/include`). The `.pup` / `.os4` /
`.elf` binaries in this repo are the untouched 2004 originals (v2.4,
17.03.04) and carry none of this fork's fixes. `make clean` removes object
files.

## Repository layout

| Path | What |
|------|------|
| `library_init.c/.h`, `library_base.h` | Library boilerplate / init |
| `wrap_mpega.c/.h` | The mpega API wrapper over libmad |
| `support.c/.h`, `support_pup.c/.h`, `startup_pup.c` | Platform support / PowerUP glue |
| `mad/` | Robert Leslie's libmad 0.15.1b **plus the hardening upstream folded in at 0.16.0**, heavily modified for 68k (decoder + `*.dat` tables). **This is the libmad that gets built.** |
| `mpega_68k_os4.S` | 68k assembler glue for the OS4 build |
| `mpega.fd`, `mpega.i` | Library FD + assembler inline interface |
| `compiler.h` | Cross-platform compiler/ABI shims |
| `Makefile` | 68020 build (the supported one); 2004 PPC / PowerUP / OS4 / MorphOS recipes kept for reference |
| `Makefile.aros`, `library_init_aros.c`, `include_aros/`, `README.AROS` | AROS i386 port |
| `mpega.library.040/.pup/.os4/.elf/.aros_x86` | **Original** prebuilt release binaries (baselines) |
| `mpega.storm`, `mpega_PPC.storm` (+ `.info`) | StormC IDE project files (see note) |
| `original-archives/` | Pristine upstream archives, byte-for-byte |

## Provenance / import history

This repo was assembled from two upstream archives, each committed pristine
under `original-archives/` and extracted into its own commit:

1. **`mpega_libmad.lha`**: the 7th release (code 3 Mar 2004; the .lha
   snapshot itself dates to 15 Jun 2004, when the OS4 binary was added). The
   canonical multi-platform Amiga distribution (68k/PPC/OS4/MorphOS).
2. **`mpega_libmad.i386.tar.bz2`**: Georg Steger's AROS i386 port. Its only
   changes to the shared sources are additive `__AROS__` / `BUILD_AROS`
   branches; the 68k path is identical to the .lha.

**Note on the StormC project files:** the original `mpega.<0xB6>` /
`mpega_PPC.<0xB6>` StormC IDE project files (and their `.info` icons) used a
non-ASCII `0xB6` byte in their names. They are renamed here to `mpega.storm` /
`mpega_PPC.storm` (+ `.info`) for filesystem and cross-platform portability.
The byte-exact originals (with the original names) live inside
`original-archives/mpega_libmad.lha`.

## Thanks

`mpega.library` is Stephane Tavenard's program; this is Sigbjørn Skjæret's
libmad-based clone of it, carried forward here. Thanks to **Midwan**, for the
initial analysis of the source and his suggestions, which set the direction
of the whole effort, and to **Philipp Lonke**, for beta testing.

- **Sigbjørn Skjæret (CISC)**: the original libmad-based mpega.library clone
- **Robert Leslie**: libmad (in [`mad/`](mad/))
- **Jarmo Laakkonen**: WarpOS version
- **Stephan Rupprecht**: OS4 version
- **Georg Steger**: AROS i386 port
- **Jan Zahurancik / AmiKit**: the bugfixed, Hi-Fi 68020 fork (2026)

Licensed under the **GNU GPL v2** (because libmad is GPL). See
[`mad/COPYING`](mad/COPYING), [`mad/COPYRIGHT`](mad/COPYRIGHT) and
[`mad/CREDITS`](mad/CREDITS). Applications using this library must be GPL too.
The complete source for this release is at
https://github.com/AmiKit/mpega_library_3 - you are entitled to the source
of any binary you receive.
