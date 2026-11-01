/*
 * mpega.library call stubs for the Amiga-side test programs (m68k_decode,
 * mpegabench). The library is third-party, so amiga-gcc ships no proto header
 * for it. Offsets and registers come from mpega.fd: "##bias 30", six bytes per
 * entry, in declaration order. The program defines MPEGABase.
 *
 * a0 and a1 are "+r", read AND written: on AmigaOS d0/d1/a0/a1 are scratch.
 * Declared as inputs only, gcc assumed they still held the stream and the pcm
 * array after the call, and the first mpegabench failed after one frame. They
 * cannot be clobbers either (gcc 4+ rejects clobbering an operand register),
 * so an operand the callee destroys is an output; d1 is no operand and is a
 * clobber. a4 is left alone: the library passes it to a bs_access hook only.
 */
#ifndef MPEGA_CALLS_H
#define MPEGA_CALLS_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <libraries/mpega.h>

extern struct Library *MPEGABase;

static __inline MPEGA_STREAM *MPEGA_open(char *name, MPEGA_CTRL *ctrl)
{
    register struct Library *b __asm("a6") = MPEGABase;
    register MPEGA_STREAM *res __asm("d0");
    register char *nm __asm("a0") = name;
    register MPEGA_CTRL *ct __asm("a1") = ctrl;
    __asm volatile ("jsr a6@(-30)" : "=r" (res), "+r" (nm), "+r" (ct) : "r" (b) : "d1", "cc", "memory");
    return res;
}
static __inline void MPEGA_close(MPEGA_STREAM *s)
{
    register struct Library *b __asm("a6") = MPEGABase;
    register LONG res __asm("d0");
    register MPEGA_STREAM *st __asm("a0") = s;
    __asm volatile ("jsr a6@(-36)" : "=r" (res), "+r" (st) : "r" (b) : "d1", "a1", "cc", "memory");
}
static __inline LONG MPEGA_decode_frame(MPEGA_STREAM *s, WORD **pcm)
{
    register struct Library *b __asm("a6") = MPEGABase;
    register LONG res __asm("d0");
    register MPEGA_STREAM *st __asm("a0") = s;
    register WORD **pc __asm("a1") = pcm;
    __asm volatile ("jsr a6@(-42)" : "=r" (res), "+r" (st), "+r" (pc) : "r" (b) : "d1", "cc", "memory");
    return res;
}
static __inline LONG MPEGA_seek(MPEGA_STREAM *s, ULONG ms)
{
    register struct Library *b __asm("a6") = MPEGABase;
    register LONG d0 __asm("d0") = (LONG)ms;
    register MPEGA_STREAM *st __asm("a0") = s;
    __asm volatile ("jsr a6@(-48)" : "+r" (d0), "+r" (st) : "r" (b) : "d1", "a1", "cc", "memory");
    return d0;
}
static __inline LONG MPEGA_time(MPEGA_STREAM *s, ULONG *ms)
{
    register struct Library *b __asm("a6") = MPEGABase;
    register LONG res __asm("d0");
    register MPEGA_STREAM *st __asm("a0") = s;
    register ULONG *mp __asm("a1") = ms;
    __asm volatile ("jsr a6@(-54)" : "=r" (res), "+r" (st), "+r" (mp) : "r" (b) : "d1", "cc", "memory");
    return res;
}
static __inline LONG MPEGA_scale(MPEGA_STREAM *s, LONG pct)
{
    register struct Library *b __asm("a6") = MPEGABase;
    register LONG d0 __asm("d0") = pct;
    register MPEGA_STREAM *st __asm("a0") = s;
    __asm volatile ("jsr a6@(-66)" : "+r" (d0), "+r" (st) : "r" (b) : "d1", "a1", "cc", "memory");
    return d0;
}

#endif
