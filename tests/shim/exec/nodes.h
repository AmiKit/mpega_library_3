/*
 * Host test shim -- <exec/nodes.h>
 * Only what struct Hook needs.
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef EXEC_NODES_H
#define EXEC_NODES_H

#include <exec/types.h>

struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE        ln_Type;
    BYTE         ln_Pri;
    char        *ln_Name;
};

#endif /* EXEC_NODES_H */
