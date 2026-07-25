#!/bin/bash
#
# Guard for the stack-swap argument cleanup (CRITICAL).
#
# library_init.c's three swapping entry points do:
#
#     StackSwap(&sw->stack)      ; SP -> private stack
#     push arg, arg, arg         ; 12 bytes, ON THE PRIVATE STACK
#     jsr WRAP_MPEGA_xxx
#     <the 12-byte cleanup MUST happen here, while still on that stack>
#     StackSwap(&sw->stack)      ; SP -> back to the caller
#
# With gcc's default -fdefer-pop the cleanup is sunk into the epilogue and runs
# AFTER the second swap, corrupting the caller's SP, the restored registers and
# the return address. -fno-defer-pop in the Makefile prevents that, but a flag
# is easy to lose in a refactor, so this script asserts the property in the
# GENERATED CODE rather than trusting the flag.
#
# Exit 0 = all three entry points are correct.
#
set -uo pipefail

ROOT="${MPEGA_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CC="${CROSS_CC:-m68k-amigaos-gcc}"
OBJDUMP="${CROSS_OBJDUMP:-m68k-amigaos-objdump}"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "SKIP: $CC not on PATH (try: export PATH=/opt/amiga/bin:\$PATH)"
    if [ "${MPEGA_ALLOW_SKIP:-0}" = 1 ]; then
        exit 0
    fi
    echo "FAIL: the stack-swap guard did not run, so nothing was verified. A guard that"
    echo "      reports success without compiling anything is worse than no guard."
    echo "      Set MPEGA_ALLOW_SKIP=1 to accept a skip as a pass."
    exit 1
fi

OBJ="$(mktemp -d)/library_init.o"
trap 'rm -rf "$(dirname "$OBJ")"' EXIT

# Mirror the Makefile: the same flags, including -fno-defer-pop.
FLAGS="-O2 -fomit-frame-pointer -fno-strict-aliasing -noixemul -nostartfiles -m68020
       -I$ROOT/mad -I$ROOT/include_aros -DHAVE_CONFIG_H -DNDEBUG
       -DFPM_64BIT -DOPT_ACCURACY -fno-defer-pop"

# shellcheck disable=SC2086
if ! (cd "$ROOT" && $CC $FLAGS -c library_init.c -o "$OBJ" 2>/dev/null); then
    echo "FAIL: could not compile library_init.c"
    exit 1
fi

"$OBJDUMP" -d "$OBJ" | awk '
    # StackSwap is exec LVO -732.
    /jsr a6@\(-732\)/ {
        if (!inswap) { inswap = 1; swaps++; call = 0; pop = 0; next }
        # second swap of the pair, adjudicate
        pairs++
        if (call && !pop) {
            printf "  FAIL  pair %d (%s): argument cleanup does NOT precede the second StackSwap\n", pairs, fn
            bad++
        } else if (call) {
            printf "  ok    pair %d (%s): cleanup at %s precedes the second StackSwap\n", pairs, fn, popat
        } else {
            printf "  ok    pair %d (%s): no arguments pushed, nothing to clean up\n", pairs, fn
        }
        inswap = 0
        next
    }
    # remember which function we are in
    /^[0-9a-f]+ <_/ { fn = $2; gsub(/[<>:]/, "", fn) }

    inswap && /jsr .*<_WRAP_/ { call = 1; next }

    # any instruction that moves the stack pointer: lea sp@(N),sp / add?? #N,sp
    inswap && call && (/lea sp@\([0-9]+\),sp/ || /add[aq]?[wl]? #-?[0-9]+,sp/) {
        pop = 1
        popat = $1
        next
    }
    END {
        printf "\n  %d StackSwap calls, %d pairs, %d bad\n", swaps + pairs, pairs, bad
        if (pairs != 3)
            printf "  WARNING: expected 3 swapping entry points, found %d\n", pairs
        exit (bad > 0 || pairs != 3) ? 1 : 0
    }
'
rc=$?

echo
if [ $rc -eq 0 ]; then
    echo "PASS: stack-swap guard, all swapping entry points clean up before swapping back."
else
    echo "FAIL: the stack-swap cleanup has regressed. See the comment above library_init.o in the Makefile."
fi
exit $rc
