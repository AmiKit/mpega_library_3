/*
 * Host test shim -- <exec/memory.h>
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef EXEC_MEMORY_H
#define EXEC_MEMORY_H

#include <exec/types.h>

#define MEMF_ANY     0
#define MEMF_PUBLIC  (1L << 0)
#define MEMF_CHIP    (1L << 1)
#define MEMF_FAST    (1L << 2)
#define MEMF_LOCAL   (1L << 8)
#define MEMF_CLEAR   (1L << 16)
#define MEMF_SHARED  MEMF_PUBLIC

#endif /* EXEC_MEMORY_H */
