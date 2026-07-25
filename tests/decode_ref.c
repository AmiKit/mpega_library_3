/*
 * decode_ref, reference decoder driver for the mpega.library regression harness.
 *
 * Links the REAL wrap_mpega.c + mad/ sources against a small host shim and
 * decodes a file exactly the way a player would: WRAP_MPEGA_open(), then
 * WRAP_MPEGA_decode_frame() in a loop, then WRAP_MPEGA_close().
 *
 * Output is deterministic and byte-order independent:
 *   - PCM written as interleaved signed 16-bit LITTLE-endian, regardless of host
 *   - a one-line summary of the stream metadata on stdout
 * run.sh hashes both, so a change in either audio or reported metadata is caught.
 *
 * Usage:
 *   decode_ref <in> <out.pcm> [--mode=decode|seek] [--quality=0|1|2]
 *              [--freqdiv=N] [--freqmax=N] [--mono] [--bufsize=N] [--scale=N]
 *
 * Not built into the library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <libraries/mpega.h>

#include "wrap_mpega.h"

#define MAX_FRAMES 100000   /* hard stop: a zero-progress loop must not hang CI */

/* Defined in the shim: cap every dos.library Read, simulating the short reads a
   PIPE:, serial or network handler produces. 0 = behave like a plain file. */
extern LONG shim_max_read;

static void put_le16(FILE *f, WORD v)
{
    unsigned int u = (unsigned int)(unsigned short)v;
    fputc((int)( u        & 0xFF), f);
    fputc((int)((u >> 8)  & 0xFF), f);
}

static LONG opt_int(const char *arg, const char *key, LONG dflt, int *seen)
{
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0) { *seen = 1; return (LONG)strtol(arg + klen, NULL, 10); }
    return dflt;
}

int main(int argc, char **argv)
{
    MPEGA_CTRL    ctrl;
    MPEGA_STREAM *stream;
    WORD          left[MPEGA_PCM_SIZE], right[MPEGA_PCM_SIZE];
    WORD         *pcm[MPEGA_MAX_CHANNELS];
    FILE         *out;
    const char   *mode = "decode";
    LONG          quality = MPEGA_QUALITY_HIGH;
    LONG          freqdiv = 1, freqmax = 48000, bufsize = 0, scale = 0;
    LONG          seekms = -1;      /* -1 = a third of the way in */
    int           force_mono = 0, seen, i;
    long          frames = 0, samples = 0;
    LONG          n, last_error = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <in> <out.pcm> [options]\n", argv[0]);
        return 2;
    }

    for (i = 3; i < argc; i++) {
        seen = 0;
        if (strncmp(argv[i], "--mode=", 7) == 0)   { mode = argv[i] + 7; continue; }
        if (strcmp(argv[i], "--mono") == 0)        { force_mono = 1; continue; }
        quality = opt_int(argv[i], "--quality=", quality, &seen); if (seen) continue;
        freqdiv = opt_int(argv[i], "--freqdiv=", freqdiv, &seen); if (seen) continue;
        freqmax = opt_int(argv[i], "--freqmax=", freqmax, &seen); if (seen) continue;
        bufsize = opt_int(argv[i], "--bufsize=", bufsize, &seen); if (seen) continue;
        scale   = opt_int(argv[i], "--scale=",   scale,   &seen); if (seen) continue;
        shim_max_read = opt_int(argv[i], "--shortread=", shim_max_read, &seen); if (seen) continue;
        seekms  = opt_int(argv[i], "--seekms=",  seekms,  &seen); if (seen) continue;
        fprintf(stderr, "unknown option: %s\n", argv[i]);
        return 2;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access          = NULL;          /* use the dos.library path */
    ctrl.check_mpeg         = 1;
    ctrl.stream_buffer_size = bufsize;

    ctrl.layer_1_2.force_mono      = (WORD)force_mono;
    ctrl.layer_1_2.mono.freq_div   = (WORD)freqdiv;
    ctrl.layer_1_2.mono.quality    = (WORD)quality;
    ctrl.layer_1_2.mono.freq_max   = freqmax;
    ctrl.layer_1_2.stereo          = ctrl.layer_1_2.mono;
    ctrl.layer_3                   = ctrl.layer_1_2;

    if (!(out = fopen(argv[2], "wb"))) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }

    stream = WRAP_MPEGA_open(argv[1], &ctrl, NULL);
    if (!stream) {
        printf("OPEN_FAILED\n");
        fclose(out);
        return 1;
    }

    if (scale) WRAP_MPEGA_scale(stream, scale);

    /* Metadata as reported at open time, players configure AHI from this. */
    printf("open norm=%d layer=%d mode=%d bitrate=%d freq=%ld ch=%d "
           "ms=%lu dec_ch=%d dec_q=%d dec_freq=%ld\n",
           (int)stream->norm, (int)stream->layer, (int)stream->mode,
           (int)stream->bitrate, (long)stream->frequency, (int)stream->channels,
           (unsigned long)stream->ms_duration, (int)stream->dec_channels,
           (int)stream->dec_quality, (long)stream->dec_frequency);

    pcm[0] = left;
    pcm[1] = right;

    while (frames < MAX_FRAMES) {
        n = WRAP_MPEGA_decode_frame(stream, pcm, NULL);

        if (n <= 0) { last_error = n; break; }

        for (i = 0; i < n; i++) {
            put_le16(out, left[i]);
            put_le16(out, (stream->dec_channels == 1) ? left[i] : right[i]);
        }

        frames++;
        samples += n;

        /* Exercise the seek path once, a third of the way in. */
        if (strcmp(mode, "seek") == 0 && frames == 10) {
            ULONG target = (seekms >= 0) ? (ULONG)seekms : (stream->ms_duration / 3);
            LONG  sr = WRAP_MPEGA_seek(stream, target, NULL);
            ULONG pos = 0;
            WRAP_MPEGA_time(stream, &pos);
            printf("seek to=%lu rc=%ld now=%lu\n",
                   (unsigned long)target, (long)sr, (unsigned long)pos);
            if (sr < 0) break;
        }
    }

    {
        ULONG pos = 0;
        WRAP_MPEGA_time(stream, &pos);
        printf("done frames=%ld samples=%ld last=%ld end_ms=%lu\n",
               frames, samples, (long)last_error, (unsigned long)pos);
    }

    WRAP_MPEGA_close(stream, NULL);
    fclose(out);

    if (frames >= MAX_FRAMES) {
        fprintf(stderr, "ABORT: frame cap reached, possible zero-progress loop\n");
        return 3;
    }
    return 0;
}
