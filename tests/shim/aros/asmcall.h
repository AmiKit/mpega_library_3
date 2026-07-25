/*
 * Host test shim -- <aros/asmcall.h>
 *
 * wrap_mpega.c's __AROS__ branch builds its CALLHOOK out of AROS_UFC4/AROS_UFCA.
 * On the host we lower that to a plain indirect call with the same argument
 * order (hook, object, message, base); the register annotations are dropped.
 *
 * Part of the mpega.library regression harness. Not built into the library.
 */

#ifndef AROS_ASMCALL_H
#define AROS_ASMCALL_H

#include <exec/types.h>

/* AROS_UFCA(type, value, register) -- register annotation is irrelevant here. */
#define AROS_UFCA(type, value, reg) ((APTR)(IPTR)(value))

typedef IPTR (*mpega_hookfn_t)(APTR, APTR, APTR, APTR);

#define AROS_UFC4(rettype, fn, a0, a1, a2, a3) \
    ((rettype)((mpega_hookfn_t)(fn))((a0), (a1), (a2), (a3)))

#endif /* AROS_ASMCALL_H */
