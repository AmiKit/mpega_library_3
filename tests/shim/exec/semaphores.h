/*
 * Host test shim -- <exec/semaphores.h>
 * The harness is single-threaded; semaphores are structurally present but inert.
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef EXEC_SEMAPHORES_H
#define EXEC_SEMAPHORES_H

#include <exec/types.h>
#include <exec/nodes.h>

struct SignalSemaphore {
    struct Node  ss_Link;
    WORD         ss_NestCount;
    struct MinNode ss_WaitQueue_head;
    APTR         ss_MultipleLink;
    APTR         ss_Owner;
    WORD         ss_QueueCount;
};

#endif /* EXEC_SEMAPHORES_H */
