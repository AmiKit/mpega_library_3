/*
 * Host test shim -- <dos/dos.h>
 * Constants and struct FileInfoBlock, laid out as in the OS3 NDK (the library
 * does malloc(sizeof(struct FileInfoBlock)), so the size must be sane).
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef DOS_DOS_H
#define DOS_DOS_H

#include <exec/types.h>

#define MODE_OLDFILE      1005
#define MODE_NEWFILE      1006
#define MODE_READWRITE    1004

#define OFFSET_BEGINNING  (-1)
#define OFFSET_CURRENT    0
#define OFFSET_END        1

struct DateStamp {
    LONG ds_Days;
    LONG ds_Minute;
    LONG ds_Tick;
};

struct FileInfoBlock {
    LONG   fib_DiskKey;
    LONG   fib_DirEntryType;
    TEXT   fib_FileName[108];
    LONG   fib_Protection;
    LONG   fib_EntryType;
    LONG   fib_Size;            /* number of bytes in file */
    LONG   fib_NumBlocks;
    struct DateStamp fib_Date;
    TEXT   fib_Comment[80];
    UWORD  fib_OwnerUID;
    UWORD  fib_OwnerGID;
    UBYTE  fib_Reserved[32];
};

#endif /* DOS_DOS_H */
