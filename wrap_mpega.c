#include "wrap_mpega.h"
#include "compiler.h"

#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>

/* BUGFIX: this file drives libmad's data structures but never
   included its config.h, so mad/fixed.h saw SIZEOF_INT undefined, took the
   "#if SIZEOF_INT >= 4" branch as false, and typed mad_fixed_t as signed long
   here while every file in mad/ typed it as signed int. On m68k both are 32 bit
   so the mismatch is invisible, but it is a silent landmine for any LP64
   target (the AROS direction, and the host test harness, which had to pass
   -DSIZEOF_INT=4 to work around exactly this). Same guarded include the mad/
   sources use. -Wundef in the Makefile now catches this class at compile time. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "audio.h"
#include "stream.h"
#include "frame.h"
#include "synth.h"
#include "resample.h"
#include "filter.h"
#include "timer.h"
#include "tag.h"

#ifdef BUILD_WARPUP
#include <powerpc/powerpc.h>
#include <proto/powerpc.h>
#elif defined(BUILD_POWERUP)
#include <powerup/gcclib/powerup_protos.h>

#undef Open
#define Open PPCOpen
#undef Close
#define Close PPCClose
#undef Read
#define Read PPCRead
#undef Seek
#define Seek PPCSeek
#endif

#define DEFAULT_BUFFER_SIZE 8192

typedef struct
{
	BPTR fh;
	UBYTE *buffer;
	ULONG streamsize;
	ULONG streamskip;
	ULONG sampleskip;

	MPEGA_CTRL *ctrl;
	MPEGA_ACCESS *access;

	/* Scratch block for the 68k stack swap in library_init.c: the private stack
	   plus its MyStackSwapStruct, allocated once per stream instead of once per
	   call. Allocated and freed by library_init.c, this module only stores it,
	   because it is the only one that knows DecHandle's layout. NULL when the
	   opening caller had enough stack that no swap was needed. */
	APTR swapblk;

	/* Last frame header values SetStreamOptions was configured for. The output
	   configuration is a pure function of these plus ctrl (which is fixed for
	   the stream), so while they hold steady the configuration work can be
	   skipped entirely, see SetStreamOptions. Initialised to values no real
	   frame can carry so the first call always configures. */
	WORD cfg_layer;
	WORD cfg_channels;
	LONG cfg_frequency;

	/* Set once the end-of-stream guard bytes have been handed to libmad, so the
	   final flush happens exactly once and the next call really does report EOF.
	   Cleared on seek, which puts more data ahead of us again. See the refill in
	   WRAP_MPEGA_decode_frame. */
	WORD eof_flushed;

	/* Set by MPEGA_seek, cleared by the first frame that decodes afterwards.
	   While it is set, decode_frame rides out recoverable errors instead of
	   reporting them: a seek lands mid-stream, so the sync search can latch onto
	   a false header and the bit reservoir it needs is still behind us. */
	WORD post_seek;

	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	struct tag tag;
	mad_timer_t timer;

	struct audio_dither left_dither;
	struct audio_dither right_dither;

	mad_fixed_t gain;
	mad_fixed_t replaygain;

	struct resample_state resample[2];
	mad_fixed_t resampled[2][1152];
} DecHandle;

#ifdef __AROS__
#include <aros/asmcall.h>
#define CALLHOOK(hook, object, message, base) 	     \
    AROS_UFC4(IPTR, ((struct Hook *) hook)->h_Entry, \
    	AROS_UFCA(struct Hook *, hook, A0), 	     \
	AROS_UFCA(APTR, object, A2),	    	     \
	AROS_UFCA(APTR, message, A1),	    	     \
	AROS_UFCA(APTR, base, A4))	
#elif defined(__PPC__)
#ifdef BUILD_POWERUP
#include <powerup/ppclib/interface.h>
#define CALLHOOK(hook, object, message, base) \
({ ULONG ret; \
	struct Caos MyCaos; \
	MyCaos.M68kCacheMode = IF_CACHEFLUSHALL; \
	MyCaos.PPCCacheMode = IF_CACHEFLUSHALL; \
	MyCaos.a0 = (ULONG) hook; \
	MyCaos.a2 = (ULONG) object; \
	MyCaos.a1 = (ULONG) message; \
	MyCaos.a4 = (ULONG) base; \
	MyCaos.caos_Un.Function = hook->h_Entry; \
	ret = PPCCallM68k(&MyCaos); \
	ret; \
})
#elif defined(BUILD_WARPUP)
#define CALLHOOK(hook, object, message, base) \
({ ULONG ret; \
	struct PPCArgs args; \
	args.PP_Regs[PPREG_A0] = (ULONG)hook; \
	args.PP_Regs[PPREG_A2] = (ULONG)object; \
	args.PP_Regs[PPREG_A1] = (ULONG)message; \
	args.PP_Regs[PPREG_A4] = (ULONG)base; \
	args.PP_Code = (APTR)hook->h_Entry; \
	args.PP_Offset = 0; \
	args.PP_Flags = 0; \
	args.PP_Stack = NULL; \
	args.PP_StackSize = 0; \
	Run68K(&args); \
	ret = args.PP_Regs[PPREG_D0]; \
	ret; \
})
#elif defined(BUILD_OS4)
#include <proto/utility.h>
#define CALLHOOK(hook, object, message, base) \
({ ULONG ret; \
	SysBase->EmuWS[48] = base; \
	ret = CallHookPkt(hook, object, message); \
	ret; \
})
#else
#include <emul/emulregs.h>
#include <emul/emulinterface.h>
#define CALLHOOK(hook, object, message, base) \
({ ULONG ret; \
	REG_A0 = (ULONG) hook; \
	REG_A2 = (ULONG) object; \
	REG_A1 = (ULONG) message; \
	REG_A4 = (ULONG) base; \
	ret = (*MyEmulHandle->EmulCallDirect68k)(hook->h_Entry); \
	ret; \
})
#endif
#else
#if 0
static ULONG ASM MyCallHook(REG(a0, struct Hook *hook), REG(a2, APTR object), REG(a1, APTR message), REG(a4, APTR base))
{
	return hook->h_Entry();
}
#define CALLHOOK MyCallHook
#else
/* 68k hook call. MODERNIZED (gcc 6.x): operand registers (d0/a0/a1/a2/a4)
   removed from the clobber list (gcc 4+ forbids clobbering operand regs),
   and _base is now an explicit a4 input (it was declared but never passed,
   so a4base was not reliably loaded before the call). Register ABI unchanged. */
#define CALLHOOK(hook, object, message, base) \
({ register ULONG ret __asm("d0"); \
	register struct Hook *_hook __asm("a0") = hook; \
	register APTR _object __asm("a2") = object; \
	register APTR _message __asm("a1") = message; \
	register APTR _base __asm("a4") = base; \
	__asm volatile ("movel a0@(8:W),a3\n\t" "jsr a3@" : "=r" (ret) : "r" (_hook), "r" (_object), "r" (_message), "r" (_base) : "d1", "a3", "cc", "memory"); \
	ret; \
})
#endif
#endif


static __inline void SetStreamValues(MPEGA_STREAM *mpega_stream, struct mad_header *header)
{
	switch (header->mode)
	{
		case MAD_MODE_SINGLE_CHANNEL:
			mpega_stream->mode = MPEGA_MODE_MONO;
			break;

		case MAD_MODE_DUAL_CHANNEL:
			mpega_stream->mode = MPEGA_MODE_DUAL;
			break;

		case MAD_MODE_JOINT_STEREO:
			mpega_stream->mode = MPEGA_MODE_J_STEREO;
			break;

		case MAD_MODE_STEREO:
			mpega_stream->mode = MPEGA_MODE_STEREO;
			break;

		default:
			mpega_stream->mode = -1;
			break;
	}

	mpega_stream->norm = ((header->flags & MAD_FLAG_LSF_EXT) ? 2 : 1);
	mpega_stream->layer = header->layer;
	mpega_stream->bitrate = header->bitrate / 1000;
	mpega_stream->frequency = header->samplerate;
	mpega_stream->channels = ((header->mode == MAD_MODE_SINGLE_CHANNEL) ? 1 : 2);

	mpega_stream->private_bit = ((header->private_bits & MAD_PRIVATE_HEADER) ? 1 : 0);
	mpega_stream->copyright = ((header->flags & MAD_FLAG_COPYRIGHT) ? 1 : 0);
	mpega_stream->original = ((header->flags & MAD_FLAG_ORIGINAL) ? 1 : 0);
}

static __inline void ClearStreamValues(MPEGA_STREAM *mpega_stream)
{
	/* Dummy values */
	mpega_stream->mode = MPEGA_MODE_J_STEREO;
	mpega_stream->norm = 1;
	mpega_stream->layer = 3;
	mpega_stream->bitrate = 128;
	mpega_stream->frequency = 44100;
	mpega_stream->channels = 2;

	mpega_stream->private_bit = 0;
	mpega_stream->copyright = 0;
	mpega_stream->original = 0;

	mpega_stream->ms_duration = 1000;

	mpega_stream->dec_channels = 2;
	mpega_stream->dec_quality = 2;
	mpega_stream->dec_frequency = 44100;
}

/*
 * Output configuration: the resampler ratios, libmad's sample-rate options and
 * dec_frequency.
 *
 * Split out of SetStreamOptions because it is
 * a pure function of the frame header and ctrl, yet SetStreamOptions runs on
 * EVERY decoded frame, 38 times a second. Re-running it that often was not
 * merely wasteful (one or two DIVS.L plus, in any resampling configuration, a
 * resample_init with two more divisions and a 28-iteration division loop); it was
 * WRONG. resample_init() zeroes state->step and state->last, so the resampler's
 * cross-block interpolation state was destroyed at every frame boundary and the
 * interpolation branch in mad/resample.c was permanently dead.
 *
 * The caller now invokes this only when the header actually changes.
 */
static void ConfigureOutput(MPEGA_STREAM *mpega_stream, MPEGA_OUTPUT *output, DecHandle *dechandle)
{
	LONG freq;

	/* BUGFIX: start from "no resampling". Several paths below install
	   a sample-rate option WITHOUT calling resample_init, the QUART and HALF
	   cases and both default arms, and the old code left resample[].ratio
	   holding whatever a PREVIOUS configuration had put there. After a
	   mid-stream rate change (44.1 -> 22.05 kHz under a freq_max cap, routine in
	   concatenated files and in streams) the decoder therefore kept decimating
	   by the stale factor while honestly reporting the new dec_frequency: 54 %
	   speed and pitch for the rest of the file. Clearing it here covers every
	   path at once; anything that genuinely resamples calls resample_init below,
	   which overwrites this (and zeroes step/last, which are meaningless while
	   ratio is unity). */
	dechandle->resample[0].ratio = MAD_F_ONE;
	dechandle->resample[1].ratio = MAD_F_ONE;

	if (output->quality == 0)
	{
		switch (output->freq_div)
		{
			case 4:
				if ((freq = mpega_stream->frequency / 4) > 8000)
				{
					mad_stream_options(&dechandle->stream, MAD_OPTION_QUARTSAMPLERATE);
					mpega_stream->dec_frequency = freq;
					break;
				}
				/* Fall through */

			case 2:
				if ((freq = mpega_stream->frequency / 2) > 8000)
				{
					mad_stream_options(&dechandle->stream, MAD_OPTION_HALFSAMPLERATE);
					mpega_stream->dec_frequency = freq;
					break;
				}
				/* Fall through */

			case 0:
				if ((freq = mpega_stream->frequency) > output->freq_max)
				{
					LONG freqdiv = freq / output->freq_max;

					switch (freqdiv)
					{
						case 6:
						case 5:
						case 4:
							freq /= 4;
							mad_stream_options(&dechandle->stream, MAD_OPTION_QUARTSAMPLERATE);

							resample_init(&dechandle->resample[0], freq, output->freq_max);
							if (mpega_stream->dec_channels == 2)
								resample_init(&dechandle->resample[1], freq, output->freq_max);

							mpega_stream->dec_frequency = output->freq_max;
							break;

						case 3:
						case 2:
							freq /= 2;
							mad_stream_options(&dechandle->stream, MAD_OPTION_HALFSAMPLERATE);

							resample_init(&dechandle->resample[0], freq, output->freq_max);
							if (mpega_stream->dec_channels == 2)
								resample_init(&dechandle->resample[1], freq, output->freq_max);

							mpega_stream->dec_frequency = output->freq_max;
							break;

						default:
							mad_stream_options(&dechandle->stream, 0);

							resample_init(&dechandle->resample[0], mpega_stream->frequency, freq);
							if (mpega_stream->dec_channels == 2)
								resample_init(&dechandle->resample[1], mpega_stream->frequency, freq);

							mpega_stream->dec_frequency = freq;
							break;
					}

					break;
				}
				/* Fall through */

			default:
				mad_stream_options(&dechandle->stream, 0);
				mpega_stream->dec_frequency = mpega_stream->frequency;
				break;
		}
	} else {
		switch (output->freq_div)
		{
			case 4:
				if ((freq = mpega_stream->frequency / 4) > 8000)
				{
					resample_init(&dechandle->resample[0], mpega_stream->frequency, freq);
					if (mpega_stream->dec_channels == 2)
						resample_init(&dechandle->resample[1], mpega_stream->frequency, freq);

					mpega_stream->dec_frequency = freq;
					break;
				}
				/* Fall through */

			case 2:
				if ((freq = mpega_stream->frequency / 2) > 8000)
				{
					resample_init(&dechandle->resample[0], mpega_stream->frequency, freq);
					if (mpega_stream->dec_channels == 2)
						resample_init(&dechandle->resample[1], mpega_stream->frequency, freq);

					mpega_stream->dec_frequency = freq;
					break;
				}
				/* Fall through */

			case 0:
				if ((freq = mpega_stream->frequency) > output->freq_max)
				{
					resample_init(&dechandle->resample[0], freq, output->freq_max);
					if (mpega_stream->dec_channels == 2)
						resample_init(&dechandle->resample[1], freq, output->freq_max);

					mpega_stream->dec_frequency = output->freq_max;
					break;
				}
				/* Fall through */

			default:
				mpega_stream->dec_frequency = mpega_stream->frequency;
				break;
		}
	}
}


static __inline void SetStreamOptions(MPEGA_STREAM *mpega_stream, MPEGA_CTRL *ctrl, DecHandle *dechandle)
{
	MPEGA_LAYER *layer = ((mpega_stream->layer == 3) ? (&ctrl->layer_3) : (&ctrl->layer_1_2));
	MPEGA_OUTPUT *output;

	if (layer->force_mono || mpega_stream->channels == 1)
	{
		output = &layer->mono;
		mpega_stream->dec_channels = 1;
	} else {
		output = &layer->stereo;
		mpega_stream->dec_channels = 2;
	}

	/* BUGFIX: reconfigure only when the frame header actually
	   changed. ctrl is a private copy fixed for the stream, so the output
	   configuration depends on nothing else, and re-running it per frame was
	   destroying the resampler's interpolation state (see ConfigureOutput).

	   The quality/CRC/gain tail below deliberately stays per-frame: it is
	   idempotent, and folding it into the gate would silently change when
	   dechandle->gain is reset, which is a separate bug with its own
	   audible consequences. */
	if (mpega_stream->layer     != dechandle->cfg_layer ||
	    mpega_stream->channels  != dechandle->cfg_channels ||
	    mpega_stream->frequency != dechandle->cfg_frequency)
	{
		dechandle->cfg_layer     = mpega_stream->layer;
		dechandle->cfg_channels  = mpega_stream->channels;
		dechandle->cfg_frequency = mpega_stream->frequency;

		ConfigureOutput(mpega_stream, output, dechandle);
	}

#ifdef MPEGA_FORCE_HIFI
	mpega_stream->dec_quality = output->quality;

	/* FORCED HI-FI: always tolerate broken CRCs so any file plays (CISC's medium-
	   quality behaviour); quality is forced HIGH above, so this is the robustness half. */
	dechandle->stream.options |= MAD_OPTION_IGNORECRC;

	if (mpega_stream->dec_quality < 2)
		dechandle->gain = MAD_F_ONE;
#else
	if ((mpega_stream->dec_quality = output->quality) < 2)
	{
		dechandle->stream.options |= MAD_OPTION_IGNORECRC;
		dechandle->gain = MAD_F_ONE;
	}
#endif
}

/*
 * Payload size in bytes: the stream minus whatever was skipped in front of it.
 *
 * BUGFIX: this subtraction appeared unguarded at three sites, all
 * ULONG. streamskip CAN exceed streamsize: when ExamineFH fails or a bs_access
 * hook reports stream_size == 0, InitDecoder falls back to
 * "streamsize = max(bytes, skip)", i.e. the ID3 tag's own size, and
 * streamskip then grows past it. The subtraction wrapped to nearly 2^32, so
 * ms_duration came out around 33554413 ms (9h19m for a four-minute track) and
 * every Xing seek collapsed onto toc[0].
 */
static __inline ULONG AudioBytes(DecHandle *dechandle)
{
	return (dechandle->streamsize > dechandle->streamskip)
		 ? (dechandle->streamsize - dechandle->streamskip) : 0;
}

/*
 * Duration in milliseconds for a constant bitrate, in kbit/s.
 *
 * BUGFIX: the direct form, "(bytes * 8) / kbps", overflows ULONG once bytes
 * passes about 500 MB, the source even admitted it in a comment. At 600 MB
 * 600e6 * 8 = 4.8e9 wraps past 2^32 to ~5.1e8, so a 10-hour 128 kbit/s
 * audiobook reported about 66 minutes, and because MPEGA_seek scales the
 * requested position against ms_duration, every seek in such a file landed
 * wrong as well. Dividing first keeps every intermediate inside 32 bits and
 * costs one extra division in a function called once per open.
 */
static __inline ULONG AudioMs(ULONG bytes, ULONG kbps)
{
	if (kbps == 0) kbps = 8;

	return (bytes / kbps) * 8 + ((bytes % kbps) * 8) / kbps;
}

static __inline ULONG CheckID3(UBYTE *buffer)
{
	ULONG size = 0;

	if (buffer[0]=='I' && buffer[1]=='D' && buffer[2]=='3' && buffer[3]<0xFF && buffer[4]<0xFF && buffer[6]<0x80 && buffer[7]<0x80 && buffer[8]<0x80 && buffer[9]<0x80)
	{
		size = 10 + (((buffer[6] & 0x7F) << 21) | ((buffer[7] & 0x7F) << 14) | ((buffer[8] & 0x7F) << 7) | buffer[9] & 0x7F);

		if (buffer[5] & 0x10)
			size += 10;
	}

	return size;
}

static __inline LONG ReadFunc(DecHandle *dechandle, UBYTE *buffer, LONG len, APTR a4base)
{
	MPEGA_CTRL *ctrl = dechandle->ctrl;
	LONG bytes;

	if (ctrl->bs_access == NULL)
	{
		bytes = Read(dechandle->fh, buffer, len);
	} else {
		MPEGA_ACCESS *access = dechandle->access;

		access->func = MPEGA_BSFUNC_READ;
		access->data.read.buffer = buffer;
		access->data.read.num_bytes = len;

		bytes = CALLHOOK(ctrl->bs_access, (APTR)dechandle->fh, access, a4base);
	}

	return bytes;
}

/* BUGFIX (mpega Hi-Fi fork, 2026): ReplayGain used to be computed with double
   arithmetic, the only floating point in the whole library. On a plain-68020
   build there is no FPU, so gcc turns every double operation into a call
   through mathieeedoubbas.library / mathieeedoubtrans.library. Nothing ever
   OpenLibrary()s them: the library is linked -nostartfiles, so the libnix
   autoinit that would normally do it never runs, and the bases stay at the
   $FFFFFFFF autoinit sentinel. The first MP3 carrying a LAME ReplayGain tag
   then jumps through $FFFFFFFF into an odd address and freezes the machine
   inside MPEGA_open(), before a single frame is decoded. Every 68020 build
   since the 68040->68020 recompile had this; the 2004 original escaped only
   because 68040 code uses FPU instructions directly.

   The cure is to not want an FPU: gain = 10^(t/200) for t tenth-dB is one
   scalar per file, computed here in Q28 by binary decomposition, one
   pre-scaled factor per power-of-two tenth-dB step, at most 11 multiplies.
   Worst error across the realistic LAME range is ~27 micro-dB. Positive
   steps stop at 2^7: t is clamped to +180 (10^0.9 = 7.94, just under the
   Q28 ceiling of 8.0, the old +18 dB clamp existed for the same reason),
   so bits >= 2^8 are unreachable; the negative table runs to 2^10 to cover
   the -1750 clamp. */
static const mad_fixed_t rg_pos[8] = {
	0x102F6DF0,	/* 10^(+1/200) */
	0x105F6879,	/* 10^(+2/200) */
	0x10C109DD,	/* 10^(+4/200) */
	0x118B2CB9,	/* 10^(+8/200) */
	0x133C79A2,	/* 10^(+16/200) */
	0x17208574,	/* 10^(+32/200) */
	0x216DC1C8,	/* 10^(+64/200) */
	0x45D7B041,	/* 10^(+128/200) */
};
static const mad_fixed_t rg_neg[11] = {
	0x0FD11D0D,	/* 10^(-1/200) */
	0x0FA2C37F,	/* 10^(-2/200) */
	0x0F47A64E,	/* 10^(-4/200) */
	0x0E9798AE,	/* 10^(-8/200) */
	0x0D4EE787,	/* 10^(-16/200) */
	0x0B11BD5A,	/* 10^(-32/200) */
	0x07A87807,	/* 10^(-64/200) */
	0x03AA56E2,	/* 10^(-128/200) */
	0x00D6F60D,	/* 10^(-256/200) */
	0x000B4805,	/* 10^(-512/200) */
	0x000007F4,	/* 10^(-1024/200) */
};

static mad_fixed_t ReplayGainFixed(LONG t)
{
	const mad_fixed_t *tab = rg_pos;
	mad_fixed_t gain = MAD_F_ONE;
	int i;

	if (t < 0)
	{
		t = -t;
		tab = rg_neg;
	}

	/* ascending factors keep every partial product below the final value,
	   so nothing here can overflow Q28's +-8.0 range */
	for (i = 0; t; t >>= 1, i++)
		if (t & 1)
			gain = (mad_fixed_t)(((long long)gain * tab[i]) >> 28);

	return gain;
}

static LONG InitDecoder(MPEGA_STREAM *mpega_stream, MPEGA_CTRL *ctrl, APTR a4base)
{
	DecHandle *dechandle = (DecHandle *) mpega_stream->handle;
	ULONG skip;
	LONG bytes;

	ClearStreamValues(mpega_stream);

	mad_stream_init(&dechandle->stream);
	mad_frame_init(&dechandle->frame);
	mad_synth_init(&dechandle->synth);

	tag_init(&dechandle->tag);
	resample_init(&dechandle->resample[0], 44100, 44100);
	resample_init(&dechandle->resample[1], 44100, 44100);
	mad_timer_reset(&dechandle->timer);
	dechandle->gain = dechandle->replaygain = MAD_F_ONE;

	bytes = ReadFunc(dechandle, dechandle->buffer, ctrl->stream_buffer_size, a4base);

	if (bytes <= 0)
	{
		if (ctrl->check_mpeg) return 0;

		/* Force MAD_ERROR_BUFLEN on first decode */
		bzero(dechandle->buffer, MAD_BUFFER_GUARD);
		mad_stream_buffer(&dechandle->stream, dechandle->buffer, 0);

		return 1;
	}

	dechandle->streamskip = skip = (bytes >= 10) ? CheckID3(dechandle->buffer) : 0;
	if (dechandle->streamsize == 0) dechandle->streamsize = ((bytes > skip) ? (bytes) : (skip));

	/* Ensure that we have something to decode */
	if (skip + 2000 > (ULONG)bytes)	/* MODERNIZED: avoid unsigned underflow when bytes < 2000 */
	{
		LONG len = 0, size = bytes;

		if (skip < size)
		{
			bytes = len = size - skip;
			bcopy(dechandle->buffer + (size - len), dechandle->buffer, len);
			skip = size;
		}

		while (skip >= size)
		{
			skip -= size;
			size = ReadFunc(dechandle, dechandle->buffer + len, ctrl->stream_buffer_size - len, a4base);

			if (size <= 0)
			{
				/* BUGFIX: a fork regression. Upstream wrote
				   "skip > (bytes - 2000)" which, for bytes < 2000, underflowed
				   to a huge unsigned and was therefore FALSE, so small streams
				   skipped this whole block and opened fine. The modernized
				   "skip + 2000 > bytes" is true for them instead, and the
				   top-up read below then hits EOF and failed the open outright.
				   Any stream under 2000 bytes stopped working.

				   Reaching EOF here is only fatal if the skip has NOT been
				   satisfied, that means the tag runs past the end of the
				   file and there is genuinely nothing to decode. If the skip is
				   done and we are already holding data, a short file is simply
				   a short file. */
				if (skip == 0 && len > 0) { size = 0; break; }

				return 0;
			}
		}

		if (skip)
		{
			bytes = len = size - skip;
			bcopy(dechandle->buffer + (size - len), dechandle->buffer, len);
			size = ReadFunc(dechandle, dechandle->buffer + len, ctrl->stream_buffer_size - len, a4base);
		}

		/* BUGFIX: "bytes" must be the number of valid bytes now in
		   the buffer, which is always "len bytes preserved" + "size bytes just
		   read". It used to be left holding the FIRST read's count on one path
		   and then had size added to it.

		   That path is reached whenever skip decrements to exactly zero in the
		   loop above, i.e. an ID3v2 tag whose size is an exact multiple of
		   stream_buffer_size, which taggers that pad to a power of two and
		   files with embedded cover art produce routinely. Neither fix-up ran
		   (the "skip < size" one because the compare is false at equality, the
		   "if (skip)" one because skip is zero), so bytes became
		   8192 + 8192 = 16384 in an 8200-byte allocation: the bzero below wrote
		   8184 bytes past the end and mad_stream_buffer set bufend 8 KB beyond
		   it, after which libmad consumed adjacent pool memory as MPEG data.

		   The other two paths already set bytes = len before adding size, so
		   this is a no-op for them. */
		bytes = len;
		if (size > 0) bytes += size;
	}

	/* Defensive: the buffer is stream_buffer_size + MAD_BUFFER_GUARD, so this
	   makes the bzero provably in bounds no matter what the arithmetic above
	   ever does again. */
	if (bytes < 0) bytes = 0;
	if (bytes > (LONG)ctrl->stream_buffer_size) bytes = ctrl->stream_buffer_size;

	bzero(dechandle->buffer + bytes, MAD_BUFFER_GUARD);
	mad_stream_buffer(&dechandle->stream, dechandle->buffer, bytes);

	dechandle->stream.sync = 0;
	mad_stream_options(&dechandle->stream, MAD_OPTION_IGNORECRC);

	if (mad_frame_decode(&dechandle->frame, &dechandle->stream) == -1)
	{
		while (MAD_RECOVERABLE(dechandle->stream.error))
		{
			dechandle->stream.error = 0;
			if (mad_frame_decode(&dechandle->frame, &dechandle->stream) == 0) break;
		}

		if (dechandle->stream.error)
		{
			mad_stream_options(&dechandle->stream, 0);
			if (ctrl->check_mpeg || dechandle->streamsize <= (bytes + skip)) return 0;

			return 1;
		}
	}

	mad_stream_options(&dechandle->stream, 0);
	SetStreamValues(mpega_stream, &dechandle->frame.header);

	if (tag_parse(&dechandle->tag, &dechandle->stream) == 0)
	{
		skip = dechandle->stream.next_frame - dechandle->buffer;
		dechandle->streamskip += skip;

		if (dechandle->tag.flags & TAG_LAME)
		{
			struct tag_rgain *rgain = &dechandle->tag.lame.replay_gain[0];

			/*** TODO: Skip Encoder Delay samples when decoding the first frames ***/
			dechandle->sampleskip = dechandle->tag.lame.start_delay;

			if (dechandle->streamsize <= (bytes+skip) && dechandle->tag.lame.music_length > (bytes+skip))
				dechandle->streamsize = dechandle->tag.lame.music_length;

			if (dechandle->tag.lame.replay_gain[1].name == TAG_RGAIN_NAME_AUDIOPHILE && dechandle->tag.lame.replay_gain[1].originator != TAG_RGAIN_ORIGINATOR_UNSPECIFIED)
				rgain = &dechandle->tag.lame.replay_gain[1];

			if ((rgain->name == TAG_RGAIN_NAME_RADIO || rgain->name == TAG_RGAIN_NAME_AUDIOPHILE) && rgain->originator != TAG_RGAIN_ORIGINATOR_UNSPECIFIED)
			{
#ifdef MPEGA_FORCE_HIFI
				/* FORCED HI-FI: ReplayGain is intentionally NOT applied, keep unity gain for a
				   faithful, clip-free decode. gain/replaygain stay MAD_F_ONE; MPEGA_scale still works. */
#else
				LONG tenthdb = (LONG)rgain->adjustment + 60;	/* 6.0 dB pregain */

				/* same clamp as the old double code: +18 dB is the Q28 ceiling */
				if (tenthdb > 180) tenthdb = 180;
				else if (tenthdb < -1750) tenthdb = -1750;

				dechandle->gain = dechandle->replaygain = ReplayGainFixed(tenthdb);
#endif
			}
		}

		if (dechandle->tag.flags & TAG_VBR && dechandle->tag.xing.flags & TAG_XING_FRAMES)
		{
			ULONG seconds;
			mad_timer_t total = dechandle->frame.header.duration;

			mad_timer_multiply(&total, dechandle->tag.xing.frames);
			mpega_stream->ms_duration = mad_timer_count(total, MAD_UNITS_MILLISECONDS);
			seconds = ((mpega_stream->ms_duration > 1000) ? (mpega_stream->ms_duration / 1000) : (1));

			if (dechandle->tag.xing.flags & TAG_XING_BYTES)
			{
				mpega_stream->bitrate = ((dechandle->tag.xing.bytes / 125) / seconds);
			} else {
				mpega_stream->bitrate = ((AudioBytes(dechandle) / 125) / seconds);
			}
		}
		else mpega_stream->ms_duration = AudioMs(AudioBytes(dechandle), mpega_stream->bitrate);
	} else {
		skip = dechandle->stream.this_frame - dechandle->buffer;
		dechandle->streamskip += skip;

		if (dechandle->streamsize)
		{
#if 0
			mad_timer_t total = dechandle->frame.header.duration;

			mad_timer_multiply(&total, ((dechandle->streamsize - dechandle->streamskip) / (dechandle->stream.next_frame - dechandle->stream.this_frame)));
			mpega_stream->ms_duration = mad_timer_count(total, MAD_UNITS_MILLISECONDS);
#else
			/* Was: "(streamsize - streamskip) * 8 / bitrate", with the in-tree
			   comment "This will overflow on files over 500MB". AudioMs divides
			   first, so it no longer does; AudioBytes guards the subtraction. */
			mpega_stream->ms_duration = AudioMs(AudioBytes(dechandle), mpega_stream->bitrate);
#endif
		}

		mad_stream_buffer(&dechandle->stream, dechandle->buffer + skip, bytes - skip);
		mad_frame_mute(&dechandle->frame);
	}

	SetStreamOptions(mpega_stream, ctrl, dechandle);

	return 1;
}

/*
 * Accessors for the per-stream stack-swap scratch block (see DecHandle above).
 *
 * library_init.c owns the block's lifetime but cannot reach it directly:
 * DecHandle is private to this file and MPEGA_STREAM only exposes an opaque
 * "handle". These two functions are the whole interface. They are internal:
 * not in the library's function table, so they carry no ABI weight.
 */
APTR LIBPPC WRAP_MPEGA_get_swapblk(MPEGA_STREAM *mpega_stream)
{
	if (mpega_stream && mpega_stream->handle)
		return ((DecHandle *)mpega_stream->handle)->swapblk;

	return NULL;
}

void LIBPPC WRAP_MPEGA_set_swapblk(MPEGA_STREAM *mpega_stream, APTR blk)
{
	if (mpega_stream && mpega_stream->handle)
		((DecHandle *)mpega_stream->handle)->swapblk = blk;
}


static void FinishDecoder(DecHandle *dechandle)
{
	resample_finish(&dechandle->resample[0]);
	resample_finish(&dechandle->resample[1]);
	tag_finish(&dechandle->tag);
	mad_synth_finish(&dechandle->synth);
	mad_frame_finish(&dechandle->frame);
	mad_stream_finish(&dechandle->stream);
}

static __inline LONG ValidateFrequency(LONG freq)
{
	if (freq <= 0) return 48000;
	if (freq <= 8000) return 8000;
	if (freq <= 11025) return 11025;
	if (freq <= 12000) return 12000;
	if (freq <= 16000) return 16000;
	if (freq <= 22050) return 22050;
	if (freq <= 24000) return 24000;
	if (freq <= 32000) return 32000;
	if (freq <= 44100) return 44100;

	return 48000;
}

static __inline void ValidateCtrl(MPEGA_CTRL *ctrl)
{
	if (ctrl->stream_buffer_size < DEFAULT_BUFFER_SIZE)
		ctrl->stream_buffer_size = DEFAULT_BUFFER_SIZE;

	if (ctrl->layer_1_2.mono.freq_div < 0)
		ctrl->layer_1_2.mono.freq_div = 0;

	if (ctrl->layer_1_2.mono.freq_div > 2)
		ctrl->layer_1_2.mono.freq_div = 4;

	if (ctrl->layer_1_2.mono.quality < 0)
		ctrl->layer_1_2.mono.quality = 0;

	if (ctrl->layer_1_2.mono.quality > 2)
		ctrl->layer_1_2.mono.quality = 2;

	if (ctrl->layer_1_2.stereo.freq_div < 0)
		ctrl->layer_1_2.stereo.freq_div = 0;

	if (ctrl->layer_1_2.stereo.freq_div > 2)
		ctrl->layer_1_2.stereo.freq_div = 4;

	if (ctrl->layer_1_2.stereo.quality < 0)
		ctrl->layer_1_2.stereo.quality = 0;

	if (ctrl->layer_1_2.stereo.quality > 2)
		ctrl->layer_1_2.stereo.quality = 2;

	ctrl->layer_1_2.mono.freq_max = ValidateFrequency(ctrl->layer_1_2.mono.freq_max);
	ctrl->layer_1_2.stereo.freq_max = ValidateFrequency(ctrl->layer_1_2.stereo.freq_max);

	if (ctrl->layer_3.mono.freq_div < 0)
		ctrl->layer_3.mono.freq_div = 0;

	if (ctrl->layer_3.mono.freq_div > 2)
		ctrl->layer_3.mono.freq_div = 4;

	if (ctrl->layer_3.mono.quality < 0)
		ctrl->layer_3.mono.quality = 0;

	if (ctrl->layer_3.mono.quality > 2)
		ctrl->layer_3.mono.quality = 2;

	if (ctrl->layer_3.stereo.freq_div < 0)
		ctrl->layer_3.stereo.freq_div = 0;

	if (ctrl->layer_3.stereo.freq_div > 2)
		ctrl->layer_3.stereo.freq_div = 4;

	if (ctrl->layer_3.stereo.quality < 0)
		ctrl->layer_3.stereo.quality = 0;

	if (ctrl->layer_3.stereo.quality > 2)
		ctrl->layer_3.stereo.quality = 2;

	ctrl->layer_3.mono.freq_max = ValidateFrequency(ctrl->layer_3.mono.freq_max);
	ctrl->layer_3.stereo.freq_max = ValidateFrequency(ctrl->layer_3.stereo.freq_max);

#ifdef MPEGA_FORCE_HIFI
	/* FORCED HI-FI: ignore the caller's quality / freq_div / freq_max, always decode
	   at full quality and full native bandwidth (no downsampling). force_mono is left
	   alone (a legitimate channel request, not a fidelity reduction). The bugfix and
	   plain Hi-Fi variants honour the caller's settings instead. */
	ctrl->layer_1_2.mono.quality   = ctrl->layer_1_2.stereo.quality = MPEGA_QUALITY_HIGH;
	ctrl->layer_3.mono.quality     = ctrl->layer_3.stereo.quality   = MPEGA_QUALITY_HIGH;
	ctrl->layer_1_2.mono.freq_div  = ctrl->layer_1_2.stereo.freq_div = 0;
	ctrl->layer_3.mono.freq_div    = ctrl->layer_3.stereo.freq_div   = 0;
	ctrl->layer_1_2.mono.freq_max  = ctrl->layer_1_2.stereo.freq_max = 48000;
	ctrl->layer_3.mono.freq_max    = ctrl->layer_3.stereo.freq_max   = 48000;
#endif
}

static __inline LONG OutputPCM(MPEGA_STREAM *mpega_stream, DecHandle *dechandle, WORD *pcm[MPEGA_MAX_CHANNELS])
{
	WORD *outleft = pcm[0];
	WORD *outright = pcm[1];
	mad_fixed_t const *left = dechandle->synth.pcm.samples[0];
	mad_fixed_t const *right = dechandle->synth.pcm.samples[1];
	ULONG len = dechandle->synth.pcm.length;
	ULONG count;	/* MODERNIZED: count of (possibly resampled) samples to output */
	LONG nsamples, sample0, sample1;

	nsamples = len;

	if (mpega_stream->dec_channels == 1 || !outright)
	{
		/* BUGFIX: when a STEREO source is reduced to one channel,
		   because the player asked for force_mono, or because it supplied no
		   right-hand buffer, mix both channels instead of emitting the left
		   one and discarding the right. There was no L/R sum anywhere in the
		   tree and libmad's own MAD_OPTION_SINGLECHANNEL is #if 0'd out, so
		   anything panned hard right simply vanished: a guitar, a backing vocal,
		   half of a wide mix. The library reported success and the listener
		   heard a hole.

		   Halve before adding rather than after: mad_fixed_t carries the whole
		   part in the top four bits, so two near-full-scale samples can overflow
		   a 32-bit sum. The lost LSB is 28 fractional bits down and cannot
		   survive the reduction to 16 bits anyway.

		   A genuinely mono frame keeps the old path, samples[1] holds nothing
		   meaningful there. mpega_stream->channels is refreshed from every frame
		   header, so a mid-stream stereo/mono change is handled per frame. */
		if (mpega_stream->channels == 2)
		{
			/* resampled[1] is unused on this path, so it is free scratch. */
			mad_fixed_t *mix = dechandle->resampled[1];
			ULONG i;

			for (i = 0; i < len; i++)
				mix[i] = (left[i] >> 1) + (right[i] >> 1);

			left = mix;
		}

		if (dechandle->resample[0].ratio != MAD_F_ONE)
		{
			nsamples = resample_block(&dechandle->resample[0], len, left, dechandle->resampled[0]);
			left = dechandle->resampled[0];
		}

		count = nsamples;	/* MODERNIZED: was implicitly len; use resampled count */

		if (mpega_stream->dec_quality < 2)
		{
			while (count--)
			{
				sample0 = audio_linear_round(16, *left++);

				*outleft++ = sample0 & 0x0000FFFF;
			}
		} else {
			while (count--)
			{
				sample0 = audio_linear_dither(16, *left++, &dechandle->left_dither);

				*outleft++ = sample0 & 0x0000FFFF;
			}
		}
	} else {	/* Stereo */
		if (dechandle->resample[0].ratio != MAD_F_ONE)
		{
			nsamples = resample_block(&dechandle->resample[0], len, left, dechandle->resampled[0]);
			resample_block(&dechandle->resample[1], len, right, dechandle->resampled[1]);
			left = dechandle->resampled[0];
			right = dechandle->resampled[1];
		}

		count = nsamples;	/* MODERNIZED: was implicitly len; use resampled count */

		if (mpega_stream->dec_quality < 2)
		{
			while (count--)
			{
				sample0 = audio_linear_round(16, *left++);
				sample1 = audio_linear_round(16, *right++);

				*outleft++ = sample0 & 0x0000FFFF;
				*outright++ = sample1 & 0x0000FFFF;
			}
		} else {
			while (count--)
			{
				sample0 = audio_linear_dither(16, *left++, &dechandle->left_dither);
				sample1 = audio_linear_dither(16, *right++, &dechandle->right_dither);

				*outleft++ = sample0 & 0x0000FFFF;
				*outright++ = sample1 & 0x0000FFFF;
			}
		}
	}

	return nsamples;
}


MPEGA_STREAM * LIBPPC WRAP_MPEGA_open(char *stream_name, MPEGA_CTRL *ctrl, APTR a4base)
{
	if (ctrl)
	{
		MPEGA_STREAM *mpega_stream;

		if ((mpega_stream = malloc(sizeof(MPEGA_STREAM))))
		{
			DecHandle *dechandle;

			if ((dechandle = calloc(1, sizeof(DecHandle))))
			{
				mpega_stream->handle = dechandle;

				dechandle->fh = NULL;
				dechandle->access = NULL;

				if ((dechandle->ctrl = malloc(sizeof(MPEGA_CTRL))))
				{
					bcopy(ctrl, dechandle->ctrl, sizeof(MPEGA_CTRL));
					ValidateCtrl(dechandle->ctrl);

					if (ctrl->bs_access == NULL)
					{
						if (stream_name)
						{
							if ((dechandle->fh = Open(stream_name, MODE_OLDFILE)))
							{
								struct FileInfoBlock *fib;

								dechandle->streamsize = 0;

								if ((fib = malloc(sizeof(struct FileInfoBlock))))
								{
									if (ExamineFH(dechandle->fh, fib))
									{
										dechandle->streamsize = fib->fib_Size;
									}

									free(fib);
								}

								if ((dechandle->buffer = malloc(dechandle->ctrl->stream_buffer_size + MAD_BUFFER_GUARD)))
								{
									if (InitDecoder(mpega_stream, dechandle->ctrl, a4base) != 0)
									{
										return mpega_stream;
									}

									/* Symmetry: InitDecoder has run
									   mad_stream_init/frame_init/synth_init and
									   tag/resample init, so tear them down on the
									   failure path too rather than only on close.

									   HONEST NOTE: this was thought to leak
									   ~7 KB per rejected file (libmad's overlap and
									   main_data). That did NOT reproduce, those two
									   are allocated lazily inside mad_layer_III, which
									   a failing InitDecoder never reaches, and the
									   *_init calls themselves allocate nothing. Verified
									   with LeakSanitizer over several inputs that do
									   take this path. Kept as correct teardown symmetry,
									   not as a fix for a demonstrated leak. */
									FinishDecoder(dechandle);
									free(dechandle->buffer);
								}

								Close(dechandle->fh);
							}
						}
					} else {
						MPEGA_ACCESS *access;

						if ((access = malloc(sizeof(MPEGA_ACCESS))))
						{
							dechandle->access = access;

							access->func = MPEGA_BSFUNC_OPEN;
							access->data.open.stream_name = stream_name;
							access->data.open.buffer_size = dechandle->ctrl->stream_buffer_size;
							access->data.open.stream_size = 0;

							if ((dechandle->fh = CALLHOOK(ctrl->bs_access, NULL, access, a4base)))
							{
								dechandle->streamsize = access->data.open.stream_size;
								if (access->data.open.buffer_size > 0) dechandle->ctrl->stream_buffer_size = access->data.open.buffer_size;
								if (dechandle->ctrl->stream_buffer_size < DEFAULT_BUFFER_SIZE) dechandle->ctrl->stream_buffer_size = DEFAULT_BUFFER_SIZE;

								if ((dechandle->buffer = malloc(dechandle->ctrl->stream_buffer_size + MAD_BUFFER_GUARD)))
								{
									if (InitDecoder(mpega_stream, dechandle->ctrl, a4base) != 0)
									{
										return mpega_stream;
									}

									/* Symmetry: see the matching comment on
									   the dos.library path above, teardown symmetry,
									   not a demonstrated leak. */
									FinishDecoder(dechandle);
									free(dechandle->buffer);
								}

								access->func = MPEGA_BSFUNC_CLOSE;
								CALLHOOK(ctrl->bs_access, (APTR)dechandle->fh, access, a4base);
							}

							free(access);
						}
					}

					free(dechandle->ctrl);
				}

				free(dechandle);
			}

			free(mpega_stream);
		}
	}

	return NULL;
}

void LIBPPC WRAP_MPEGA_close(MPEGA_STREAM *mpega_stream, APTR a4base)
{
	if (mpega_stream)
	{
		DecHandle *dechandle = (DecHandle *) mpega_stream->handle;

		if (dechandle)
		{
			MPEGA_CTRL *ctrl = dechandle->ctrl;

			FinishDecoder(dechandle);

			if (ctrl->bs_access == NULL)
			{
				if (dechandle->fh) Close(dechandle->fh);
			} else {
				MPEGA_ACCESS *access = dechandle->access;

				access->func = MPEGA_BSFUNC_CLOSE;

				CALLHOOK(ctrl->bs_access, (APTR)dechandle->fh, access, a4base);
			}

			if (dechandle->ctrl) free(dechandle->ctrl);
			if (dechandle->buffer) free(dechandle->buffer);
			if (dechandle->access) free(dechandle->access);

			free(dechandle);
		}

		free(mpega_stream);
	}
}

LONG LIBPPC WRAP_MPEGA_decode_frame(MPEGA_STREAM *mpega_stream, WORD *pcm[MPEGA_MAX_CHANNELS], APTR a4base)
{
	if (mpega_stream && pcm && pcm[0])
	{
		DecHandle *dechandle = (DecHandle *) mpega_stream->handle;
		MPEGA_CTRL *ctrl = dechandle->ctrl;

		if (mad_frame_decode(&dechandle->frame, &dechandle->stream) == -1)
		{
			LONG error = dechandle->stream.error;

			/* BUGFIX: a file that ends in an ID3v1 tag reported
			   MPEGA_ERR_NO_SYNC instead of a clean end, so a player showed an
			   error at the end of every normal track.

			   Once a frame has decoded, stream.sync is set, and in that state
			   mad_header_decode does NOT search for the next sync word: it
			   tests the byte in front of it and fails immediately
			   (mad/frame.c, the "if (stream->sync)" arm). An ID3v1 tag begins
			   with 'T', so the first byte of the tag is a hard LOSTSYNC.

			   The end-of-stream fix below cannot help, because the tag
			   arrived as ordinary read data: "got" was positive, so
			   eof_flushed is still clear and LOSTSYNC falls through to
			   MPEGA_ERR_NO_SYNC.

			   If nothing that could start a frame is left in the buffer, this
			   is trailing metadata rather than a sync error. Say BUFLEN
			   instead and let the refill loop below run: it will read, get
			   nothing, set eof_flushed and hand over the guard bytes, and the
			   end-of-stream path then reports the clean EOF the caller was waiting for.
			   If more data does arrive, decoding simply continues.

			   Deliberately narrow. Damage in the middle of a file still
			   reports NO_SYNC exactly as before, because a sync word is still
			   present further along the buffer. Only a run of non-frame bytes
			   reaching the end of the buffer takes this path. */
			if (error == MAD_ERROR_LOSTSYNC && !dechandle->eof_flushed)
			{
				unsigned char const *scan = dechandle->stream.next_frame;
				unsigned char const *bufend = dechandle->stream.bufend;
				LONG framestart = 0;

				if (scan && bufend)
				{
					while (scan + 1 < bufend)
					{
						if (scan[0] == 0xFF && (scan[1] & 0xE0) == 0xE0)
						{
							framestart = 1;
							break;
						}

						++scan;
					}
				}

				if (!framestart) error = MAD_ERROR_BUFLEN;
			}

			if (error == MAD_ERROR_BUFLEN)
			{
				/* "bufsize" is the buffer's CAPACITY, "got" the number of bytes
				   actually read. The original used one variable, "bytes", for
				   both in turn, which is most of why the arithmetic below was
				   hard to get right. */
				LONG bufsize = ctrl->stream_buffer_size, len, got;
				LONG resync;

				/* BUGFIX: refill until a frame decodes or the stream truly ends.
				   The refill used to happen exactly ONCE per decode_frame call,
				   and one refill is often not enough:

				     - a handler that returns less than a frame at a time (PIPE:,
				       serial, network) needs several before a frame is complete;
				     - after a seek the first buffer routinely lands mid-frame.

				   Tracing a 44.1 kHz file seeked to 716 ms shows MAD_ERROR_BUFLEN
				   three times in a row before the decoder catches up. With a
				   single refill the third one escaped to the caller, which then
				   reported a bad frame (or, in older code, a false
				   EOF) on a perfectly good stream.

				   This terminates: every iteration either reads at least one byte
				   or takes the got == 0 branch, and the "frame larger than the
				   whole buffer" case below forces progress by dropping the
				   residue. */
				while (error == MAD_ERROR_BUFLEN)
				{
				len = 0;
				resync = 0;

				if (dechandle->stream.next_frame)
				{
					/* BUGFIX: measure the residue against the bytes
					   actually in the buffer, stream.bufend, not against the
					   buffer's capacity. The old form assumed the previous fill
					   had been a FULL one; after any short read (network or PIPE
					   hooks, and guaranteed once at end of file) it counted
					   beyond the valid data and spliced bytes left over from the
					   previous buffer generation into the bitstream, declaring
					   them part of the next frame. Audible as a stutter or a
					   repeated fragment.

					   BUGFIX: the length used to be
					   computed inside the bcopy argument list, so an
					   out-of-range value had already been acted on before it
					   could be rejected, and bcopy takes an UNSIGNED length,
					   so a negative len became ~0xFFFFE000. Range-check first. */
					len = (LONG)(dechandle->stream.bufend - dechandle->stream.next_frame);

					if (len < 0 || len > bufsize) return MPEGA_ERR_BADFRAME;

					if (len > 0) bcopy(dechandle->stream.next_frame, dechandle->buffer, len);
				}

				if (bufsize <= len)
				{
					/* BUGFIX: the frame claims to span more than the
					   whole buffer, so there is no room to read anything new.
					   The old code returned MPEGA_ERR_BADFRAME here WITHOUT
					   reading and without touching any decoder state, so the
					   next call re-decoded byte-identical input, hit the same
					   branch, and returned the same error forever. The stream
					   never advanced and EOF became unreachable: a player
					   looping until EOF would spin until the user killed it.

					   Drop the unusable residue and resync from fresh data.
					   One frame is lost; that is the right trade against a loop
					   with no exit. */
					len = 0;
					resync = 1;
				}

				got = ReadFunc(dechandle, dechandle->buffer + len, bufsize - len, a4base);

				if (got < 0) return MPEGA_ERR_BADVALUE;

				if (got == 0)
				{
					/* BUGFIX: true end of stream. A complete final
					   frame is usually still sitting in the residue, and libmad
					   refused it only because mad/frame.c requires
					   MAD_BUFFER_GUARD bytes to exist BEYOND the frame:

					       if (N + MAD_BUFFER_GUARD > end - stream->this_frame)

					   The wrapper always zeroed those guard bytes but never
					   counted them in the length handed to mad_stream_buffer, so
					   "end" never covered them and the last frame of every file
					   was silently dropped, about 26 ms, and MPEGA_time's final
					   value one frame short.

					   Hand them over now, once. Doing it mid-stream would be
					   wrong: libmad would consume the zero padding as real data
					   and the next residue calculation would splice it into the
					   bitstream. */
					if (len <= 0 || dechandle->eof_flushed) return MPEGA_ERR_EOF;

					dechandle->eof_flushed = 1;

					bzero(dechandle->buffer + len, MAD_BUFFER_GUARD);
					mad_stream_buffer(&dechandle->stream, dechandle->buffer, len + MAD_BUFFER_GUARD);
				}
				else
				{
					bzero(dechandle->buffer + len + got, MAD_BUFFER_GUARD);
					mad_stream_buffer(&dechandle->stream, dechandle->buffer, len + got);
				}

				/* mad_stream_buffer marks the stream as synced; after discarding
				   a partial frame we must search for the next sync word instead
				   (same idiom as InitDecoder). */
				if (resync) dechandle->stream.sync = 0;

				if (mad_frame_decode(&dechandle->frame, &dechandle->stream) == -1) error = dechandle->stream.error;
				else error = 0;
				}
			}

			/* BUGFIX: ride out recoverable errors on the first frames after a
			   seek. A seek lands in the middle of the data, and two things
			   follow from that, traced, not guessed:

			     - libmad's sync search can latch onto a false header inside
			       audio payload. Seeking to the very start of the audio gave
			       MAD_ERROR_BADLAYER (0x0102), and because pos == streamskip
			       there the junk-flush in MPEGA_seek is skipped, nothing rode it
			       out and the caller got MPEGA_ERR_BADFRAME on an intact file.
			     - the bit reservoir is behind us: Layer III frames reference
			       main_data from earlier frames, so the first frames after a
			       seek legitimately fail until it refills. On the VBR fixture at
			       1100 ms this surfaced as MAD_ERROR_BADHUFFDATA (0x0238).

			   Both are transient and both are recoverable errors, so retry until
			   a frame decodes. Bounded, and BUFLEN is excluded because the
			   refill loop above owns that case. */
			if (error && dechandle->post_seek && MAD_RECOVERABLE(error) && error != MAD_ERROR_BUFLEN)
			{
				LONG tries = 32;

				while (tries-- > 0)
				{
					dechandle->stream.error = 0;

					if (mad_frame_decode(&dechandle->frame, &dechandle->stream) == 0)
					{
						error = 0;
						break;
					}

					error = dechandle->stream.error;

					if (!MAD_RECOVERABLE(error) || error == MAD_ERROR_BUFLEN) break;
				}
			}

			if (error)
			{
				/* BUGFIX: once the end-of-stream guard
				   has been flushed, the only thing left in the buffer is that
				   zero padding. Any decode failure from here IS the end of the
				   stream, so report it as such, otherwise libmad's LOSTSYNC on
				   the guard bytes surfaced as MPEGA_ERR_NO_SYNC and a player
				   waiting for MPEGA_ERR_EOF saw an error instead of a clean end. */
				if (dechandle->eof_flushed) return MPEGA_ERR_EOF;

				switch (error)
				{
					case MAD_ERROR_BUFPTR:
						return MPEGA_ERR_BADVALUE;
						break;

					case MAD_ERROR_NOMEM:
						return MPEGA_ERR_MEM;
						break;

					case MAD_ERROR_LOSTSYNC:
						return MPEGA_ERR_NO_SYNC;
						break;

					case MAD_ERROR_BADBIGVALUES:
					case MAD_ERROR_BADBLOCKTYPE:
					case MAD_ERROR_BADSCFSI:
					case MAD_ERROR_BADHUFFDATA:
#ifdef MPEGA_FORCE_HIFI
						/* FORCED HI-FI: tolerate these recoverable Layer III errors (as the
						   original did at LOW quality) so damaged files keep playing;
						   robustness is decoupled from the forced HIGH fidelity. */
						break;
#else
						if (mpega_stream->dec_quality > 0) return MPEGA_ERR_BADFRAME;
						break;
#endif

					case MAD_ERROR_BADLAYER:
					case MAD_ERROR_BADBITRATE:
					case MAD_ERROR_BADSAMPLERATE:
					case MAD_ERROR_BADEMPHASIS:
					case MAD_ERROR_BADCRC:
					case MAD_ERROR_BADBITALLOC:
					case MAD_ERROR_BADSCALEFACTOR:
					case MAD_ERROR_BADMODE:
					case MAD_ERROR_BADFRAMELEN:
					case MAD_ERROR_BADDATAPTR:
					case MAD_ERROR_BADPART3LEN:
					case MAD_ERROR_BADHUFFTABLE:
					case MAD_ERROR_BADSTEREO:
						return MPEGA_ERR_BADFRAME;
						break;

					/* BUGFIX: BUFLEN had no case of its own and fell
					   into the default below. MAD_ERROR_BUFLEN is 0x0001, so
					   "return -error" returned -1, which is MPEGA_ERR_EOF.
					   A frame still short of data after a refill therefore
					   reported end of stream, and the player stopped mid-song. */
					case MAD_ERROR_BUFLEN:
						return MPEGA_ERR_BADFRAME;
						break;

					default:
						/* Never reflect a libmad error code onto the public error
						   space: MPEGA_ERR_* runs from -1 to -5, so any libmad
						   code in 0x0001-0x0005 negates onto a completely
						   unrelated documented error. Every code libmad can
						   actually produce is handled above; anything new is a
						   bad frame, not a fabricated EOF or out-of-memory. */
						return MPEGA_ERR_BADFRAME;
						break;
				}
			}
		}

		/* A frame decoded, so we are resynchronised and the reservoir has caught
		   up: stop tolerating errors that a normal stream should not produce. */
		dechandle->post_seek = 0;

		mad_timer_add(&dechandle->timer, dechandle->frame.header.duration);

		SetStreamValues(mpega_stream, &dechandle->frame.header);
		SetStreamOptions(mpega_stream, ctrl, dechandle);

		if (dechandle->gain != MAD_F_ONE)
			gain_filter(dechandle->gain, &dechandle->frame);

		mad_synth_frame(&dechandle->synth, &dechandle->frame);

		return OutputPCM(mpega_stream, dechandle, pcm);
	}

	return MPEGA_ERR_BADVALUE;
}

LONG LIBPPC WRAP_MPEGA_seek(MPEGA_STREAM *mpega_stream, ULONG ms_time_position, APTR a4base)
{
	if (mpega_stream)
	{
		DecHandle *dechandle = (DecHandle *) mpega_stream->handle;
		MPEGA_CTRL *ctrl = dechandle->ctrl;
		LONG pos, error;

		ULONG upos, achieved_ms = ms_time_position;

		if (dechandle->tag.flags & TAG_VBR && dechandle->tag.xing.flags & TAG_XING_TOC)
		{
			/* BUGFIX: step was "int" and step * toc[i] was signed
			   arithmetic. step can reach streamsize/256 (about 16.7 million for
			   a 4 GB stream) and toc[] is 0..255, so the product reaches ~4.28e9:
			   signed overflow, undefined behaviour, and in practice a
			   negative byte offset handed straight to Seek(). All unsigned now. */
			ULONG step, i = 99;

			step = (dechandle->tag.xing.flags & TAG_XING_BYTES)
				 ? (dechandle->tag.xing.bytes / 256)
				 : (AudioBytes(dechandle) / 256);

			if (ms_time_position < mpega_stream->ms_duration)
			{
				i = ((mpega_stream->ms_duration > 100) ? (ms_time_position / (mpega_stream->ms_duration / 100)) : (0));
				if (i > 99) i = 99;
			}

			upos = step * (ULONG)dechandle->tag.xing.toc[i] + dechandle->streamskip;

			/* BUGFIX: the clock used to be set to the position the
			   caller ASKED for. The TOC has 100 entries, so what we actually
			   reach is the start of percentile i, report that instead, or the
			   reported time drifts from the audio and the error compounds
			   across seeks. */
			achieved_ms = i * (mpega_stream->ms_duration / 100);
		} else {
			/* BUGFIX: "(bitrate * 125) * (ms / 1000)" truncated the
			   request to whole seconds, so everything in 0..999 ms seeked to
			   byte zero and sub-second scrubbing did nothing at all. Split the
			   multiply so the milliseconds survive without overflowing ULONG:
			   bytes_per_second * ms can reach 1.4e12 on a long file. */
			ULONG bps = (ULONG)mpega_stream->bitrate * 125UL;

			upos = bps * (ms_time_position / 1000UL)
			     + (bps * (ms_time_position % 1000UL)) / 1000UL
			     + dechandle->streamskip;

			achieved_ms = AudioMs(upos - dechandle->streamskip, mpega_stream->bitrate);
		}

		/* Keep the target inside the stream. dos.doc, dos.library/Seek: "You
		   cannot Seek() beyond the end of a file." */
		if (upos < dechandle->streamskip) upos = dechandle->streamskip;
		if (dechandle->streamsize > dechandle->streamskip && upos > dechandle->streamsize)
			upos = dechandle->streamsize;

		pos = (LONG)upos;

		if (ctrl->bs_access == NULL)
		{
			/* BUGFIX: "if (error > 0) error = 0" treats any
			   non-negative return as success, and Seek returns the PREVIOUS
			   position on success, so a failure that happens to return a
			   positive number passed as fine. Worse, dos.doc's BUGS entry for
			   Seek says the V36/V37 ROM filesystem and l:fastfilesystem
			   "returns the current position instead of -1 on an error. It sets
			   IoErr() to 0 on success, and an error code on an error." On
			   OS 2.0/2.1 the return value alone therefore cannot distinguish
			   the two, and playback silently continued from the wrong place
			   while the clock reported the target. IoErr() is the reliable
			   signal on every version. */
			SetIoErr(0);
			error = Seek(dechandle->fh, pos, OFFSET_BEGINNING);

			if (error == -1 || IoErr() != 0) error = -1;
			else error = 0;
		} else {
			MPEGA_ACCESS *access = dechandle->access;

			access->func = MPEGA_BSFUNC_SEEK;
			access->data.seek.abs_byte_seek_pos = pos;

			error = CALLHOOK(ctrl->bs_access, (APTR)dechandle->fh, access, a4base);
		}

		if (error == 0)
		{
			LONG bytes;

			bytes = ReadFunc(dechandle, dechandle->buffer, ctrl->stream_buffer_size, a4base);

			if (bytes == 0) return MPEGA_ERR_EOF;
			if (bytes < 0) return MPEGA_ERR_BADVALUE;

			bzero(dechandle->buffer + bytes, MAD_BUFFER_GUARD);
			mad_stream_buffer(&dechandle->stream, dechandle->buffer, bytes);

			/* There is data ahead of us again, so the end-of-stream guard flush
			   must be allowed to happen once more. Without this, a
			   seek backwards from the end would decode to the last frame and
			   then never flush it a second time. */
			dechandle->eof_flushed = 0;

			/* We have landed mid-stream: let decode_frame ride out the false
			   syncs and the still-empty bit reservoir that follow. */
			dechandle->post_seek = 1;

			mad_frame_mute(&dechandle->frame);
			mad_synth_mute(&dechandle->synth);
			dechandle->stream.sync = 0;

			/* If we seek somewhere other than the beginning we need to flush out junk audio */
			if (pos > dechandle->streamskip)
			{
				/* BUGFIX: a seek lands in the middle of the data, so libmad's
				   sync search can latch onto a false sync word inside audio
				   payload. Traced on the 8 kHz MPEG-2.5 fixture: both of the
				   flush decodes below failed with MAD_ERROR_BADBITRATE (0x0103):
				   a forbidden bitrate field read out of what was actually
				   sample data. Their return values were ignored, so the false
				   sync was still current when the caller's first
				   decode_frame ran, and the wrapper reported MPEGA_ERR_BADFRAME
				   on a perfectly intact file. Every mid-file seek in that stream
				   failed this way.

				   Ride out recoverable errors until a real frame header turns
				   up, exactly as InitDecoder does. Bounded so that a stream of
				   genuine garbage cannot spin here. */
				int tries = 32;

				while (tries-- > 0)
				{
					if (mad_frame_decode(&dechandle->frame, &dechandle->stream) == 0) break;
					if (!MAD_RECOVERABLE(dechandle->stream.error)) break;
					dechandle->stream.error = 0;
				}

				mad_frame_decode(&dechandle->frame, &dechandle->stream);
				mad_synth_frame(&dechandle->synth, &dechandle->frame);
			}

			/* BUGFIX: set the clock to the position we ACHIEVED, not
			   the one that was requested. The old form made MPEGA_time report
			   the request forever after, so the error persisted for the rest of
			   playback and compounded across seeks, and with the CBR
			   truncation fixed above, "achieved" is now a meaningful number.
			   (The commented-out original computed it in seconds only, losing
			   the same sub-second precision it was meant to preserve.) */
			mad_timer_set(&dechandle->timer, 0, achieved_ms, 1000);

			return 0;
		}
	}

	return MPEGA_ERR_BADVALUE;
}

LONG LIBPPC WRAP_MPEGA_time(MPEGA_STREAM *mpega_stream, ULONG *ms_time_position)
{
	if (mpega_stream && ms_time_position)
	{
		DecHandle *dechandle = (DecHandle *) mpega_stream->handle;

		*ms_time_position = mad_timer_count(dechandle->timer, MAD_UNITS_MILLISECONDS);
		return 0;
	}

	return MPEGA_ERR_BADVALUE;
}

LONG LIBPPC WRAP_MPEGA_find_sync(UBYTE *buffer, LONG buffer_size)
{
	if (buffer && buffer_size > 75)
	{
		LONG i = 0;

		while (i < buffer_size - 1)
		{
			if ((buffer[i] == 0xFF) && ((buffer[i+1] & 0xE0) == 0xE0)) return i;
			++i;
		}

		return MPEGA_ERR_NO_SYNC;
	}

	return MPEGA_ERR_BADVALUE;
}

LONG LIBPPC WRAP_MPEGA_scale(MPEGA_STREAM *mpega_stream, LONG scale_percent)
{
	if (mpega_stream)
	{
		DecHandle *dechandle = (DecHandle *) mpega_stream->handle;

		if (scale_percent <= 0) dechandle->gain = ((mad_fixed_t)0x00000001L);
		else
		{
			long long scaled;	/* MODERNIZED: 64-bit avoids signed overflow; old > MAD_F_MAX (==INT_MAX) test was dead */

			if (scale_percent > 800) scale_percent = 800;
			scaled = (((long long)dechandle->replaygain * scale_percent) / 100) + 1;

			if (scaled > (long long)MAD_F_MAX) dechandle->gain = MAD_F_MAX;
			else if (scaled < 1) dechandle->gain = ((mad_fixed_t)0x00000001L);
			else dechandle->gain = (mad_fixed_t)scaled;
		}

		return 0;
	}

	return MPEGA_ERR_BADVALUE;
}

#ifdef BUILD_POWERUP
APTR __LibFuncTable[] =
{
	WRAP_MPEGA_open,
	WRAP_MPEGA_close,
	WRAP_MPEGA_decode_frame,
	WRAP_MPEGA_seek,
	WRAP_MPEGA_time,
	WRAP_MPEGA_find_sync,
	WRAP_MPEGA_scale,
	NULL
};
#endif
