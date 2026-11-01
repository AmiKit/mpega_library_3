/*
 * m68k_decode: decode_ref for the Amiga side. Opens LIBS:mpega.library, decodes
 * a file the way decode_ref does on the host and writes the same stdout lines
 * and the same interleaved little-endian 16-bit PCM, so the two can be compared
 * byte for byte. Built and run under vamos by tests/check_m68k.sh. Not built
 * into the library.
 *
 * Options: --mode=seek --seekms=N --quality=N --freqdiv=N --freqmax=N --mono
 *          --scale=N --monoq=N --monodiv=N --bufsize=N
 *          --hook=1|2  read through a bs_access hook (1: stream_size 0, 2: real size)
 *          --shortread=N  cap every hook read at N bytes
 *          --lowstack=1|2  make the library swap stacks (1: after the open, 2: before it)
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <utility/hooks.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/mpega.h>
#include "mpega_calls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *MPEGABase = NULL;

/* bs_access hook, called by the library's 68k CALLHOOK: a0 hook, a2 object
   (the handle we returned), a1 message. */
static int  hook_mode;
static LONG hook_cap;
/* --lowstack: the hook runs inside the library, so its sp shows whether the
   library swapped stacks. vamos gives a real 64 KB stack, so a library that
   never swaps decodes fine here and would crash on a 4000-byte stack. */
static char *stack_top;
static LONG probes, noswap;
static ULONG hook_entry(struct Hook *h __asm("a0"), APTR obj __asm("a2"), MPEGA_ACCESS *a __asm("a1"))
{
    BPTR fh = (BPTR)obj;
    LONG n;

    (void)h;
    if (stack_top) {
        register char *sp __asm("sp");
        __asm("" : "=r" (sp));
        probes++;
        if (sp <= stack_top && sp > stack_top - 8192) noswap++;   /* still on our own stack */
    }
    switch (a->func) {
    case MPEGA_BSFUNC_OPEN:
        if (!(fh = Open((STRPTR)a->data.open.stream_name, MODE_OLDFILE))) return 0;
        a->data.open.stream_size = 0;
        if (hook_mode == 2) {
            Seek(fh, 0, OFFSET_END);
            a->data.open.stream_size = Seek(fh, 0, OFFSET_BEGINNING);
        }
        return (ULONG)fh;
    case MPEGA_BSFUNC_READ:
        n = a->data.read.num_bytes;
        if (hook_cap > 0 && n > hook_cap) n = hook_cap;
        return (ULONG)Read(fh, a->data.read.buffer, n);
    case MPEGA_BSFUNC_SEEK:
        return (Seek(fh, a->data.seek.abs_byte_seek_pos, OFFSET_BEGINNING) < 0) ? (ULONG)-1 : 0;
    case MPEGA_BSFUNC_CLOSE:
        if (fh) Close(fh);
        return 0;
    }
    return 0;
}
static struct Hook bs_hook = { { NULL, NULL }, (ULONG (*)())hook_entry, NULL, NULL };

static WORD left[MPEGA_PCM_SIZE], right[MPEGA_PCM_SIZE];

/* vamos leaves tc_SPLower at 0, so the library never sees a small stack. Claim
   that 4000 bytes are left (the AmigaDOS default) and it swaps to its own. */
static void low_stack(void)
{
    struct Task *t = FindTask(NULL);
    t->tc_SPLower = (APTR)((char *)&t - 4000);
    stack_top = (char *)&t;
}

static LONG opt(const char *arg, const char *key, LONG dflt, int *seen)
{
    size_t k = strlen(key);
    if (strncmp(arg, key, k) == 0) { *seen = 1; return strtol(arg + k, NULL, 10); }
    return dflt;
}

int main(int argc, char **argv)
{
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *s;
    WORD *pcm[MPEGA_MAX_CHANNELS];
    FILE *out;
    const char *mode = "decode";
    LONG quality = MPEGA_QUALITY_HIGH, freqdiv = 1, freqmax = 48000, bufsize = 0, scale = 0;
    LONG seekms = -1, monoq = -1, monodiv = -1, lowstack = 0, n, last = 0;
    long frames = 0, samples = 0;
    int force_mono = 0, seen, i;
    unsigned char *o;
    static unsigned char obuf[MPEGA_PCM_SIZE * 4];

    if (argc < 3) { printf("usage: m68k_decode <in> <out.pcm> [options]\n"); return 20; }
    for (i = 3; i < argc; i++) {
        seen = 0;
        if (strncmp(argv[i], "--mode=", 7) == 0) { mode = argv[i] + 7; continue; }
        if (strcmp(argv[i], "--mono") == 0) { force_mono = 1; continue; }
        quality = opt(argv[i], "--quality=", quality, &seen); if (seen) continue;
        freqdiv = opt(argv[i], "--freqdiv=", freqdiv, &seen); if (seen) continue;
        freqmax = opt(argv[i], "--freqmax=", freqmax, &seen); if (seen) continue;
        bufsize = opt(argv[i], "--bufsize=", bufsize, &seen); if (seen) continue;
        scale   = opt(argv[i], "--scale=",   scale,   &seen); if (seen) continue;
        seekms  = opt(argv[i], "--seekms=",  seekms,  &seen); if (seen) continue;
        monoq   = opt(argv[i], "--monoq=",   monoq,   &seen); if (seen) continue;
        monodiv = opt(argv[i], "--monodiv=", monodiv, &seen); if (seen) continue;
        hook_mode = (int)opt(argv[i], "--hook=", hook_mode, &seen); if (seen) continue;
        hook_cap  = opt(argv[i], "--shortread=", hook_cap, &seen); if (seen) continue;
        lowstack  = opt(argv[i], "--lowstack=", lowstack, &seen); if (seen) continue;
        printf("unknown option: %s\n", argv[i]);
        return 20;
    }

    if (!(MPEGABase = OpenLibrary((STRPTR)"mpega.library", 2))) { printf("NO_LIBRARY\n"); return 20; }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access = hook_mode ? &bs_hook : NULL;
    ctrl.check_mpeg = 1;
    ctrl.stream_buffer_size = bufsize;
    ctrl.layer_1_2.force_mono    = (WORD)force_mono;
    ctrl.layer_1_2.mono.freq_div = (WORD)freqdiv;
    ctrl.layer_1_2.mono.quality  = (WORD)quality;
    ctrl.layer_1_2.mono.freq_max = freqmax;
    ctrl.layer_1_2.stereo        = ctrl.layer_1_2.mono;
    if (monoq >= 0)   ctrl.layer_1_2.mono.quality  = (WORD)monoq;
    if (monodiv >= 0) ctrl.layer_1_2.mono.freq_div = (WORD)monodiv;
    ctrl.layer_3                 = ctrl.layer_1_2;

    if (!(out = fopen(argv[2], "wb"))) { printf("cannot write %s\n", argv[2]); CloseLibrary(MPEGABase); return 20; }

    if (lowstack == 2) low_stack();
    if (!(s = MPEGA_open(argv[1], &ctrl))) {
        printf("OPEN_FAILED\n");
        fclose(out);
        CloseLibrary(MPEGABase);
        return 5;
    }
    if (scale) MPEGA_scale(s, scale);
    if (lowstack == 1) low_stack();

    printf("open norm=%d layer=%d mode=%d bitrate=%d freq=%ld ch=%d "
           "ms=%lu dec_ch=%d dec_q=%d dec_freq=%ld\n",
           (int)s->norm, (int)s->layer, (int)s->mode, (int)s->bitrate, (long)s->frequency,
           (int)s->channels, (unsigned long)s->ms_duration, (int)s->dec_channels,
           (int)s->dec_quality, (long)s->dec_frequency);

    pcm[0] = left;
    pcm[1] = right;
    while (frames < 100000) {
        n = MPEGA_decode_frame(s, pcm);
        if (n <= 0) { last = n; break; }
        o = obuf;
        for (i = 0; i < n; i++) {
            UWORD l = (UWORD)left[i], r = (UWORD)((s->dec_channels == 1) ? left[i] : right[i]);
            *o++ = l & 0xFF; *o++ = l >> 8; *o++ = r & 0xFF; *o++ = r >> 8;
        }
        fwrite(obuf, 1, (size_t)(o - obuf), out);
        frames++;
        samples += n;
        if (strcmp(mode, "seek") == 0 && frames == 10) {
            ULONG target = (seekms >= 0) ? (ULONG)seekms : (s->ms_duration / 3), pos = 0;
            LONG sr = MPEGA_seek(s, target);
            MPEGA_time(s, &pos);
            printf("seek to=%lu rc=%ld now=%lu\n", (unsigned long)target, (long)sr, (unsigned long)pos);
            if (sr < 0) break;
        }
    }
    {
        ULONG pos = 0;
        MPEGA_time(s, &pos);
        printf("done frames=%ld samples=%ld last=%ld end_ms=%lu\n", frames, samples, (long)last, (unsigned long)pos);
        /* the host prints no such line, so check-m68k reports the run as differing */
        if (stack_top && (noswap || !probes)) printf("NOSWAP %ld of %ld hook calls\n", (long)noswap, (long)probes);
    }
    MPEGA_close(s);
    fclose(out);
    CloseLibrary(MPEGABase);
    return 0;
}
