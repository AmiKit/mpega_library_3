#!/usr/bin/env python3
"""
Stack-swap argument cleanup -- verify a LINKED mpega.library binary.

tests/check_stackswap.sh guards the compiler output (library_init.o) at build
time. This script checks the finished artifact instead, so a library taken out
of a release archive -- or one someone else built -- can be checked without the
sources.

Background
----------
LIB_MPEGA_open/decode_frame/seek push three arguments, call the WRAP_* worker,
and StackSwap() back to the caller's stack:

    jsr a6@(-732)          StackSwap  -- SP now on the private stack
    push arg, arg, arg     12 bytes, on the private stack
    jsr WRAP_MPEGA_xxx
    <cleanup MUST be here>
    jsr a6@(-732)          StackSwap  -- SP back on the caller's stack

If the 12-byte cleanup is missing from that window, gcc deferred it into the
epilogue, where it runs after the swap and moves the CALLER's stack pointer.
The register restore then reads the wrong slots and rts returns to garbage --
a crash for any caller with less than 8 KB of stack left (AmigaDOS defaults to
4000 bytes).

Usage
-----
    python3 tests/check_swap_cleanup.py mpega.library-hifi-forced [...]

Exit 0 if every entry point in every file is clean, 1 otherwise.
"""
import sys

# jsr a6@(-732) -- exec StackSwap
SWAP = bytes.fromhex("4eaefd24")

# The two ways the 12-byte argument cleanup is encoded.
POPS = {
    "lea sp@(12),sp": bytes.fromhex("4fef000c"),   # gcc 6.x
    "adda.w #12,sp":  bytes.fromhex("defc000c"),   # gcc 2.95 (the 2004 build)
}


def find_all(haystack, needle):
    out, i = [], haystack.find(needle)
    while i != -1:
        out.append(i)
        i = haystack.find(needle, i + 1)
    return out


def check(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        print(f"  {path}: cannot read ({e})")
        return 1

    swaps = find_all(data, SWAP)
    counts = "  ".join(f"{n}={len(find_all(data, op))}" for n, op in POPS.items())
    print(f"  {path}")
    print(f"    {len(data)} bytes, {len(swaps)} StackSwap calls, {counts}")

    if not swaps:
        print("    no StackSwap calls found -- not an m68k mpega.library?")
        return 1
    if len(swaps) % 2:
        print(f"    WARNING: odd number of StackSwap calls ({len(swaps)}); cannot pair")
        return 1

    bad = 0
    for n in range(0, len(swaps), 2):
        a, b = swaps[n], swaps[n + 1]
        window = data[a + 4:b]
        hit = [name for name, op in POPS.items() if op in window]
        if hit:
            print(f"    entry {n // 2 + 1} @{a}..{b}: cleanup present ({hit[0]}) -- ok")
        else:
            print(f"    entry {n // 2 + 1} @{a}..{b}: NO cleanup before the second swap -- BUG")
            bad += 1
    return bad


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    bad = sum(check(p) for p in argv[1:])
    print()
    print("FAIL: argument cleanup after the swap" if bad else "PASS: cleanup before the swap in every binary checked")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
