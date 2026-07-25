#include "library_base.h"
#include "compiler.h"

#define AQuote(string) #string								/* Put quotes around the whole thing */
#define AVersion(major,minor) AQuote(major ## . ## minor)	/* Concatenate the two version-numbers */
#define AmVersion(major,minor) AVersion(major,minor)		/* We need to do it this way to elimate the blank spaces */

#define LIB_NAME	"mpega.library"
#define LIB_VERSION	3
#define LIB_REVISION	0
#define LIB_DATE	"(25.07.26)"

#define CLONE_VERSION	1
#define CLONE_REVISION	7

#ifndef LIBCPU
	#if defined(BUILD_POWERUP)
		#define LIBCPU " [PowerUP]"
	#elif defined(BUILD_WARPUP)
		#define LIBCPU " [WarpOS]"
	#elif defined(BUILD_MORPHOS)
		#define LIBCPU " [MorphOS]"
	#elif defined(BUILD_OS4)
		#define LIBCPU " [AmigaOS4]"
	#elif defined(BUILD_AROS)
	    	#define LIBCPU " [AROS]"
	#else
		#if defined(_M68060) || defined(mc68060)
			#if defined(_M68881) || defined(__HAVE_68881__)
				#define LIBCPU " [060/FPU]"
			#else
				#define LIBCPU " [060]"
			#endif
		#elif defined(_M68040) || defined(mc68040)
			#if defined(_M68881) || defined(__HAVE_68881__)
				#define LIBCPU " [040/FPU]"
			#else
				#define LIBCPU " [040]"
			#endif
		#elif defined(_M68030) || defined(mc68030)
			#if defined(_M68881) || defined(__HAVE_68881__)
				#define LIBCPU " [030/FPU]"
			#else
				#define LIBCPU " [030]"
			#endif
		#elif defined(_M68020) || defined(mc68020)
			#if defined(_M68881) || defined(__HAVE_68881__)
				#define LIBCPU " [020/FPU]"
			#else
				#define LIBCPU " [020]"
			#endif
		#else
			#define LIBCPU
		#endif
	#endif
#endif

#ifndef LIBVARIANT
	#ifdef MPEGA_FORCE_HIFI
		#define LIBVARIANT " [Hi-Fi forced]"
	#elif defined(OPT_ACCURACY)
		#define LIBVARIANT " [Hi-Fi]"
	#else
		#define LIBVARIANT " [bugfix]"
	#endif
#endif

/* Phase marker for internal testing. Building with -DMPEGA_PHASE=N stamps
   that number into $VER, so a library under test can always be identified
   with `version LIBS:mpega.library`, essential when several builds under
   test are installed side by side to find which one differs. Undefined (a
   normal release build) adds nothing. */
#ifndef LIBPHASE
	#ifdef MPEGA_PHASE
		#define APhase_(n) AQuote(n)
		#define APhase(n) APhase_(n)
		#define LIBPHASE " [P" APhase(MPEGA_PHASE) "]"
	#else
		#define LIBPHASE
	#endif
#endif

/* Fixed-point backend marker. The FPM_* branch selected in mad/fixed.h decides
   how every multiply-accumulate in the decoder is carried out, and it moves
   both speed and accuracy, yet nothing else in $VER reflected it, so two
   libraries differing only in FPM were indistinguishable once installed. That
   cost a benchmark round: the timings could not be proven to belong to the
   binary they were filed against, and only the tester's own notes tied them
   together. A measurement has to be able to identify its own subject. */
#ifndef LIBFPM
	#if defined(FPM_M68K)
		#define LIBFPM " [M68K]"
	#elif defined(FPM_64BIT)
		#define LIBFPM " [64bit]"
	#elif defined(FPM_DEFAULT)
		#define LIBFPM " [default]"
	#elif defined(FPM_PPC)
		#define LIBFPM " [PPC]"
	#elif defined(FPM_INTEL) || defined(FPM_ARM) || defined(FPM_MIPS) || defined(FPM_SPARC)
		#define LIBFPM " [asm]"
	#else
		#define LIBFPM
	#endif
#endif

#define LIB_ID LIB_NAME " " AmVersion(LIB_VERSION,LIB_REVISION) " " LIB_DATE LIBCPU LIBVARIANT LIBFPM LIBPHASE " mpega.library clone v" AmVersion(CLONE_VERSION,CLONE_REVISION) " by Sigbjørn (CISC) Skjæret, using libmad 0.15.1b + 0.16.x fixes - Hi-Fi 68k fork (2026) by Jan Zahurancik / AmiKit - Copyright (C) 2000-2004 Underbit Technologies, Inc."


#include <libraries/mpega.h>

#ifdef __MORPHOS__

struct Library *LibInit(struct PrivateBase *PrivateBase, BPTR SegList, struct ExecBase *sysbase);
struct Library *LIB_Open(void);
BPTR LIB_Close(void);
BPTR LIB_Expunge(void);

MPEGA_STREAM *LIB_MPEGA_open(void);
void LIB_MPEGA_close(void);
LONG LIB_MPEGA_decode_frame(void);
LONG LIB_MPEGA_seek(void);
LONG LIB_MPEGA_time(void);
LONG LIB_MPEGA_find_sync(void);
LONG LIB_MPEGA_scale(void);

#else

#ifdef BUILD_OS4
#define __IFACE_OR_BASE		struct Interface *Self
#else
#define __IFACE_OR_BASE		struct PrivateBase *PrivateBase
#endif

#ifdef BUILD_OS4
struct Library * ASM LibInit(REG(d0, struct PrivateBase *PrivateBase), REG(a0, BPTR SegList), REG(a6, struct ExecIFace *pIExec));
#else
struct Library * ASM LibInit(REG(d0, struct PrivateBase *PrivateBase), REG(a0, BPTR SegList), REG(a6, struct ExecBase *sysbase));
#endif

struct Library * ASM LIB_Open(REG(a6, __IFACE_OR_BASE));
BPTR ASM LIB_Close(REG(a6, __IFACE_OR_BASE));
BPTR ASM LIB_Expunge(REG(a6, __IFACE_OR_BASE));

MPEGA_STREAM * LIB LIB_MPEGA_open(REG(a6, __IFACE_OR_BASE), REG(a0, char *stream_name), REG(a1, MPEGA_CTRL *ctrl), REG(a4, APTR a4base));
void LIB LIB_MPEGA_close(REG(a6, __IFACE_OR_BASE), REG(a0, MPEGA_STREAM *mpega_stream), REG(a4, APTR a4base));
LONG LIB LIB_MPEGA_decode_frame(REG(a6, __IFACE_OR_BASE), REG(a0, MPEGA_STREAM *mpega_stream), REG(a1, WORD **pcm), REG(a4, APTR a4base));
LONG LIB LIB_MPEGA_seek(REG(a6, __IFACE_OR_BASE), REG(a0, MPEGA_STREAM *mpega_stream), REG(d0, ULONG ms_time_position), REG(a4, APTR a4base));
LONG LIB LIB_MPEGA_time(REG(a6, __IFACE_OR_BASE), REG(a0, MPEGA_STREAM *mpega_stream), REG(a1, ULONG *ms_time_position));
LONG LIB LIB_MPEGA_find_sync(REG(a6, __IFACE_OR_BASE), REG(a0, UBYTE *buffer), REG(d0, LONG buffer_size));
LONG LIB LIB_MPEGA_scale(REG(a6, __IFACE_OR_BASE), REG(a0, MPEGA_STREAM *mpega_stream), REG(d0, LONG scale_percent));
#endif
