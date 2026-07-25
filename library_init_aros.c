#include <aros/libcall.h>
#include <aros/asmcall.h>
#include "library_init.h"

#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#include "wrap_mpega.h"
#include "support.h"

#define LIBBASETYPE    struct PrivateBase
#define LIBBASETYPEPTR LIBBASETYPE *

AROS_LH0 (LIBBASETYPEPTR, Reserved,
    LIBBASETYPEPTR, PrivateBase, 4, mpega
)
{
    AROS_LIBFUNC_INIT
    return NULL;
    AROS_LIBFUNC_EXIT
}

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

extern const APTR LibVectors[];

static const char LibVersion[] = "$VER: " LIB_ID;

AROS_UFP3 (LIBBASETYPEPTR, InitLib,
    AROS_UFPA(LIBBASETYPEPTR, PrivateBase, D0),
    AROS_UFPA(BPTR, SegList, A0),
    AROS_UFPA(struct ExecBase *, sysbase, A6)
);

static struct LibInitStruct
{
    IPTR                   LibSize;
    const APTR             *FuncTable;
    const struct DataTable *DataTable;
    APTR                    InitFunc;
}
const LibInitStruct =
{
    sizeof(LIBBASETYPE),
    LibVectors,
    NULL,
    (APTR)InitLib
};

struct Resident const RomTag =
{
    RTC_MATCHWORD,
    (struct Resident *)&RomTag,
    &RomTag + 1,
    RTF_AUTOINIT,
    LIB_VERSION,
    NT_LIBRARY,
    0,
    LIB_NAME,
    &LibVersion[6],
    (APTR)&LibInitStruct
};

static void LibCleanup(struct PrivateBase *PrivateBase)
{
    RemoveSupport();

    if (DOSBase)
    {
	CloseLibrary((struct Library *)DOSBase);
	PrivateBase->pv_DOSBase = DOSBase = NULL;
    }
}

BPTR LibExpunge(struct PrivateBase *PrivateBase)
{
    if ((PrivateBase->pv_Lib.lib_OpenCnt == 0))
    {
	BPTR seglist;

	seglist = PrivateBase->pv_SegList;
	Remove(&PrivateBase->pv_Lib.lib_Node);
	LibCleanup(PrivateBase);

	FreeMem((APTR)((IPTR)(PrivateBase) - (IPTR)(PrivateBase->pv_Lib.lib_NegSize)),
			PrivateBase->pv_Lib.lib_NegSize + PrivateBase->pv_Lib.lib_PosSize);
	return seglist;
    } else PrivateBase->pv_Lib.lib_Flags |= LIBF_DELEXP;

    return 0L;
}

AROS_UFH3 (LIBBASETYPEPTR, InitLib,
    AROS_UFHA(LIBBASETYPEPTR, PrivateBase, D0),
    AROS_UFHA(BPTR, SegList, A0),
    AROS_UFHA(struct ExecBase *, sysbase, A6)
)
{
    AROS_USERFUNC_INIT

    SysBase = sysbase;

    if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36)))
    {
	if (InitSupport())
	{
	    PrivateBase->pv_Lib.lib_Revision     = LIB_REVISION;
	    PrivateBase->pv_SegList              = SegList;
	    PrivateBase->pv_SysBase              = SysBase;
	    PrivateBase->pv_DOSBase              = DOSBase;

	    return PrivateBase;
	}
    }

    LibCleanup(PrivateBase);
    return NULL;
	
    AROS_USERFUNC_EXIT
}

AROS_LH1 (LIBBASETYPEPTR, Open,
    AROS_LHA (ULONG, version, D0),
    LIBBASETYPEPTR, PrivateBase, 1, mpega
)
{
    AROS_LIBFUNC_INIT

    PrivateBase->pv_Lib.lib_Flags &= ~LIBF_DELEXP;
    PrivateBase->pv_Lib.lib_OpenCnt++;

    return PrivateBase;

    AROS_LIBFUNC_EXIT
    
}

AROS_LH0 (BPTR, Close,
    LIBBASETYPEPTR, PrivateBase, 2, mpega
)
{
    AROS_LIBFUNC_INIT

    if (PrivateBase->pv_Lib.lib_OpenCnt > 0) PrivateBase->pv_Lib.lib_OpenCnt--;

    if ((PrivateBase->pv_Lib.lib_OpenCnt == 0) && (PrivateBase->pv_Lib.lib_Flags & LIBF_DELEXP))
	return LibExpunge(PrivateBase);

    return 0L;
	
    AROS_LIBFUNC_EXIT
}

AROS_LH1 (BPTR, Expunge,
    AROS_LHA(LIBBASETYPEPTR, PrivateBase, D0),
    struct ExecBase *, sysBase, 3, mpega
)
{
    AROS_LIBFUNC_INIT

    return LibExpunge(PrivateBase);
	
    AROS_LIBFUNC_EXIT
}

AROS_LH2(MPEGA_STREAM *, MPEGA_open,
    AROS_LHA(char *, stream_name, A0),
    AROS_LHA(MPEGA_CTRL *, ctrl, A1),
    LIBBASETYPEPTR, PrivateBase, 5, mpega)
{
    AROS_LIBFUNC_INIT
    
    return WRAP_MPEGA_open(stream_name, ctrl, 0);
    
    AROS_LIBFUNC_EXIT
}

AROS_LH1(void, MPEGA_close,
    AROS_LHA(MPEGA_STREAM *, mpega_stream, A0),
    LIBBASETYPEPTR, PrivateBase, 6, mpega)
{
    AROS_LIBFUNC_INIT
    
    WRAP_MPEGA_close(mpega_stream, 0);

    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, MPEGA_decode_frame,
    AROS_LHA(MPEGA_STREAM *, mpega_stream, A0),
    AROS_LHA(WORD *, pcm[MPEGA_MAX_CHANNELS], A1),
    LIBBASETYPEPTR, PrivateBase, 7, mpega)
{
    AROS_LIBFUNC_INIT
    
    return WRAP_MPEGA_decode_frame(mpega_stream, pcm, 0);
    
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, MPEGA_seek,
    AROS_LHA(MPEGA_STREAM *, mpega_stream, A0),
    AROS_LHA(ULONG, ms_time_position, D0),
    LIBBASETYPEPTR, PrivateBase, 8, mpega)
{
    AROS_LIBFUNC_INIT
    
    return WRAP_MPEGA_seek(mpega_stream, ms_time_position, 0);
    
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, MPEGA_time,
    AROS_LHA(MPEGA_STREAM *, mpega_stream, A0),
    AROS_LHA(ULONG *, ms_time_position, A1),
    LIBBASETYPEPTR, PrivateBase, 9, mpega)
{
    AROS_LIBFUNC_INIT
    
    return WRAP_MPEGA_time(mpega_stream, ms_time_position);
    
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, MPEGA_find_sync,
    AROS_LHA(UBYTE *, buffer, A0),
    AROS_LHA(LONG, buffer_size, D0),
    LIBBASETYPEPTR, PrivateBase, 10, mpega)
{
    AROS_LIBFUNC_INIT

    return WRAP_MPEGA_find_sync(buffer, buffer_size);
    
    AROS_LIBFUNC_EXIT
}

AROS_LH2(LONG, MPEGA_scale,
    AROS_LHA(MPEGA_STREAM *, mpega_stream, A0),
    AROS_LHA(LONG, scale_percent, D0),
    LIBBASETYPEPTR, PrivateBase, 11, mpega)
{
    AROS_LIBFUNC_INIT
    
    return WRAP_MPEGA_scale(mpega_stream, scale_percent);
    
    AROS_LIBFUNC_EXIT
}

const APTR LibVectors[]=
{
    &AROS_SLIB_ENTRY(Open,mpega),
    &AROS_SLIB_ENTRY(Close,mpega),
    &AROS_SLIB_ENTRY(Expunge,mpega),
    &AROS_SLIB_ENTRY(Reserved,mpega),
    &AROS_SLIB_ENTRY(MPEGA_open,mpega),
    &AROS_SLIB_ENTRY(MPEGA_close,mpega),
    &AROS_SLIB_ENTRY(MPEGA_decode_frame,mpega),
    &AROS_SLIB_ENTRY(MPEGA_seek,mpega),
    &AROS_SLIB_ENTRY(MPEGA_time,mpega),
    &AROS_SLIB_ENTRY(MPEGA_find_sync,mpega),
    &AROS_SLIB_ENTRY(MPEGA_scale,mpega),
    (void *)-1
};

