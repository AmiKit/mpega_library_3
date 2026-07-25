#!/bin/sh
#
# Guard: the built library must not reference the IEEE math libraries.
#
# On a 68020-without-FPU build, gcc compiles every double/float operation into
# a call through mathieeedoubbas.library / mathieeedoubtrans.library. This
# library never opens them (-nostartfiles skips the libnix autoinit that
# would), so the bases stay at the $FFFFFFFF sentinel and the first use jumps
# through it, an odd-address Address Error that freezes the whole machine.
#
# That is not hypothetical: two double-arithmetic lines in wrap_mpega.c once
# froze AmigaOS solid on any MP3 carrying a LAME ReplayGain tag. The fix
# removed all floating point from the library; this guard makes the property
# permanent. If it ever fires, someone reintroduced a float, rewrite it in
# fixed point, or open the math libraries in LibInit and accept the new
# runtime dependency.
#
# Usage: check_nofloat.sh <binary> [binary ...]

status=0

for bin in "$@"; do
    if [ ! -f "$bin" ]; then
        echo "check_nofloat: $bin: not found" >&2
        status=1
        continue
    fi
    if grep -qai 'mathieee' "$bin"; then
        echo "check_nofloat: FAIL: $bin references $(grep -oai 'mathieee[a-z]*\.library' "$bin" | sort -u | tr '\n' ' ')" >&2
        echo "check_nofloat: floating point has crept back into the library." >&2
        status=1
    else
        echo "check_nofloat: ok: $bin"
    fi
done

exit $status
