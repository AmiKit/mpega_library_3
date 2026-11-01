# mpegabench: on-Amiga decode benchmark

Everything else under `tests/` runs on a host PC and answers one question: *did
the output change?* This tool runs on the Amiga and answers the other one: *did
it get faster?*

It exists because cycle counts from the Motorola manuals are estimates, and
estimates have been wrong before. Nothing that claims a speed-up should be
committed on the strength of arithmetic alone.

## Build

```sh
make bench          # needs amiga-gcc on PATH
```

Produces `mpegabench`, a plain AmigaOS executable (~13 KB).

## Use

Copy `mpegabench` and a test MP3 to the Amiga, then:

```
mpegabench Music.mp3            # stereo, 3 passes
mpegabench Music.mp3 5          # 5 passes
mpegabench Music.mp3 3 mono     # measure the force_mono path too
```

It opens `LIBS:mpega.library` **by name**, so it measures whichever build is
installed. To compare two builds, copy one in, run, copy the other in, run
again. Every result prints that library's own `$VER` string and its
version/revision, so a measurement can never be filed against the wrong build,
which matters when three flavours and several test builds are in play.

## Reading the result

```
library : mpega.library 3.1 (29.10.26) [020] [Hi-Fi] [64bit] ...
file    : Music.mp3
mode    : stereo, 3 passes
  pass 1: 214 ticks (4.28 s)
  pass 2: 211 ticks (4.22 s)
  pass 3: 213 ticks (4.26 s)

frames  : 3841
audio   : 100304 ms
elapsed : 4220 ms (best of 3)
realtime: 23.76x
```

**`realtime` is the number that matters.** It is how many seconds of audio the
library decodes per second of CPU. Below `1.00x` the machine cannot keep up and
playback will break up; the higher above it, the more headroom there is for the
player, the filesystem and everything else.

Notes on method:

- One warm-up pass runs before the measured ones and is discarded: the first
  decode pays for the file cache and any one-time allocation.
- The **best** pass is reported, not the mean: it is the one least polluted by
  whatever else the system was doing.
- Timing uses `DateStamp()`, whose resolution is 1/50 s. Use a file of at least
  a few seconds; the tool says so explicitly if the run was too short to
  measure.
- The PCM buffers are static rather than on the stack. 4.5 KB of stack would
  otherwise eat into the headroom the library's own stack-swap logic is
  deciding about, and change what is being measured.

## Comparing two builds honestly

Run both on the **same machine, same file, same system load**, back to back.
A difference under a few percent is noise at this resolution. If that is the
size of the effect you are chasing, decode a longer file rather than trusting a
single tick.
