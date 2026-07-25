/*
 * Host test shim -- <utility/hooks.h>
 * struct Hook copied verbatim from the OS3 NDK so the layout matches.
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef UTILITY_HOOKS_H
#define UTILITY_HOOKS_H

#include <exec/types.h>
#include <exec/nodes.h>

struct Hook {
    struct MinNode h_MinNode;
    ULONG        (*h_Entry)();     /* assembler entry point */
    ULONG        (*h_SubEntry)();  /* often HLL entry point */
    APTR           h_Data;         /* owner specific        */
};

#endif /* UTILITY_HOOKS_H */
