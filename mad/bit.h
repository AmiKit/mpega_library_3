/*
 * libmad - MPEG audio decoder library
 * Copyright (C) 2000-2003 Underbit Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id: bit.h,v 1.11 2003/05/27 22:40:36 rob Exp $
 */

# ifndef LIBMAD_BIT_H
# define LIBMAD_BIT_H

struct mad_bitptr {
  unsigned char const *byte;
  unsigned short cache;
  unsigned short left;
};

# define mad_bit_finish(bitptr)		/* nothing */
# define mad_bit_bitsleft(bitptr)  ((bitptr)->left)

/*
 * OPT_INLINE_BITS (mpega Hi-Fi fork): give the bit reader to the compiler
 * instead of hiding it behind a call.
 *
 * The header already macro-expands mad_bit_finish and mad_bit_bitsleft, so
 * this is the same idea applied to the four that were still calls. Profiling
 * decode_ref over l3_44_stereo_cbr128.mp3 counts 25953 mad_bit_read calls for
 * 78 frames, about 333 per frame, and the Huffman decoder reaches it through
 * two more layers of call. On a 68020, where jsr/rts plus the callee's
 * register saves cost more than the work in mad_bit_nextbyte does, that
 * overhead is worth removing.
 *
 * Safe to do here only because libmad is entirely internal to this library:
 * mpega.fd exports the seven MPEGA_* entry points and no mad_* symbol at all,
 * so nothing outside links against these. bit.c compiles the out-of-line
 * copies when the option is off, so the switch is a one-flag revert.
 *
 * mad_bit_crc keeps its own translation unit either way: it is a loop over a
 * 256-entry table, far too big to inline and not on the hot path.
 */
# if defined(OPT_INLINE_BITS)

/* bit.c pulls this in itself; the other users of bit.h do not, so the header
   has to be self-contained once the bodies live here. */
#  include <limits.h>

#  define MAD_BIT_INLINE static __inline__

MAD_BIT_INLINE
void mad_bit_init(struct mad_bitptr *bitptr, unsigned char const *byte)
{
  bitptr->byte  = byte;
  bitptr->cache = 0;
  bitptr->left  = CHAR_BIT;
}

MAD_BIT_INLINE
unsigned int mad_bit_length(struct mad_bitptr const *begin,
			    struct mad_bitptr const *end)
{
  return begin->left +
    CHAR_BIT * (end->byte - (begin->byte + 1)) + (CHAR_BIT - end->left);
}

MAD_BIT_INLINE
unsigned char const *mad_bit_nextbyte(struct mad_bitptr const *bitptr)
{
  return bitptr->left == CHAR_BIT ? bitptr->byte : bitptr->byte + 1;
}

MAD_BIT_INLINE
void mad_bit_skip(struct mad_bitptr *bitptr, unsigned int len)
{
  bitptr->byte += len / CHAR_BIT;
  bitptr->left -= len % CHAR_BIT;

  if (bitptr->left > CHAR_BIT) {
    bitptr->byte++;
    bitptr->left += CHAR_BIT;
  }

  if (bitptr->left < CHAR_BIT)
    bitptr->cache = *bitptr->byte;
}

MAD_BIT_INLINE
unsigned long mad_bit_read(struct mad_bitptr *bitptr, unsigned int len)
{
  register unsigned long value;

  if (len == 0)
    return 0;

  if (bitptr->left == CHAR_BIT)
    bitptr->cache = *bitptr->byte;

  if (len < bitptr->left) {
    value = (bitptr->cache & ((1 << bitptr->left) - 1)) >>
      (bitptr->left - len);
    bitptr->left -= len;

    return value;
  }

  /* remaining bits in current byte */

  value = bitptr->cache & ((1 << bitptr->left) - 1);
  len  -= bitptr->left;

  bitptr->byte++;
  bitptr->left = CHAR_BIT;

  /* more bytes */

  while (len >= CHAR_BIT) {
    value = (value << CHAR_BIT) | *bitptr->byte++;
    len  -= CHAR_BIT;
  }

  if (len > 0) {
    bitptr->cache = *bitptr->byte;

    value = (value << len) | (bitptr->cache >> (CHAR_BIT - len));
    bitptr->left -= len;
  }

  return value;
}

# else

void mad_bit_init(struct mad_bitptr *, unsigned char const *);

unsigned int mad_bit_length(struct mad_bitptr const *,
			    struct mad_bitptr const *);

unsigned char const *mad_bit_nextbyte(struct mad_bitptr const *);

void mad_bit_skip(struct mad_bitptr *, unsigned int);
unsigned long mad_bit_read(struct mad_bitptr *, unsigned int);

# endif

void mad_bit_write(struct mad_bitptr *, unsigned int, unsigned long);

unsigned short mad_bit_crc(struct mad_bitptr, unsigned int, unsigned short);

# endif
