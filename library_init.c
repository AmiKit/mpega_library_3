#include "library_init.h"

#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#include "wrap_mpega.h"
#include "support.h"

/* Reserved library-function and dummy entrypoint */
LONG LIB_Reserved(void)
{
	return 0;
}

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

ULONG LibVectors[] = {
	(ULONG) &LIB_Open,
	(ULONG) &LIB_Close,
	(ULONG) &LIB_Expunge,
	(ULONG) &LIB_Reserved,

	(ULONG) &LIB_MPEGA_open,
	(ULONG) &LIB_MPEGA_close,
	(ULONG) &LIB_MPEGA_decode_frame,
	(ULONG) &LIB_MPEGA_seek,
	(ULONG) &LIB_MPEGA_time,
	(ULONG) &LIB_MPEGA_find_sync,
	(ULONG) &LIB_MPEGA_scale,
	0xFFFFFFFF
};

struct LibInitStruct
{
	ULONG	LibSize;
	void	*FuncTable;
	void	*DataTable;
	void	(*InitFunc)(void);
};

struct LibInitStruct LibInitStruct = {
	sizeof(struct PrivateBase),
	LibVectors,
	NULL,
	(void (*)(void)) &LibInit
};

static const char LibVersion[] = "$VER: " LIB_ID;

static const struct Resident RomTag __attribute__((__aligned__(2))) = {
	RTC_MATCHWORD,
	(struct Resident *)&RomTag,
	(APTR)(&RomTag + 1),
	RTF_AUTOINIT,
	LIB_VERSION,
	NT_LIBRARY,
	0,
	LIB_NAME,
	(char *)&LibVersion[6],
	&LibInitStruct
};

static void LibCleanup(void)
{
	RemoveSupport();

	if (DOSBase)
	{
		CloseLibrary((struct Library *)DOSBase);
		DOSBase = NULL;
	}
}

BPTR LibExpunge(struct PrivateBase *PrivateBase)
{
	if ((PrivateBase->pv_Lib.lib_OpenCnt == 0))
	{
		BPTR seglist;

		seglist = PrivateBase->pv_SegList;
		Remove(&PrivateBase->pv_Lib.lib_Node);
		LibCleanup();

		FreeMem((APTR)((ULONG)(PrivateBase) - (ULONG)(PrivateBase->pv_Lib.lib_NegSize)),
				PrivateBase->pv_Lib.lib_NegSize + PrivateBase->pv_Lib.lib_PosSize);

		return seglist;
	} else PrivateBase->pv_Lib.lib_Flags |= LIBF_DELEXP;

	return 0L;
}

struct Library * LibInit(REG(d0, struct PrivateBase *PrivateBase), REG(a0, BPTR SegList), REG(a6, struct ExecBase *sysbase))
{
	SysBase = sysbase;

	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36)))
	{
		if (InitSupport())
		{
			PrivateBase->pv_Lib.lib_Revision     = LIB_REVISION;
			PrivateBase->pv_SegList              = SegList;

			return &PrivateBase->pv_Lib;
		}
	}

	LibCleanup();

	/* BUGFIX: free the library base itself. The AUTOINIT contract is
	   explicit -- exec.doc, InitResident: "If the initialization function fails,
	   the device memory must be manually deallocated, then NULL returned in D0."
	   Nobody frees it for us, so every failed init leaked the base (~164 bytes of
	   MEMF_PUBLIC) permanently -- and a failed init happens precisely when memory
	   is already short. Mirrors LibExpunge's own teardown. */
	FreeMem((APTR)((ULONG)(PrivateBase) - (ULONG)(PrivateBase->pv_Lib.lib_NegSize)),
			PrivateBase->pv_Lib.lib_NegSize + PrivateBase->pv_Lib.lib_PosSize);

	return NULL;
}

struct Library * LIB_Open(REG(a6, struct PrivateBase *PrivateBase))
{
	PrivateBase->pv_Lib.lib_Flags &= ~LIBF_DELEXP;
	PrivateBase->pv_Lib.lib_OpenCnt++;

	return &PrivateBase->pv_Lib;
}

BPTR LIB_Close(REG(a6, struct PrivateBase *PrivateBase))
{
	if (PrivateBase->pv_Lib.lib_OpenCnt > 0) PrivateBase->pv_Lib.lib_OpenCnt--;

	if ((PrivateBase->pv_Lib.lib_OpenCnt == 0) && (PrivateBase->pv_Lib.lib_Flags & LIBF_DELEXP))
		return LibExpunge(PrivateBase);

	return 0L;
}

BPTR LIB_Expunge(REG(a6, struct PrivateBase *PrivateBase))
{
	return LibExpunge(PrivateBase);
}

/* TRUE when the caller has less than STACK_THRESHOLD of stack left, so the
   decoder (about 8.3 KB deep, see library_base.h) must run on a private one. */
static int StackIsLow(void)
{
	register char *stackptr __asm("sp");

	__asm("":"=r"(stackptr));	/* force gcc to acknowledge usage of the sp register */

	return (stackptr - (char *)FindTask(NULL)->tc_SPLower) < STACK_THRESHOLD;
}

MPEGA_STREAM * LIB_MPEGA_open(REG(a6, struct PrivateBase *PrivateBase), REG(a0, char *stream_name), REG(a1, MPEGA_CTRL *ctrl), REG(a4, APTR a4base))
{
	if (StackIsLow())
	{
		/* Reentrant stack swap: the StackSwapStruct and the argument copies live
		   in the freshly AllocVec'd block (one per call), NOT in the shared
		   library base, and are reached through a callee-saved register (a3) that
		   survives StackSwap(). This makes concurrent calls from several tasks
		   safe -- the old shared PrivateBase->stackswap could be clobbered by a
		   second task mid-swap, corrupting the saved SP -> machine freeze.

		   BUILD REQUIREMENT -- this file MUST be compiled
		   with -fno-defer-pop. At -O2 gcc otherwise sinks the 12-byte argument
		   cleanup for the WRAP_* call below into the function epilogue, where it
		   executes AFTER the second StackSwap() and therefore adjusts the
		   CALLER's stack pointer to pop bytes that lived on the swapped-away
		   stack. The following register restore then reads the wrong slots and
		   rts returns to garbage. The Makefile passes the flag, and every build
		   checks the linked library for it (tests/check_swap_cleanup.py). */
		register struct MyStackSwapStruct *sw __asm("a3");
		char *blk = AllocVec(SWAPBLK_SIZE, MEMF_PUBLIC);

		if (!blk) return NULL;

		sw = (struct MyStackSwapStruct *)(blk + REQ_STACKSIZE);
		sw->Args[0] = (ULONG)stream_name;
		sw->Args[1] = (ULONG)ctrl;
		sw->Args[2] = (ULONG)a4base;
		sw->stack.stk_Lower = blk;
		sw->stack.stk_Pointer = blk + REQ_STACKSIZE;
		sw->stack.stk_Upper = (ULONG)(blk + REQ_STACKSIZE);

		StackSwap(&sw->stack);
		sw->Return = (ULONG)WRAP_MPEGA_open((char *)sw->Args[0], (MPEGA_CTRL *)sw->Args[1], (APTR)sw->Args[2]);
		StackSwap(&sw->stack);

		{
			ULONG r = sw->Return;
			char *b = (char *)sw - REQ_STACKSIZE;

			/* Hand the block to the stream rather than freeing it: decode_frame
			   and seek then reuse it instead of allocating one per call. It is
			   released in LIB_MPEGA_close. On failure there is no stream to hang
			   it on, so free it here.

			   Args[4] is the claim word (see library_base.h). AllocVec does not
			   clear memory, so it must be zeroed before the block becomes
			   visible to another entrant, or the very first decode_frame would
			   read garbage, believe the block busy and never use it. */
			if (r)
			{
				sw->Args[4] = 0;
				WRAP_MPEGA_set_swapblk((MPEGA_STREAM *)r, b);
			}
			else FreeVec(b);

			return (MPEGA_STREAM *)r;
		}
	} else {
		return WRAP_MPEGA_open(stream_name, ctrl, a4base);
	}
}

void LIB_MPEGA_close(REG(a6, struct PrivateBase *PrivateBase), REG(a0, MPEGA_STREAM *mpega_stream), REG(a4, APTR a4base))
{
	{
		/* Release the per-stream swap block. Read it BEFORE the close:
		   WRAP_MPEGA_close frees the DecHandle it lives on.

		   Never free a block that is still claimed. "close does not swap stacks"
		   says nothing about whether somebody ELSE is currently swapped onto it
		   -- and freeing a live stack hands it to the next AllocVec, or lets
		   FreeVec coalesce it with a neighbour, corrupting the owner's frames
		   and return addresses. If the claim is set we deliberately leak the
		   block (16 KB, once) rather than do that: the caller closing a stream
		   another task is still decoding is already misusing the library, and a
		   leak is a far better outcome than a freeze. */
		char *blk;

		Forbid();
		blk = (char *)WRAP_MPEGA_get_swapblk(mpega_stream);
		if (blk && ((struct MyStackSwapStruct *)(blk + REQ_STACKSIZE))->Args[4])
			blk = NULL;
		Permit();

		WRAP_MPEGA_close(mpega_stream, a4base);

		if (blk) FreeVec(blk);
	}
}

/* The swap block for one decode_frame or seek call: the stream's own block
   when it can be claimed, otherwise a fresh one with Args[3] = 1, which the
   caller frees after the call. Args[3] lives in the block, not in a local:
   locals are addressed through sp, which is swapped away, and taking the
   address of one gives the caller a frame pointer that the swap check rejects.
   The claim (Args[4]) is what keeps this safe: StackSwap overwrites the
   StackSwapStruct with the caller's SP, so two entrants sharing one block
   would destroy each other's only record of where to return to; see
   library_base.h. A stream opened with enough stack has no block yet and keeps
   the fresh one, claimed, instead of allocating on every call. Forbid()/
   Permit() is legitimate here -- memory access only, no Wait, no DOS call.
   NULL when out of memory. */
static char *ClaimSwapBlock(MPEGA_STREAM *mpega_stream)
{
	struct MyStackSwapStruct *sw;
	char *blk;

	Forbid();
	blk = (char *)WRAP_MPEGA_get_swapblk(mpega_stream);

	if (blk)
	{
		sw = (struct MyStackSwapStruct *)(blk + REQ_STACKSIZE);

		if (sw->Args[4])
			blk = NULL;	/* busy -- take the slow path instead of the block */
		else
		{
			sw->Args[4] = (ULONG)FindTask(NULL);
			sw->Args[3] = 0;
		}
	}
	Permit();

	if (!blk && (blk = AllocVec(SWAPBLK_SIZE, MEMF_PUBLIC)))
	{
		sw = (struct MyStackSwapStruct *)(blk + REQ_STACKSIZE);
		sw->Args[3] = 1;

		Forbid();
		if (mpega_stream && !WRAP_MPEGA_get_swapblk(mpega_stream))
		{
			sw->Args[4] = (ULONG)FindTask(NULL);
			sw->Args[3] = 0;
			WRAP_MPEGA_set_swapblk(mpega_stream, blk);
		}
		Permit();
	}

	return blk;
}

LONG LIB_MPEGA_decode_frame(REG(a6, struct PrivateBase *PrivateBase), REG(a0, MPEGA_STREAM *mpega_stream), REG(a1, WORD *pcm[MPEGA_MAX_CHANNELS]), REG(a4, APTR a4base))
{
	if (StackIsLow())
	{
		/* Stack swap. The StackSwapStruct and the argument copies live in the
		   block ClaimSwapBlock() hands out, NOT in the shared library base, and
		   are reached through a callee-saved register (a3) that survives
		   StackSwap(). Same BUILD REQUIREMENT as in LIB_MPEGA_open. */
		register struct MyStackSwapStruct *sw __asm("a3");
		char *blk = ClaimSwapBlock(mpega_stream);

		if (!blk) return MPEGA_ERR_MEM;

		sw = (struct MyStackSwapStruct *)(blk + REQ_STACKSIZE);
		sw->Args[0] = (ULONG)mpega_stream;
		sw->Args[1] = (ULONG)pcm;
		sw->Args[2] = (ULONG)a4base;
		sw->stack.stk_Lower = blk;
		sw->stack.stk_Pointer = blk + REQ_STACKSIZE;
		sw->stack.stk_Upper = (ULONG)(blk + REQ_STACKSIZE);

		StackSwap(&sw->stack);
		sw->Return = (ULONG)WRAP_MPEGA_decode_frame((MPEGA_STREAM *)sw->Args[0], (WORD **)sw->Args[1], (APTR)sw->Args[2]);
		StackSwap(&sw->stack);

		{
			ULONG r = sw->Return;

			if (sw->Args[3]) FreeVec((char *)sw - REQ_STACKSIZE);
			else sw->Args[4] = 0;	/* release the claim on the stream's block */

			return (LONG)r;
		}
	} else {
		return WRAP_MPEGA_decode_frame(mpega_stream, pcm, a4base);
	}
}

LONG LIB_MPEGA_seek(REG(a6, struct PrivateBase *PrivateBase), REG(a0, MPEGA_STREAM *mpega_stream), REG(d0, ULONG ms_time_position), REG(a4, APTR a4base))
{
	if (StackIsLow())
	{
		/* Stack swap. The StackSwapStruct and the argument copies live in the
		   block ClaimSwapBlock() hands out, NOT in the shared library base, and
		   are reached through a callee-saved register (a3) that survives
		   StackSwap(). Same BUILD REQUIREMENT as in LIB_MPEGA_open. */
		register struct MyStackSwapStruct *sw __asm("a3");
		char *blk = ClaimSwapBlock(mpega_stream);

		if (!blk) return MPEGA_ERR_MEM;

		sw = (struct MyStackSwapStruct *)(blk + REQ_STACKSIZE);
		sw->Args[0] = (ULONG)mpega_stream;
		sw->Args[1] = (ULONG)ms_time_position;
		sw->Args[2] = (ULONG)a4base;
		sw->stack.stk_Lower = blk;
		sw->stack.stk_Pointer = blk + REQ_STACKSIZE;
		sw->stack.stk_Upper = (ULONG)(blk + REQ_STACKSIZE);

		StackSwap(&sw->stack);
		sw->Return = (ULONG)WRAP_MPEGA_seek((MPEGA_STREAM *)sw->Args[0], (ULONG)sw->Args[1], (APTR)sw->Args[2]);
		StackSwap(&sw->stack);

		{
			ULONG r = sw->Return;

			if (sw->Args[3]) FreeVec((char *)sw - REQ_STACKSIZE);
			else sw->Args[4] = 0;	/* release the claim on the stream's block */

			return (LONG)r;
		}
	} else {
		return WRAP_MPEGA_seek(mpega_stream, ms_time_position, a4base);
	}
}

LONG LIB_MPEGA_time(REG(a6, struct PrivateBase *PrivateBase), REG(a0, MPEGA_STREAM *mpega_stream), REG(a1, ULONG *ms_time_position))
{
	return WRAP_MPEGA_time(mpega_stream, ms_time_position);
}

LONG LIB_MPEGA_find_sync(REG(a6, struct PrivateBase *PrivateBase), REG(a0, UBYTE *buffer), REG(d0, LONG buffer_size))
{
	return WRAP_MPEGA_find_sync(buffer, buffer_size);
}

LONG LIB_MPEGA_scale(REG(a6, struct PrivateBase *PrivateBase), REG(a0, MPEGA_STREAM *mpega_stream), REG(d0, LONG scale_percent))
{
	return WRAP_MPEGA_scale(mpega_stream, scale_percent);
}
