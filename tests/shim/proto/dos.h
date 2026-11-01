/*
 * Host test shim -- <proto/dos.h>
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef PROTO_DOS_H
#define PROTO_DOS_H

#include <exec/types.h>
#include <dos/dos.h>

BPTR Open(const char *name, LONG accessMode);
LONG Close(BPTR file);
LONG Read(BPTR file, void *buffer, LONG length);
LONG Seek(BPTR file, LONG position, LONG mode);
BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib);
LONG IoErr(void);
void SetIoErr(LONG result);

#endif /* PROTO_DOS_H */
