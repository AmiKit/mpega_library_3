#ifndef LIBRARY_BASE_H
#define LIBRARY_BASE_H

#define __USE_SYSBASE
#include <proto/exec.h>
#include <exec/execbase.h>
#include <proto/dos.h>

struct MyStackSwapStruct
{
	struct StackSwapStruct stack;
	ULONG Return;
	ULONG Args[8];	/* [0..2] call arguments
			   [3] = "this block is ours to free" (per-call fallback)
			   [4] = owning task while a swap is in progress, else 0 --
			         the claim that keeps a second entrant off this block */
};

/*
 * Stack sizing -- MEASURED, not guessed.
 *
 * Built with m68k-amigaos-gcc 6.5.0b -fstack-usage (-m68020, FPM_64BIT,
 * OPT_ACCURACY), the deepest reachable decode chain is:
 *
 *     WRAP_MPEGA_decode_frame     80
 *     mad_frame_decode            36
 *     mad_layer_III              356
 *     III_decode                7248   <- dominates; holds the xr[] work arrays
 *     III_imdct_l / III_imdct_s  488
 *     fastsdct                    64
 *                               ----
 *                               8272 bytes
 *
 * (The synthesis path -- mad_synth_frame + synth_full + dct32 -- peaks near
 * 900 bytes and is nowhere close.)
 *
 * Re-derive after any change to mad/ with:
 *     m68k-amigaos-gcc <build flags> -fstack-usage -c mad/layer3.c -o /tmp/x.o
 *     sort -k2 -rn -t$'\t' /tmp/x.su | head
 *
 * REQ_STACKSIZE was 131072 -- 16x the requirement. On a fragmented Amiga the
 * library had to find a free contiguous 128 KB block for every swapped call;
 * losing that race once aborts the track with MPEGA_ERR_MEM. 16 KB is roughly
 * twice the measured peak and is far easier to satisfy.
 *
 * STACK_THRESHOLD was 8192, i.e. BELOW the 8272-byte requirement: a caller with
 * exactly 8 KB free took the no-swap path and then overran by ~80 bytes. It now
 * sits comfortably above the measured peak, so not swapping is only chosen when
 * not swapping is actually safe.
 */
#define REQ_STACKSIZE 16384	/* stack we swap TO -- ~2x the measured 8272-byte peak */
#define STACK_THRESHOLD 12288	/* swap when the caller has LESS than this free */

/* One allocation holds the private stack and, immediately above it, the
   MyStackSwapStruct that describes it. */
#define SWAPBLK_SIZE (REQ_STACKSIZE + sizeof(struct MyStackSwapStruct))

/*
 * The per-stream swap block, and why it needs a claim
 * ---------------------------------------------------
 * MPEGA_open attaches this block to the stream so decode_frame/seek can reuse
 * it instead of allocating one per call (~38 AllocVec/FreeVec pairs per second
 * at 44.1 kHz).
 *
 * The hazard that makes a bare shared block unacceptable: StackSwap() does not
 * merely read the StackSwapStruct, it OVERWRITES it with the caller's own stack
 * bounds and SP, so that the second StackSwap can restore them (exec.doc,
 * exec.library/StackSwap). Between the two swaps that struct is therefore the
 * ONLY record of where the caller's stack was -- it exists nowhere else. A
 * second entrant that adopted the same block would rewrite stk_Lower/Upper/
 * Pointer, destroy the first entrant's saved SP, and point its own stack at the
 * one the first is currently executing on. The first entrant's second swap then
 * loads a foreign SP: wild stack, garbage return address, machine freeze. That
 * is precisely the failure the old shared PrivateBase->stackswap had.
 *
 * So the block is CLAIMED before it is adopted. Args[4] holds the owning task
 * while a swap is in progress and zero otherwise; the test-and-set runs under
 * Forbid()/Permit(), which is legitimate here because it touches only memory --
 * no Wait(), no DOS or device call (exec.doc, exec.library/Forbid).
 *
 * A second entrant simply finds the block claimed and falls back to a per-call
 * AllocVec: the exact pre-optimisation code path, still present and still
 * exercised. Nothing freezes; the contended caller just pays what everyone used
 * to pay. The optimisation is therefore a fast path, not a contract imposed on
 * callers.
 *
 * This also covers the single-task case that no threading rule could: a client
 * bs_access hook is invoked deep inside the libmad chain, where little stack
 * remains, so a hook that re-enters MPEGA_decode_frame/seek on its own stream
 * WOULD take the swap branch and reach for the very block it is standing on.
 * The claim turns that into a fallback allocation instead of a freeze.
 *
 * Two tasks each playing their OWN stream were never affected: separate
 * DecHandles, separate blocks. MPEGA_close refuses to free a block that is
 * still claimed rather than pulling a live stack out from under its owner.
 *
 * A caller that had enough stack at open time gets no block from the open; the
 * first decode_frame or seek that has to swap allocates one and attaches it to
 * the stream, claimed, so later calls reuse it.
 */

struct PrivateBase {
	struct Library pv_Lib;
	UWORD pv_Pad;

	BPTR pv_SegList;
};

#endif
