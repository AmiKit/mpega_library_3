#include <exec/memory.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <proto/dos.h>

#ifdef BUILD_POWERUP
#include <powerup/gcclib/powerup_protos.h>

struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
#endif

#ifdef BUILD_WARPUP
#include <powerpc/powerpc.h>
#include <proto/powerpc.h>
#endif

#include "compiler.h"

/*
 * Pool sizing. CreatePool(flags, puddleSize, threshSize): allocations LARGER
 * than threshSize get their own exact-sized puddle, everything else is carved
 * out of puddleSize-byte puddles.
 *
 * This was 131072 for both. Since threshSize == puddleSize, nothing could ever
 * qualify for a dedicated puddle, so the ~36 KB DecHandle was carved out of a
 * 128 KB puddle -- and because CreatePool defers the first AllocMem, the
 * library had to find a free contiguous 128 KB block just to satisfy its first
 * 44-byte malloc. On a fragmented Amiga that is a needless way to fail.
 *
 * At 16384 the DecHandle exceeds the threshold and gets its own exact puddle,
 * while the many small allocations share 16 KB puddles: the largest contiguous
 * demand drops from 128 KB to roughly the DecHandle's own size.
 */
#define PoolSize 16384
static void *MemPool = NULL;
#ifdef BUILD_POWERUP
static void *PoolSem = NULL;
#elif defined(BUILD_WARPUP)
static struct SignalSemaphorePPC PoolSem;
#else
static struct SignalSemaphore PoolSem;
#endif

ULONG LIBPPC InitSupport(void)
{
#ifdef BUILD_POWERUP
	SysBase = *((struct ExecBase **)4L);

	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36)))
	{
		if ((MemPool = PPCCreatePool(MEMF_PUBLIC, PoolSize, PoolSize)))
		{
			if ((PoolSem = PPCCreateSemaphore(NULL))) return 1;
		}
	}
#elif defined(BUILD_WARPUP)
	if ((MemPool = CreatePoolPPC(MEMF_PUBLIC, PoolSize, PoolSize)))
	{
		InitSemaphorePPC(&PoolSem);
		return 1;
	}
#else
#ifdef BUILD_OS4
	if ((MemPool = CreatePool(MEMF_SHARED, PoolSize, PoolSize)))
#else
	if ((MemPool = CreatePool(MEMF_PUBLIC, PoolSize, PoolSize)))
#endif
	{
		InitSemaphore(&PoolSem);
		return 1;
	}
#endif

	return 0;
}

void LIBPPC RemoveSupport(void)
{
#ifdef BUILD_POWERUP
	if (DOSBase)
		CloseLibrary((struct Library *)DOSBase);

	if (PoolSem)
		PPCDeleteSemaphore(PoolSem);
#endif

#ifdef BUILD_WARPUP
	FreeSemaphorePPC(&PoolSem);
#endif

	if (MemPool)
	{
#ifdef BUILD_POWERUP
		PPCDeletePool(MemPool);
#elif defined(BUILD_WARPUP)
		DeletePoolPPC(MemPool);
#else
		DeletePool(MemPool);
#endif
		MemPool = NULL;
	}
}

#if defined(__PPC__) || defined(__AROS__)
#include <string.h>
#else
void bcopy(const void *s1, void *s2, unsigned long n)
{
	/* MODERNIZED (gcc 6.x): the original used the old GNU "cast as lvalue"
	   extension (e.g. *((long *)s2)++ and (char *)s1 += n), removed since
	   gcc 4. Rewritten with typed working pointers; behaviour is identical. */
	unsigned long m;
	const char *cs1 = s1;
	char *cs2 = s2;

	if (!n) return;

	if (cs2 < cs1)
	{
		if ((long)cs1 & 1)
		{
			*cs2++ = *cs1++;
			n--;
		}

		if (!((long)cs2 & 1))
		{
			const long *ls1 = (const long *)cs1;
			long *ls2 = (long *)cs2;

			m = n / sizeof(long);
			n &= sizeof(long) - 1;

			for (; m; m--)
			{
				*ls2++ = *ls1++;
			}

			cs1 = (const char *)ls1;
			cs2 = (char *)ls2;
		}

		for (; n; n--)
		{
			*cs2++ = *cs1++;
		}
	} else {
		cs1 += n;
		cs2 += n;

		if ((long)cs1 & 1)
		{
			*--cs2 = *--cs1;
			n--;
		}

		if (!((long)cs2 & 1))
		{
			const long *ls1 = (const long *)cs1;
			long *ls2 = (long *)cs2;

			m = n / sizeof(long);
			n &= sizeof(long) - 1;

			for (; m; m--)
			{
				*--ls2 = *--ls1;
			}

			cs1 = (const char *)ls1;
			cs2 = (char *)ls2;
		}

		for (; n; n--)
		{
			*--cs2 = *--cs1;
		}
	}
}

void bzero(void *ptr, unsigned long siz)
{
	long lsize, bsize;
	unsigned long *l;
	unsigned char *b;

	lsize = siz >> 5;
	bsize = siz & 3;
	siz = (siz & 31) >> 2;

	l = ptr;
	l--;

	while (lsize > 0)
	{
		*++l = 0;
		*++l = 0;
		*++l = 0;
		*++l = 0;
		*++l = 0;
		*++l = 0;
		*++l = 0;
		*++l = 0;
		lsize--;
	}

	while (siz > 0)
	{
		*++l = 0;
		siz--;
	}

	b = (unsigned char *)l;
	b += 3;

	while (bsize > 0)
	{
		*++b = 0;
		bsize--;
	}
}

void *memcpy(void *s1, const void *s2, unsigned long n)
{
	bcopy(s2,s1,n);

	return s1;
}

void *memmove(void *s1, const void *s2, unsigned long n)
{
	bcopy(s2,s1,n);

	return s1;
}

void *memset(void *s, int c, unsigned long n)
{
	if (n != 0)
	{
		unsigned char *p = s;

		do {
			*p++ = c;
		} while (--n != 0);
	}

	return s;
}
#endif

void *malloc(unsigned long size)
{
	ULONG *MyMemory;

	if (size == 0) return NULL;
	size += 8;

#ifdef BUILD_POWERUP
	PPCObtainSemaphore(PoolSem);
	MyMemory = (ULONG *)PPCAllocPooled(MemPool, size);
	PPCReleaseSemaphore(PoolSem);
#elif defined(BUILD_WARPUP)
	ObtainSemaphorePPC(&PoolSem);
	MyMemory = (ULONG *)AllocPooledPPC(MemPool, size);
	ReleaseSemaphorePPC(&PoolSem);
#else
	ObtainSemaphore(&PoolSem);
	MyMemory = (ULONG *)AllocPooled(MemPool, size);
	ReleaseSemaphore(&PoolSem);
#endif

	if (MyMemory)
	{
		*MyMemory++ = size;

		if ((ULONG)MyMemory & 7)
		{
			*MyMemory++ = 0;
		}
	}

	return((void *)MyMemory);
}

void *calloc(unsigned long nobj, unsigned long size)
{
	unsigned long siz = size * nobj;
	void *ptr;

	if ((ptr = malloc(siz)))
		bzero(ptr, siz);

	return ptr;
}

void free(void *ptr)
{
	ULONG *MemPtr;
	ULONG Size;

	if (ptr == NULL) return;

	MemPtr = ptr;
	Size = *--MemPtr;

	if (Size == 0)
	{
		Size = *--MemPtr;
	}

#ifdef BUILD_POWERUP
	PPCObtainSemaphore(PoolSem);
	PPCFreePooled(MemPool, MemPtr, Size);
	PPCReleaseSemaphore(PoolSem);
#elif defined(BUILD_WARPUP)
	ObtainSemaphorePPC(&PoolSem);
	FreePooledPPC(MemPool, MemPtr, Size);
	ReleaseSemaphorePPC(&PoolSem);
#else
	ObtainSemaphore(&PoolSem);
	FreePooled(MemPool, MemPtr, Size);
	ReleaseSemaphore(&PoolSem);
#endif
}

