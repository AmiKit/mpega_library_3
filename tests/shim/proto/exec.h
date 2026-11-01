/*
 * Host test shim -- <proto/exec.h>
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef PROTO_EXEC_H
#define PROTO_EXEC_H

#include <exec/types.h>
#include <exec/nodes.h>
#include <utility/hooks.h>

/* support.c is deliberately NOT compiled into the harness (see tests/README.md);
   these are provided by the shim so the wrapper links. */
ULONG InitSupport(void);
void  RemoveSupport(void);


#endif /* PROTO_EXEC_H */
