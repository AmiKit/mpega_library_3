#!/bin/bash
#
# mpega.library regression harness.
#
# Builds the REAL library sources (wrap_mpega.c + mad/) for each of the three
# shipped decode configurations against a host shim, decodes every fixture under
# several player-setting combinations, and compares a manifest of PCM and
# metadata hashes against tests/expected/.
#
# It answers exactly one question: "did this change alter what the decoder
# produces?" See tests/README.md for what it does and does not cover.
#
# Usage:
#   ./run.sh              verify against tests/expected/
#   ./run.sh --update     regenerate tests/expected/ (do this ONLY on a known-good tree)
#   ./run.sh --asan       build with ASan+UBSan and run; any report fails the run
#   ./run.sh --config=X   restrict to one configuration
#   ./run.sh --keep       keep the build directory and print its path
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS="$ROOT/tests"
CC="${CC:-gcc}"

UPDATE=0; ASAN=0; KEEP=0; ONLY=""
for a in "$@"; do
    case "$a" in
        --update)    UPDATE=1 ;;
        --asan)      ASAN=1 ;;
        --keep)      KEEP=1 ;;
        --config=*)  ONLY="${a#--config=}" ;;
        -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

# Build OUTSIDE the repo: the working copy may live on a cloud-synced drive,
# and generating hundreds of object files there is slow and sync-hostile.
BUILD="${MPEGA_BUILD_DIR:-/tmp/mpega-check.$$}"
# It is removed at the end, so it must be ours: MPEGA_BUILD_DIR=/tmp used to
# wipe /tmp.
[ -e "$BUILD" ] && { echo "MPEGA_BUILD_DIR must not exist yet: $BUILD" >&2; exit 2; }
mkdir -p "$BUILD"
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$BUILD"; }
trap cleanup EXIT

MAD_SRC="resample tag crc fixed bit timer stream frame synth layer12 layer3 huffman"

# __AROS__ selects the portable CALLHOOK in wrap_mpega.c.
#
# The -DSIZEOF_INT=4 that used to be here is gone: it existed only because
# wrap_mpega.c did not include mad/config.h, so mad_fixed_t
# widened to 64 bit in that one translation unit while mad/ kept 32. With the
# include in place the types agree by themselves, on this host and on m68k.
CFLAGS_BASE="-std=gnu89 -g -D__AROS__ -DHAVE_CONFIG_H -DNDEBUG -Wundef
 -I$TESTS/shim -I$ROOT/include_aros -I$ROOT/mad -I$ROOT
 -fno-strict-aliasing -fno-builtin-bcopy -fno-builtin-bzero
 -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-deprecated-declarations"

if [ "$ASAN" = 1 ]; then
    CFLAGS_BASE="$CFLAGS_BASE -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
else
    CFLAGS_BASE="$CFLAGS_BASE -O2"
fi

# The three shipped decode configurations, verbatim from the Makefile.
config_flags() {
    case "$1" in
        bugfix)      echo "-DFPM_DEFAULT -DOPT_SPEED -DOPT_SSO" ;;
        hifi)        echo "-DFPM_64BIT -DOPT_ACCURACY" ;;
        hifi-forced) echo "-DFPM_64BIT -DOPT_ACCURACY -DMPEGA_FORCE_HIFI" ;;
        *) return 1 ;;
    esac
}
CONFIGS="bugfix hifi hifi-forced"
[ -n "$ONLY" ] && CONFIGS="$ONLY"

# name:arguments, each combination walks a different path through the wrapper.
#
# "div2" and "resamp" are BOTH needed and are not redundant. div2 gives an exact
# 2:1 ratio, where mad_f_fracpart(step) is always zero and resample.c's linear
# interpolation branch never executes. Only a NON-integer ratio, resamp's
# 44100/24000 = 1.8375, exercises it, and therefore the resampler's
# cross-block state. A resampler bug lived entirely in that branch and was
# invisible to a 2:1 matrix.
# "short" caps every dos.library Read at 100 bytes, which is what PIPE:, serial,
# CD and network handlers actually do, dos.doc only promises Read "usually"
# fills the buffer. A plain file never short-reads, so without this row the
# entire partially-filled-buffer path is untested. A bug that lived there
# once truncated such streams after a handful of frames. Its whole row must
# equal "default". 100 is less than one 128 kbit/s frame: with 1000 the open of
# l3_44_midstream.mp3 passed even while it still decoded from a 2000-byte first
# buffer. And with 400 the ID3v1 tag of l3_44_stereo_id3v1.mp3 happened not to
# straddle a read, so the row missed it ending in MPEGA_ERR_NO_SYNC.
#
# "mixq" gives mono and stereo DIFFERENT outputs (mono: low quality, half rate;
# stereo: high quality, full rate). Only l3_44_monostereo switches between them
# mid-stream, and a half-rate option left over from the mono part used to play
# the stereo part at double speed. "q0max" is low quality with an automatic
# divider and a freq_max below the source rate, which low quality used to ignore.
# "hook" and "hookseek" read through a bs_access hook instead of dos.library,
# the path streaming players take; their PCM must equal the default and seek
# rows of the same file. "hook" reports stream_size 0, "hookseek" the real size.
# "hooklate" seeks deep with stream_size 0; a seek used to be limited to the
# guessed size (the first buffer), so its PCM must equal "seeklate".
# "scale4" is 400 % volume: -bugfix used to wrap past twice full scale and flip
# the sign of loud samples instead of clipping them. "scale100" is 100 % volume
# and must equal "default": a "+ 1" in MPEGA_scale made it a gain just above
# 1.0 that ran the gain loop and moved samples by 1-2 LSB. "bigbuf" asks for a 64 KB
# buffer with 400-byte reads and records how much the open read: it used to
# wait for the whole 64 KB. "failread" makes the 5th 400-byte read after the
# open fail once and retries the decode, as a player does after a network
# hiccup; it must equal "default". The retry used to decode from bytes the
# refill had already moved (l3_48_stereo_vbr; it takes a small frame before a
# big one).
# "seekfail" makes the read right after the seek fail and decodes on: the audio
# and the clock must come from the target, they used to come from the old buffer.
# "nocheck" opens with check_mpeg 0 and must equal "default"; l3_44_junk.mp3
# (no frame in its first 8 KB) opens only this way, and its first decode used
# to return MPEGA_ERR_NO_SYNC. "nocheckfail" also fails the open's first read
# once: the open still succeeds, and the first decodes used to return
# MPEGA_ERR_NO_SYNC or BADFRAME there too.
# "seekend" seeks past the end: the target is held at the end of the file, and
# the clock must say so; without a TOC it used to say the time asked for.
# "loop" plays to the end, seeks to 0 and plays again, as a player repeating a
# track does. l3_44_joined.mp3 ends in an ID3v1 tag cut short, whose skip runs
# past the end; the seek kept it pending and the repeat lost its first frames.
MATRIX="default:
mono:--mono
q0:--quality=0
div2:--freqdiv=2 --freqmax=24000
resamp:--freqdiv=0 --freqmax=24000
short:--shortread=100
seek:--mode=seek
seek0:--mode=seek --seekms=0
seeklate:--mode=seek --seekms=1500
seekend:--mode=seek --seekms=99999
mixq:--monoq=0 --monodiv=2
q0max:--quality=0 --freqdiv=0 --freqmax=32000
hook:--hook=1
hookseek:--hook=2 --mode=seek
hooklate:--hook=1 --mode=seek --seekms=1500
scale4:--scale=400
scale100:--scale=100
bigbuf:--bufsize=65536 --shortread=400 --openbytes
failread:--shortread=400 --failread=5
seekfail:--mode=seek --seekms=1500 --seekfail
nocheck:--nocheck
nocheckfail:--nocheck --failfirst
loop:--mode=loop"

shopt -s nullglob
FIXTURES=("$TESTS"/fixtures/*.mp3 "$TESTS"/fixtures/*.mp2)
shopt -u nullglob
if [ ${#FIXTURES[@]} -eq 0 ]; then
    echo "No fixtures found. Run tests/make_fixtures.sh first." >&2
    exit 2
fi

mkdir -p "$TESTS/expected"
rc_total=0

for cfg in $CONFIGS; do
    flags="$(config_flags "$cfg")" || { echo "unknown config: $cfg" >&2; exit 2; }
    bin="$BUILD/decode_ref-$cfg"

    printf '=== building %-12s ' "$cfg"
    srcs=("$ROOT/wrap_mpega.c" "$TESTS/shim/amiga_shim.c" "$TESTS/decode_ref.c")
    for m in $MAD_SRC; do srcs+=("$ROOT/mad/$m.c"); done

    if ! $CC $CFLAGS_BASE $flags -o "$bin" "${srcs[@]}" -lm 2> "$BUILD/build-$cfg.log"; then
        echo "FAILED"
        sed 's/^/    /' "$BUILD/build-$cfg.log" | head -40
        exit 1
    fi
    warn=$(grep -c 'warning:' "$BUILD/build-$cfg.log" || true)
    echo "ok (${warn} warnings)"
    [ "$warn" = 0 ] || { grep 'warning:' "$BUILD/build-$cfg.log" | sed 's/^/    /' | head; rc_total=1; }

    manifest="$BUILD/manifest-$cfg.txt"
    : > "$manifest"

    while IFS= read -r entry; do
        name="${entry%%:*}"
        args="${entry#*:}"
        for fx in "${FIXTURES[@]}"; do
            base="$(basename "$fx")"
            # -bugfix keeps the 2004 limit of about twice full scale, past it
            # its 32-bit arithmetic overflows (undefined in C, ASan fails it)
            [ "$cfg" = bugfix ] && [ "$base" = l3_44_stereo_hot.mp3 ] && continue
            pcm="$BUILD/out.pcm"
            info="$BUILD/out.txt"
            rm -f "$pcm" "$info"

            # shellcheck disable=SC2086
            "$bin" "$fx" "$pcm" $args > "$info" 2> "$BUILD/err.txt"
            rc=$?

            if [ "$ASAN" = 1 ] && [ -s "$BUILD/err.txt" ] && grep -qE 'runtime error|AddressSanitizer' "$BUILD/err.txt"; then
                echo "  SANITIZER $cfg/$base/$name"
                sed 's/^/    /' "$BUILD/err.txt" | head -25
                rc_total=1
            fi

            ph=$( [ -f "$pcm" ]  && sha256sum "$pcm"  | cut -c1-16 || echo "-")
            ih=$( [ -f "$info" ] && sha256sum "$info" | cut -c1-16 || echo "-")
            printf '%-26s %-8s rc=%-3s pcm=%s info=%s\n' "$base" "$name" "$rc" "$ph" "$ih" >> "$manifest"
        done
    done <<< "$MATRIX"

    # Rows tests/README.md says must agree: the same PCM through the hook as
    # through dos.library, whatever the read or buffer size, and at 100 %
    # volume; short reads give the whole default row, end status included; the
    # three fixtures with the same audio behind different ID3v2 tags give the
    # same rows (bigbuf may differ in the byte count, not in PCM). Checked
    # before the baseline is touched, so --update cannot write one that breaks
    # them.
    broken=$(awk '
        { split($4, a, "="); pcm[$1 " " $2] = a[2]; row[$1 " " $2] = $3 " " $4 " " $5; fx[$1]; name[$2] }
        function same(x, y) { if (pcm[x] != pcm[y]) print "      " x "  !=  " y }
        END {
            for (f in fx) {
                same(f " hook", f " default"); same(f " hookseek", f " seek"); same(f " hooklate", f " seeklate")
                same(f " bigbuf", f " default"); same(f " scale100", f " default")
                same(f " failread", f " default")
                if (f != "l3_44_junk.mp3") same(f " nocheck", f " default")
                if (row[f " short"] != row[f " default"]) print "      " f " short  !=  " f " default"
            }
            split("l3_44_stereo_id3.mp3 l3_44_id3_8192.mp3 l3_44_two_id3.mp3", other, " ")
            for (n in name) for (i in other) {
                x = other[i] " " n; y = "l3_44_stereo_cbr128.mp3 " n
                if (n == "bigbuf") same(x, y)
                else if (row[x] != row[y]) print "      " x "  !=  " y
            }
        }' "$manifest" | sort)
    if [ -n "$broken" ]; then
        echo "    *** rows that must agree differ ***"
        echo "$broken"
        rc_total=1
        if [ "$UPDATE" = 1 ]; then echo "    baseline NOT written"; continue; fi
    fi

    exp="$TESTS/expected/$cfg.txt"
    if [ "$UPDATE" = 1 ]; then
        cp "$manifest" "$exp"
        echo "    baseline written: tests/expected/$cfg.txt"
    elif [ ! -f "$exp" ]; then
        echo "    NO BASELINE for $cfg, run with --update on a known-good tree" >&2
        rc_total=1
    elif diff -u "$exp" "$manifest" > "$BUILD/diff-$cfg.txt"; then
        echo "    unchanged vs baseline ($(wc -l < "$manifest") runs)"
    else
        echo "    *** OUTPUT CHANGED vs baseline ***"
        sed 's/^/    /' "$BUILD/diff-$cfg.txt"
        rc_total=1
    fi
done

echo
if [ "$rc_total" = 0 ]; then echo "PASS"; else echo "FAIL"; fi
[ "$KEEP" = 1 ] && echo "build kept in $BUILD"
exit $rc_total
