/*
 * Host test shim -- <exec/types.h>
 *
 * Minimal Amiga base types for building the real library sources on a
 * POSIX host. Sizes are chosen to match m68k-amigaos:
 *   LONG/ULONG == 32 bit  -> "int" on both ILP32 (m68k) and LP64 (host)
 * Pointers are host-width (64 bit); nothing in the decode path assumes
 * sizeof(APTR) == 4.
 *
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef signed char        BYTE;
typedef unsigned char      UBYTE;
typedef signed short       WORD;
typedef unsigned short     UWORD;
typedef signed int         LONG;   /* 32 bit, as on m68k */
typedef unsigned int       ULONG;  /* 32 bit, as on m68k */
typedef unsigned short     RPTR;

typedef char               TEXT;
typedef char              *STRPTR;
typedef const char        *CONST_STRPTR;

typedef void              *APTR;
typedef short              BOOL;
typedef float              FLOAT;
typedef double             DOUBLE;

typedef uintptr_t          IPTR;   /* AROS integer-sized pointer */

/* dos.library BPTR is a 32-bit value on AmigaOS, NOT a pointer -- the library
   assigns a hook's ULONG return straight into it (wrap_mpega.c:802). Keep it
   32-bit here too, or that assignment warns and the shim stops being faithful.
   amiga_shim.c maps these values to host FILE * through a small table. */
typedef ULONG              BPTR;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* EXEC_TYPES_H */
