# A bare `make` builds the three supported 68020 variants. This has to be said
# explicitly: GNU make takes the FIRST target in the file as the default goal,
# and that is the library_init.o rule below, not `all`. Without this line a bare
# `make` compiles one object with the host cc and dies on -noixemul.
.DEFAULT_GOAL := all

# The three variants build one after another from the same object files, so
# they must never run in parallel: "make -j4" left a file named -bugfix that
# was the Hi-Fi build.
.NOTPARALLEL:

# Test build marker. "make <target> PHASE=2" stamps " [P2]" into the $VER
# string so an installed library can be identified with `version LIBS:mpega.library`.
# Set on the command line it propagates to the recursive sub-makes below.
# Leave it unset for a normal release build.
PHASE =
ifneq ($(strip $(PHASE)),)
PHASE_DEF = -DMPEGA_PHASE=$(PHASE)
endif

CC_OPTS = -O2 -fomit-frame-pointer -fno-strict-aliasing -noixemul -nostartfiles $(CFLAGS)
# -Wundef: warns when an undefined macro is used in an #if. That is exactly the
# shape of a bug fixed here: mad/fixed.h tests "#if SIZEOF_INT >= 4", and a
# translation unit that forgot config.h silently got a different mad_fixed_t.
CPP_OPTS = -I./mad -Iinclude_aros -DHAVE_CONFIG_H -DNDEBUG -Wundef $(PHASE_DEF) $(FPM)
MAD_OPTS = -Wall -O2 -fomit-frame-pointer -fno-strict-aliasing $(CFLAGS)
RM = rm -f
MV = mv -f

PROGNAME = mpega.library

LIB_SRC = library_init.c
LIB_OBJ = $(LIB_SRC:.c=.o)

WRAP_SRC = support.c wrap_mpega.c
WRAP_OBJ = $(WRAP_SRC:.c=.o)

MAD_SRC = mad/resample.c mad/tag.c mad/crc.c mad/fixed.c mad/bit.c mad/timer.c mad/stream.c mad/frame.c mad/synth.c mad/layer12.c mad/layer3.c mad/huffman.c
MAD_OBJ = $(MAD_SRC:.c=.o)

%.o: %.c
	$(CC) $(CPP_OPTS) $(CC_OPTS) -c $< -o $@

# CRITICAL: DO NOT REMOVE -fno-defer-pop.
#
# LIB_MPEGA_open/decode_frame/seek push three arguments, call the WRAP_* worker,
# then StackSwap() back to the caller's stack. With gcc's default -fdefer-pop
# (on at -O2) the 12-byte argument cleanup is sunk into the function epilogue,
# i.e. it executes AFTER the second StackSwap, so it adjusts the CALLER's
# stack pointer by 12 to "pop" bytes that lived on the swapped-away stack.
# The following moveml then restores d2/d3/a2-a4/a6 from the wrong slots and
# rts pops a garbage PC: an immediate crash for any caller with less than
# STACK_THRESHOLD (12 KB) of stack left, and the AmigaDOS default is 4000.
# gcc 2.95 emitted "adda.w #12,sp" between the call and the second swap, which
# is why the 2004 build was correct and a modern rebuild is not.
#
# Every variant recipe below checks the linked library for it
# (tests/check_swap_cleanup.py).
library_init.o: library_init.c
	$(CC) $(CPP_OPTS) $(CC_OPTS) -fno-defer-pop -c $< -o $@

# support.c is this library's C library, so gcc must not treat malloc, memset
# and calloc as the built-ins it would call instead: calloc's malloc + memset
# became a call to calloc itself, an endless recursion on the first open.
support.o: support.c
	$(CC) $(CPP_OPTS) $(CC_OPTS) -fno-builtin -c $< -o $@

# `make` with no target builds the three supported 68020 variants. It used to
# point at the 2004 _PPC and _040 targets, so a bare `make` died on a missing
# ppc-morphos-gcc. The PPC, _040 and AROS targets and their sources are gone
# (never built in this fork); they live on in original-archives/.
all: $(PROGNAME)_020_all

# Only layer3.c gets the inlined bit reader (see OPT_INLINE_BITS in mad/bit.h).
# That is where III_huffdecode refills its bit cache, and mad_bit_read is called
# about 333 times per frame. Turning the option on globally instead costs 15180
# bytes in the linked library, most of it in tag.c and frame.c, neither of which
# is anywhere near the hot path.
mad/layer3.o: mad/layer3.c
	$(CC) $(CPP_OPTS) $(CC_OPTS) -DOPT_INLINE_BITS -c $< -o $@

# Layer I and Layer II are built for size, not speed. An MP3 never enters them,
# and a .mp2 at 192 kbit/s already decodes at 78x realtime on an 020, so the
# only thing -O2 buys here is bytes in a library that has to fit on an Amiga.
mad/layer12.o: mad/layer12.c
	$(CC) $(CPP_OPTS) $(CC_OPTS) -Os -c $< -o $@

$(MAD_OBJ): CC_OPTS = $(MAD_OPTS)

$(PROGNAME): $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)
	$(CC) -o $@ $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ) $(CC_OPTS)

# The three 68020 variants share one source tree; see README "Variants".
# Each recipe deletes the objects FIRST: a build that died half way (Ctrl-C,
# a failed check) leaves objects compiled with another FPM, and make would
# happily link them into the next variant because they are newer than the
# sources. check_swap_cleanup.py inspects the linked library itself, so it
# catches a lost -fno-defer-pop.
$(PROGNAME)_020_all: $(PROGNAME)_020_bugfix $(PROGNAME)_020_hifi $(PROGNAME)_020_hifi_forced

# One recipe for the three flavours: $(1) the FPM flags, $(2) the name suffix.
define build_edition
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)
	$(MAKE) $(PROGNAME) "CC = m68k-amigaos-gcc" "CFLAGS = -m68020" "FPM = $(1)"
	m68k-amigaos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	sh tests/check_nofloat.sh $(PROGNAME)
	python3 tests/check_swap_cleanup.py $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME)-$(2)
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)
endef

$(PROGNAME)_020_bugfix:
	$(call build_edition,-DFPM_DEFAULT -DOPT_SPEED -DOPT_SSO,bugfix)

$(PROGNAME)_020_hifi:
	$(call build_edition,-DFPM_64BIT -DOPT_ACCURACY,hifi)

$(PROGNAME)_020_hifi_forced:
	$(call build_edition,-DFPM_64BIT -DOPT_ACCURACY -DMPEGA_FORCE_HIFI,hifi-forced)

clean:
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

# Regression harness (host build, needs no Amiga toolchain). See tests/README.md.
check:
	@bash tests/run.sh

check-asan:
	@bash tests/run.sh --asan

# The built flavours under vamos (amitools) against the host decode_ref, byte
# for byte. Run `make` first. Needs amiga-gcc and vamos; FAILS without them
# unless MPEGA_ALLOW_SKIP=1. See tests/README.md.
check-m68k:
	@bash tests/check_m68k.sh

# Rewrites the committed baselines. ONLY run this on a tree you have verified.
check-update:
	@bash tests/run.sh --update

fixtures:
	@bash tests/make_fixtures.sh

# On-Amiga decode benchmark. Opens LIBS:mpega.library by name, so it measures
# whichever build is installed and prints that library's own $VER with the
# result. See tests/bench/README.md.
bench:
	m68k-amigaos-gcc -O2 -fomit-frame-pointer -noixemul -m68020 \
	    -Iinclude_aros -o mpegabench tests/bench/mpegabench.c
	m68k-amigaos-strip --strip-unneeded --remove-section .comment mpegabench
	@echo "built: mpegabench  (copy it to the Amiga together with a test MP3)"

.PHONY: all clean check check-asan check-update check-m68k fixtures bench \
        $(PROGNAME)_020_all $(PROGNAME)_020_bugfix $(PROGNAME)_020_hifi \
        $(PROGNAME)_020_hifi_forced

