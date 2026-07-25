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
mkdir -p "$BUILD"
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$BUILD"; }
trap cleanup EXIT

MAD_SRC="filter resample tag crc fixed bit timer stream frame synth layer12 layer3 huffman"

# __AROS__ selects the portable CALLHOOK and neutralises compiler.h's m68k
# attributes.
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
# "short" caps every dos.library Read at 1000 bytes, which is what PIPE:, serial,
# CD and network handlers actually do, dos.doc only promises Read "usually"
# fills the buffer. A plain file never short-reads, so without this row the
# entire partially-filled-buffer path is untested. A bug that lived there
# once truncated such streams after a handful of frames.
MATRIX="default:
mono:--mono
q0:--quality=0
div2:--freqdiv=2 --freqmax=24000
resamp:--freqdiv=0 --freqmax=24000
short:--shortread=1000
seek:--mode=seek
seek0:--mode=seek --seekms=0
seeklate:--mode=seek --seekms=1500"

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

    manifest="$BUILD/manifest-$cfg.txt"
    : > "$manifest"

    while IFS= read -r entry; do
        name="${entry%%:*}"
        args="${entry#*:}"
        for fx in "${FIXTURES[@]}"; do
            base="$(basename "$fx")"
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
