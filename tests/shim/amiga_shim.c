/*
 * Host test shim -- implementation.
 *
 * Backs the handful of AmigaOS calls wrap_mpega.c actually makes:
 *   dos.library : Open, Close, Read, Seek, ExamineFH, IoErr, SetIoErr
 *   support.c   : InitSupport / RemoveSupport (see tests/README.md for why
 *                 support.c itself is not compiled here)
 *   string      : bcopy / bzero, which the library expects to exist globally
 *
 * Semantics follow the OS3 autodocs, in particular:
 *   Read()  -> bytes read, 0 at EOF, -1 on error
 *   Seek()  -> the PREVIOUS position, -1 on error
 *   Open()  -> BPTR or NULL
 *
 * Part of the mpega.library regression harness. Not built into the library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

static LONG shim_ioerr = 0;

/*
 * Short-read simulation. dos.library/Read is documented as "USUALLY Read will
 * try to fill up your buffer before returning" (dos.doc) -- short reads are
 * legal and are what PIPE:, serial, CD and network handlers actually do. A
 * plain file never produces one, so without this the harness cannot reach any
 * of the code that deals with a partially filled buffer.
 * 0 = unlimited (normal file behaviour).
 */
LONG shim_max_read = 0;

/* Bytes Read() has delivered so far; decode_ref --openbytes reports it when
   the open returns, to show how much data the open waits for. */
LONG shim_bytes_read = 0;

/* decode_ref --failread=N: the Nth Read() fails once, without reading. */
LONG shim_fail_read = 0;

/*
 * BPTR is 32-bit on AmigaOS, so it cannot hold a host FILE *. Hand out small
 * non-zero indices instead; slot 0 is reserved so a valid BPTR is always true.
 */
#define SHIM_MAX_FILES 16
static FILE *shim_files[SHIM_MAX_FILES];

static FILE *fh_of(BPTR b)
{
    if (b == 0 || b >= SHIM_MAX_FILES) return NULL;
    return shim_files[b];
}

LONG IoErr(void)            { return shim_ioerr; }
void SetIoErr(LONG result)  { shim_ioerr = result; }

BPTR Open(const char *name, LONG accessMode)
{
    FILE *f;
    BPTR  slot;

    if (accessMode != MODE_OLDFILE) { shim_ioerr = 205; return 0; }

    for (slot = 1; slot < SHIM_MAX_FILES; slot++)
        if (!shim_files[slot]) break;
    if (slot >= SHIM_MAX_FILES) { shim_ioerr = 103; return 0; }

    f = fopen(name, "rb");
    if (!f) { shim_ioerr = 205; return 0; }   /* ERROR_OBJECT_NOT_FOUND */

    shim_files[slot] = f;
    shim_ioerr = 0;
    return slot;
}

LONG Close(BPTR file)
{
    FILE *f = fh_of(file);

    if (!f) return 0;
    shim_files[file] = NULL;
    return (fclose(f) == 0) ? 1 : 0;
}

LONG Read(BPTR file, void *buffer, LONG length)
{
    FILE  *f = fh_of(file);
    size_t n;

    if (!f) { shim_ioerr = 205; return -1; }
    if (length <= 0) return 0;
    if (shim_fail_read > 0 && --shim_fail_read == 0) { shim_ioerr = 205; return -1; }

    if (shim_max_read > 0 && length > shim_max_read) length = shim_max_read;

    n = fread(buffer, 1, (size_t)length, f);

    if (n == 0 && ferror(f)) { shim_ioerr = 205; return -1; }

    shim_ioerr = 0;
    shim_bytes_read += (LONG)n;
    return (LONG)n;
}

LONG Seek(BPTR file, LONG position, LONG mode)
{
    FILE *f = fh_of(file);
    long  previous;
    int   whence;

    if (!f) { shim_ioerr = 205; return -1; }

    switch (mode) {
        case OFFSET_BEGINNING: whence = SEEK_SET; break;
        case OFFSET_CURRENT:   whence = SEEK_CUR; break;
        case OFFSET_END:       whence = SEEK_END; break;
        default:               shim_ioerr = 205; return -1;
    }

    previous = ftell(f);
    if (previous < 0) { shim_ioerr = 205; return -1; }

    if (fseek(f, (long)position, whence) != 0) { shim_ioerr = 205; return -1; }

    shim_ioerr = 0;
    return (LONG)previous;      /* dos.library returns the OLD position */
}

BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib)
{
    FILE *f = fh_of(file);
    long  cur, size;

    if (!f || !fib) { shim_ioerr = 205; return FALSE; }

    cur = ftell(f);
    if (cur < 0)                      { shim_ioerr = 205; return FALSE; }
    if (fseek(f, 0, SEEK_END) != 0)   { shim_ioerr = 205; return FALSE; }
    size = ftell(f);
    if (fseek(f, cur, SEEK_SET) != 0) { shim_ioerr = 205; return FALSE; }

    memset(fib, 0, sizeof(*fib));
    fib->fib_DirEntryType = -1;       /* plain file */
    fib->fib_Size         = (LONG)size;

    shim_ioerr = 0;
    return TRUE;
}

/* support.c is not part of the harness; the library only needs these to exist. */
ULONG InitSupport(void) { return 1; }
void  RemoveSupport(void) { }

/*
 * The library calls bcopy()/bzero() unqualified. glibc declares them (as
 * legacy) but we define them here so behaviour is pinned and independent of
 * the host libc's version. Compiled with -fno-builtin-bcopy -fno-builtin-bzero.
 */
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
void bzero(void *ptr, size_t n)                  { memset(ptr, 0, n); }
