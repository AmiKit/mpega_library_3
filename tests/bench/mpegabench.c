/*
 * mpegabench, measure how fast mpega.library actually decodes, on the machine
 * that matters.
 *
 * Everything else in tests/ runs on a host PC and can only tell you whether the
 * OUTPUT changed. This one runs on the Amiga and tells you whether a change made
 * decoding faster or slower. It exists because cycle counts from the Motorola
 * manuals are estimates, and estimates have been wrong before.
 *
 * It opens the installed LIBS:mpega.library by name, so to compare two builds
 * you copy one in, run it, copy the other in, run it again. The library's own
 * $VER string is printed with every result, so a measurement can never be
 * attributed to the wrong build.
 *
 * Usage:
 *     mpegabench <file.mp3> [passes] [mono]
 *
 *     passes  repeat the decode this many times and report the best result
 *             (default 3). The best, not the mean: it is the one least polluted
 *             by whatever else the system was doing.
 *     mono    ask the library for a mono downmix, to measure that path too.
 *
 * Build (needs amiga-gcc):
 *     make bench
 *
 * Not part of the library. Nothing here is linked into mpega.library.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mpega.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *MPEGABase = NULL;

/*
 * Library call stubs.
 *
 * include_aros/clib/mpega_protos.h is AROS-only (it pulls aros/libcall.h), and
 * mpega.library is third-party so amiga-gcc ships no proto header for it. The
 * offsets and register assignment come straight from mpega.fd: "##bias 30",
 * six bytes per entry, in declaration order.
 *
 *     -30 MPEGA_open(filename a0, ctrl a1)
 *     -36 MPEGA_close(mpds a0)
 *     -42 MPEGA_decode_frame(mpds a0, pcm a1)
 *     -48 MPEGA_seek(mpds a0, ms d0)
 *     -54 MPEGA_time(mpds a0, ms_ptr a1)
 *
 * a0 and a1 are declared "+r", i.e. READ AND WRITTEN.
 *
 * That detail is the whole reason the first version of this benchmark failed
 * after exactly one frame. On AmigaOS d0/d1/a0/a1 are scratch: the callee is
 * free to destroy them. Declaring a0/a1 as inputs only left gcc believing they
 * still held the stream and the pcm array afterwards, so on the second turn of
 * the decode loop it reused the register contents instead of reloading them,
 * and passed garbage. The library's own argument check then rejected the call
 * with MPEGA_ERR_BADVALUE, or, with a different code layout, it crashed.
 *
 * They cannot go in the clobber list instead: gcc 4 and later reject clobbering
 * an operand register, which is the rule the library's own CALLHOOK had to be
 * corrected for. An operand that the callee destroys has to be an OUTPUT.
 * d1 is not an operand, so it belongs in the clobber list.
 *
 * a2-a6 and d2-d7 are callee-saved on Amiga and need neither treatment.
 *
 * a4 is left alone. The library's entry points take an a4base in a4 for the
 * caller's bs_access hook; we pass no hook, so it is never dereferenced.
 */

static __inline MPEGA_STREAM *MPEGA_open(char *name, MPEGA_CTRL *ctrl)
{
    register struct Library *b   __asm("a6") = MPEGABase;
    register MPEGA_STREAM   *res __asm("d0");
    register char           *nm  __asm("a0") = name;
    register MPEGA_CTRL     *ct  __asm("a1") = ctrl;

    __asm volatile ("jsr a6@(-30)"
                    : "=r" (res), "+r" (nm), "+r" (ct)
                    : "r" (b)
                    : "d1", "cc", "memory");
    return res;
}

static __inline void MPEGA_close(MPEGA_STREAM *s)
{
    register struct Library *b   __asm("a6") = MPEGABase;
    register LONG            res __asm("d0");
    register MPEGA_STREAM   *st  __asm("a0") = s;

    __asm volatile ("jsr a6@(-36)"
                    : "=r" (res), "+r" (st)
                    : "r" (b)
                    : "d1", "a1", "cc", "memory");
}

static __inline LONG MPEGA_decode_frame(MPEGA_STREAM *s, WORD **pcm)
{
    register struct Library *b   __asm("a6") = MPEGABase;
    register LONG            res __asm("d0");
    register MPEGA_STREAM   *st  __asm("a0") = s;
    register WORD          **pc  __asm("a1") = pcm;

    __asm volatile ("jsr a6@(-42)"
                    : "=r" (res), "+r" (st), "+r" (pc)
                    : "r" (b)
                    : "d1", "cc", "memory");
    return res;
}

static __inline LONG MPEGA_time(MPEGA_STREAM *s, ULONG *ms)
{
    register struct Library *b   __asm("a6") = MPEGABase;
    register LONG            res __asm("d0");
    register MPEGA_STREAM   *st  __asm("a0") = s;
    register ULONG          *mp  __asm("a1") = ms;

    __asm volatile ("jsr a6@(-54)"
                    : "=r" (res), "+r" (st), "+r" (mp)
                    : "r" (b)
                    : "d1", "cc", "memory");
    return res;
}

/* Static, not on the stack: 4.5 KB of buffers would eat into the headroom the
   library's stack-swap logic is deciding about, and skew what we are measuring. */
static WORD pcm_left[MPEGA_PCM_SIZE];
static WORD pcm_right[MPEGA_PCM_SIZE];

/* DateStamp ticks are 1/50 s. Over a multi-second decode that is ample; using
   timer.device would need a port and a request for no useful extra precision. */
#define TICKS_PER_SEC   50L
#define TICKS_PER_MIN   (60L * TICKS_PER_SEC)

static long stamp_ticks(void)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return ((ds.ds_Days * 1440L + ds.ds_Minute) * TICKS_PER_MIN) + ds.ds_Tick;
}

/* Pull the $VER out of the loaded library so the result is self-identifying. */
static void print_version(void)
{
    const char *id = (const char *)MPEGABase->lib_IdString;

    printf("library : %s\n", id ? id : "(no ID string)");
    printf("version : %ld.%ld\n", (long)MPEGABase->lib_Version, (long)MPEGABase->lib_Revision);
}

static void init_ctrl(MPEGA_CTRL *ctrl, int force_mono)
{
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->bs_access          = NULL;
    ctrl->check_mpeg         = 1;
    ctrl->stream_buffer_size = 0;          /* library default */

    ctrl->layer_1_2.force_mono    = (WORD)force_mono;
    ctrl->layer_1_2.mono.freq_div = 1;
    ctrl->layer_1_2.mono.quality  = MPEGA_QUALITY_HIGH;
    ctrl->layer_1_2.mono.freq_max = 48000;
    ctrl->layer_1_2.stereo        = ctrl->layer_1_2.mono;
    ctrl->layer_3                 = ctrl->layer_1_2;
}

/* One decode of the whole file. Returns elapsed ticks, or -1 on failure.
   ms_out receives the decoded duration so we can report a realtime factor. */
static long one_pass(const char *file, int force_mono, ULONG *ms_out,
                     long *frames_out, MPEGA_STREAM **info_out, int verbose)
{
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *s;
    WORD *pcm[MPEGA_MAX_CHANNELS];
    long t0, t1, frames = 0;
    LONG n;

    init_ctrl(&ctrl, force_mono);

    s = MPEGA_open((char *)file, &ctrl);
    if (!s) return -1;

    if (info_out) *info_out = s;

    if (verbose) {
        printf("stream  : norm=%d layer=%d mode=%d bitrate=%d freq=%ld ch=%d\n",
               (int)s->norm, (int)s->layer, (int)s->mode, (int)s->bitrate,
               (long)s->frequency, (int)s->channels);
        printf("        : dec_ch=%d dec_q=%d dec_freq=%ld duration=%lu ms\n",
               (int)s->dec_channels, (int)s->dec_quality,
               (long)s->dec_frequency, (unsigned long)s->ms_duration);
    }

    pcm[0] = pcm_left;
    pcm[1] = pcm_right;

    if (verbose)
        printf("args    : stream=$%08lx pcm=$%08lx pcm[0]=$%08lx pcm[1]=$%08lx\n",
               (unsigned long)s, (unsigned long)pcm,
               (unsigned long)pcm[0], (unsigned long)pcm[1]);

    t0 = stamp_ticks();

    while ((n = MPEGA_decode_frame(s, pcm)) > 0) {
        frames++;

        /* The library rejects a call whose stream/pcm/pcm[0] is not valid, and
           returns the same MPEGA_ERR_BADVALUE it uses for a read failure. When
           a decode stops early we need to know which of the two it was, so
           re-check the arguments here rather than guess afterwards. */
        if (verbose && frames <= 3)
            printf("        : frame %ld ok (n=%ld), args now stream=$%08lx pcm[0]=$%08lx\n",
                   frames, (long)n, (unsigned long)s, (unsigned long)pcm[0]);
    }

    t1 = stamp_ticks();

    if (verbose && n < 0)
        printf("atfail  : stream=$%08lx pcm=$%08lx pcm[0]=$%08lx pcm[1]=$%08lx\n",
               (unsigned long)s, (unsigned long)pcm,
               (unsigned long)pcm[0], (unsigned long)pcm[1]);

    /* Why the loop ended is the single most useful thing this tool can report
       when something is wrong: -1 EOF, -2 BADFRAME, -3 MEM, -4 NO_SYNC,
       -5 BADVALUE. A short decode that ends on anything but -1 is a defect. */
    if (verbose)
        printf("ended   : after %ld frames, decode_frame returned %ld\n",
               frames, (long)n);

    if (ms_out) MPEGA_time(s, ms_out);
    if (frames_out) *frames_out = frames;

    MPEGA_close(s);

    return t1 - t0;
}

int main(int argc, char **argv)
{
    const char *file;
    int passes = 3, force_mono = 0, i;
    long best = -1, worst = -1;
    ULONG ms = 0;
    long frames = 0;

    if (argc < 2) {
        printf("usage: mpegabench <file.mp3> [passes] [mono]\n");
        return RETURN_FAIL;
    }

    file = argv[1];

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "mono") == 0) force_mono = 1;
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') passes = atoi(argv[i]);
        else printf("ignoring unrecognised argument: %s\n", argv[i]);
    }
    if (passes < 1) passes = 1;

    MPEGABase = OpenLibrary("mpega.library", 2);
    if (!MPEGABase) {
        printf("cannot open mpega.library version 2 or higher\n");
        return RETURN_FAIL;
    }

    print_version();
    printf("file    : %s\n", file);
    printf("mode    : %s, %d pass%s\n", force_mono ? "force_mono" : "stereo",
           passes, passes == 1 ? "" : "es");

    /* How much stack is left decides WHICH code path the library takes: below
       its STACK_THRESHOLD it swaps to a private stack, above it it calls
       straight through. Those are very different pieces of code, and comparing
       two builds that took different paths compares nothing. Builds have used
       thresholds of 8192 and 12288, so this number is exactly what
       tells you whether a comparison is valid. Raise it with "stack 60000". */
    {
        struct Task *me = FindTask(NULL);
        register char *sp __asm("sp");

        __asm("" : "=r" (sp));

        printf("stack   : %ld bytes free (total %ld)\n",
               (long)(sp - (char *)me->tc_SPLower),
               (long)((char *)me->tc_SPUpper - (char *)me->tc_SPLower));
    }

    /* One warm-up pass: the first run pays for the file cache and for any
       one-time allocation, and we are measuring steady-state decoding. It is
       also the verbose one, so a failure is diagnosed rather than just timed. */
    if (one_pass(file, force_mono, &ms, &frames, NULL, 1) < 0) {
        printf("cannot open the stream (not an MPEG audio file?)\n");
        CloseLibrary(MPEGABase);
        return RETURN_FAIL;
    }

    for (i = 0; i < passes; i++) {
        long t = one_pass(file, force_mono, &ms, &frames, NULL, 0);

        if (t < 0) { printf("pass %d failed\n", i + 1); continue; }

        printf("  pass %d: %ld ticks (%ld.%02ld s)\n",
               i + 1, t, t / TICKS_PER_SEC, ((t % TICKS_PER_SEC) * 100) / TICKS_PER_SEC);

        if (best  < 0 || t < best)  best  = t;
        if (worst < 0 || t > worst) worst = t;
    }

    printf("\n");
    printf("frames  : %ld\n", frames);
    printf("audio   : %lu ms\n", (unsigned long)ms);

    if (best <= 0) {
        printf("elapsed : below the clock's resolution, use a longer file\n");
    } else {
        unsigned long elapsed_ms = (unsigned long)best * 1000UL / TICKS_PER_SEC;

        printf("elapsed : %lu ms (best of %d)\n", elapsed_ms, passes);

        /* Spread between the fastest and slowest pass. Taking the best of N is
           not enough on its own: a run where every pass was slowed by something
           else on the system still reports a best that is simply wrong. One
           such run in this project turned into a published claim of "57 %
           faster" that was off by a factor of four. If the passes disagree,
           the number below the line is not trustworthy, re-run it. */
        if (worst > best && best > 0) {
            unsigned long spread = (unsigned long)(worst - best) * 100UL / (unsigned long)best;

            printf("spread  : %lu%% between fastest and slowest pass%s\n",
                   spread, (spread > 10) ? "  <-- TOO NOISY, RE-RUN" : "");
        }

        /* The number that matters: how much audio is decoded per second of CPU.
           Below 1.00 the machine cannot keep up and playback will break up.
           Printed in hundredths to stay in integer arithmetic. */
        if (elapsed_ms > 0) {
            unsigned long factor = (unsigned long)ms * 100UL / elapsed_ms;

            printf("realtime: %lu.%02lux\n", factor / 100, factor % 100);
        }
    }

    CloseLibrary(MPEGABase);
    return RETURN_OK;
}
