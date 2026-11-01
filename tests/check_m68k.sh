#!/bin/bash
#
# m68k check: the three BUILT flavours (mpega.library-bugfix/-hifi/-hifi-forced
# in the repo root) run under vamos (amitools) and are compared byte for byte
# with the host decode_ref of tests/run.sh: PCM and the reported metadata.
#
# The host harness proves what the sources do; this proves the m68k binaries do
# the same: 68k byte order, the code gcc generated for m68k, support.c (pool
# allocator, bcopy), the library entry points, the stack swap and the 68k
# CALLHOOK, none of which the host harness builds. vamos leaves tc_SPLower at 0,
# so the library never sees a low stack by itself; lowopen and lowdecode fake
# one (m68k_decode --lowstack) to make it swap, and their hook checks from its
# own sp that the library really did. It says nothing about speed, AHI
# or players. See tests/README.md.
#
# Needs m68k-amigaos-gcc and vamos. FAILS without them; MPEGA_ALLOW_SKIP=1
# accepts a skip as a pass. Run `make` first.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS="$ROOT/tests"
CC="${CROSS_CC:-m68k-amigaos-gcc}"
VAMOS="${VAMOS:-$(command -v vamos || echo "$HOME/amitools-venv/bin/vamos")}"

skip() {
    echo "SKIP: $1"
    [ "${MPEGA_ALLOW_SKIP:-0}" = 1 ] && exit 0
    echo "FAIL: the m68k check did not run. MPEGA_ALLOW_SKIP=1 accepts a skip as a pass."
    exit 1
}
command -v "$CC" >/dev/null 2>&1 || skip "$CC not on PATH (try: export PATH=/opt/amiga/bin:\$PATH)"
[ -x "$VAMOS" ] || skip "vamos not found (pip install amitools machine68k==0.3.0, or set VAMOS=)"

for ed in bugfix hifi hifi-forced; do
    lib="$ROOT/mpega.library-$ed"
    [ -f "$lib" ] || { echo "FAIL: $lib missing, run make first"; exit 1; }
    stale=$(find "$ROOT"/*.c "$ROOT"/*.h "$ROOT"/Makefile "$ROOT"/mad -newer "$lib" \( -name '*.[ch]' -o -name Makefile \) | head -1)
    [ -z "$stale" ] || { echo "FAIL: $stale is newer than $lib, run make first"; exit 1; }
done

W="${MPEGA_BUILD_DIR:-/tmp/mpega-m68k.$$}"
# removed at the end, so never a folder that was there before
[ -e "$W" ] && { echo "FAIL: MPEGA_BUILD_DIR must not exist yet: $W"; exit 1; }
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

# host decode_ref: the same three configurations, built by the harness itself
MPEGA_BUILD_DIR="$W/host" bash "$TESTS/run.sh" --keep > "$W/host.log" 2>&1
[ -x "$W/host/decode_ref-hifi-forced" ] || { echo "FAIL: host harness did not build"; tail "$W/host.log"; exit 1; }
"$CC" -O2 -Wall -fno-common -noixemul -m68020 -I"$ROOT/include_aros" -o "$W/m68k_decode" "$TESTS/m68k_decode.c" || exit 1

# name | m68k args | host args | compared (both: PCM + stdout, pcm: PCM only;
# hookshort's size guess depends on the first read, which is 400 bytes there)
MODES="default||||both
q0|--quality=0|--quality=0|both
mono|--mono|--mono|both
resamp|--freqdiv=0 --freqmax=24000|--freqdiv=0 --freqmax=24000|both
seek|--mode=seek|--mode=seek|both
seeklate|--mode=seek --seekms=1500|--mode=seek --seekms=1500|both
scale4|--scale=400|--scale=400|both
mixq|--monoq=0 --monodiv=2|--monoq=0 --monodiv=2|both
hookshort|--hook=1 --shortread=400|--hook=1|pcm
hookseek|--hook=2 --mode=seek|--hook=2 --mode=seek|both
lowopen|--lowstack=2 --hook=1 --mode=seek|--hook=1 --mode=seek|both
lowdecode|--lowstack=1 --hook=2 --mode=seek|--hook=2 --mode=seek|both"

job() {   # job <flavour> <fixture path> <mode line>
    local ed=$1 fx=$2 line=$3 name m68 host cmp f d
    IFS='|' read -r name m68 host cmp <<< "$line"
    f=$(basename "$fx")
    # one directory and one vamos volume base per run: runs sharing them clash
    d="$W/j.$ed.$f.$name"
    mkdir -p "$d/libs" "$d/work"
    cp "$ROOT/mpega.library-$ed" "$d/libs/mpega.library"
    cp "$W/m68k_decode" "$d/"; cp "$fx" "$d/work/"
    # -s 64: vamos leaves tc_SPLower at 0, so without --lowstack the library
    # cannot see a small stack and does not swap; the decoder needs about 8.3 KB
    # shellcheck disable=SC2086
    ( cd "$d" && timeout 600 "$VAMOS" -q -C 68020 -s 64 --vols-base-dir "$d/vols" -V "vt:$d" -a LIBS:vt:libs \
        -- vt:m68k_decode "vt:work/$f" vt:work/o.pcm $m68 > m.txt 2> m.err )
    # shellcheck disable=SC2086
    "$W/host/decode_ref-$ed" "$fx" "$d/h.pcm" $host > "$d/h.txt" 2>/dev/null
    local p=same t=same
    cmp -s "$d/work/o.pcm" "$d/h.pcm" || p=DIFF
    cmp -s "$d/m.txt" "$d/h.txt" || t=DIFF
    [ "$cmp" = pcm ] && t=-
    echo "$ed $f $name pcm=$p stdout=$t"
    [ "$p$t" = samesame ] || [ "$p$t" = same- ] && rm -rf "$d"
}
export -f job
export W ROOT VAMOS

for ed in bugfix hifi hifi-forced; do
    for fx in "$TESTS"/fixtures/*.mp3 "$TESTS"/fixtures/*.mp2; do
        # -bugfix overflows past twice full scale by design, see run.sh
        [ "$ed" = bugfix ] && [ "$(basename "$fx")" = l3_44_stereo_hot.mp3 ] && continue
        while IFS= read -r line; do printf '%s\0%s\0%s\0' "$ed" "$fx" "$line"; done <<< "$MODES"
    done
done | xargs -0 -n 3 -P "$(nproc 2>/dev/null || echo 4)" bash -c 'job "$0" "$1" "$2"' > "$W/results.txt"

total=$(wc -l < "$W/results.txt")
bad=$(grep -c DIFF "$W/results.txt")
echo "m68k vs host: $total runs, $bad differ"
if [ "$bad" -ne 0 ]; then
    grep DIFF "$W/results.txt" | sort | head -20
    first=$(grep DIFF "$W/results.txt" | sort | head -1 | awk '{print "j."$1"."$2"."$3}')
    echo "first difference, vamos stderr:"; head -5 "$W/$first/m.err"
    echo "FAIL"
    exit 1
fi
echo "PASS"
