#include <exec/memory.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <proto/dos.h>


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
static struct SignalSemaphore PoolSem;

ULONG InitSupport(void)
{
	if ((MemPool = CreatePool(MEMF_PUBLIC, PoolSize, PoolSize)))
	{
		InitSemaphore(&PoolSem);
		return 1;
	}

	return 0;
}

void RemoveSupport(void)
{
	if (MemPool)
	{
		DeletePool(MemPool);
		MemPool = NULL;
	}
}

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

/* No bzero or memmove here: gcc turns every bzero() call into inline stores or
   a memset() call, and nothing calls memmove (checked in the disassembly). */

void *memcpy(void *s1, const void *s2, unsigned long n)
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

void *malloc(unsigned long size)
{
	ULONG *MyMemory;

	if (size == 0) return NULL;
	size += 8;

	ObtainSemaphore(&PoolSem);
	MyMemory = (ULONG *)AllocPooled(MemPool, size);
	ReleaseSemaphore(&PoolSem);

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

/* Clears in longwords: memset's byte loop over the ~36 KB DecHandle cost about
   390000 cycles per open. siz is rounded up so the stores stay in the block. */
void *calloc(unsigned long nobj, unsigned long size)
{
	unsigned long siz = (size * nobj + 3) & ~3UL;
	ULONG *ptr = malloc(siz), *p = ptr;

	if (ptr)
		for (; siz; siz -= 4)
			*p++ = 0;

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

	ObtainSemaphore(&PoolSem);
	FreePooled(MemPool, MemPtr, Size);
	ReleaseSemaphore(&PoolSem);
}

