# A bare `make` builds the three supported 68020 variants. This has to be said
# explicitly: GNU make takes the FIRST target in the file as the default goal,
# and that is the library_init.o rule below, not `all`. Without this line a bare
# `make` compiles one object with the host cc and dies on -noixemul.
.DEFAULT_GOAL := all

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

PUP_SRC = support_pup.c
PUP_OBJ = $(PUP_SRC:.c=.o)

STARTUP_SRC = startup_pup.c
STARTUP_OBJ = $(STARTUP_SRC:.c=.o)

WRAP_SRC = support.c wrap_mpega.c
WRAP_OBJ = $(WRAP_SRC:.c=.o)

MAD_SRC = mad/filter.c mad/resample.c mad/tag.c mad/crc.c mad/fixed.c mad/bit.c mad/timer.c mad/stream.c mad/frame.c mad/synth.c mad/layer12.c mad/layer3.c mad/huffman.c
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
# STACK_THRESHOLD (8 KB) of stack left, and the AmigaDOS default is 4000.
# gcc 2.95 emitted "adda.w #12,sp" between the call and the second swap, which
# is why the 2004 build was correct and a modern rebuild is not.
#
# `make check-stackswap` asserts the generated code still gets this right.
library_init.o: library_init.c
	$(CC) $(CPP_OPTS) $(CC_OPTS) -fno-defer-pop -c $< -o $@

# `make` with no target builds the three supported 68020 variants. It used to
# point at the 2004 _PPC and _040 targets, so a bare `make` died on a missing
# ppc-morphos-gcc. Those targets are still below, kept for reference; see the
# README for why the PPC ones cannot be built here at all.
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

mad_build: $(MAD_OBJ)

mad.stmp:
	$(MAKE) mad_build "CC_OPTS = \$$(MAD_OPTS)"

$(PROGNAME): $(LIB_OBJ) $(WRAP_OBJ) mad.stmp
	$(CC) -o $@ $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ) $(ADD_OBJ) $(CC_OPTS)

$(PROGNAME)_ELF: $(LIB_OBJ) $(WRAP_OBJ) mad.stmp
#	ppc-morphos-ld -r -o $(PROGNAME) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)
	$(CC) $(CC_OPTS) -o $(PROGNAME) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

elfobject.o: $(STARTUP_OBJ) $(WRAP_OBJ)
	ppc-morphos-ld -r -fl libnix -o elfobject.elf $(STARTUP_OBJ) $(WRAP_OBJ) $(MAD_OBJ) -lc
	ppc-morphos-strip --strip-unneeded --keep-symbol __LibFuncTable --remove-section .comment elfobject.elf
	Data2Object elfobject.elf elfobject.o SECTION elf SYMBOL __ElfObject

$(PROGNAME)_PUP: $(LIB_OBJ) $(PUP_OBJ)
	$(CC) -o $(PROGNAME) $(LIB_OBJ) $(PUP_OBJ) elfobject.o $(CC_OPTS)

# The three 68020 variants share one source tree; see README "Variants".
$(PROGNAME)_020_all: $(PROGNAME)_020_bugfix $(PROGNAME)_020_hifi $(PROGNAME)_020_hifi_forced

$(PROGNAME)_020_bugfix:
	$(MAKE) $(PROGNAME) "CC = m68k-amigaos-gcc" "CFLAGS = -m68020" "FPM = -DFPM_DEFAULT -DOPT_SPEED -DOPT_SSO"
	m68k-amigaos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	sh tests/check_nofloat.sh $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME)-bugfix
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

$(PROGNAME)_020_hifi:
	$(MAKE) $(PROGNAME) "CC = m68k-amigaos-gcc" "CFLAGS = -m68020" "FPM = -DFPM_64BIT -DOPT_ACCURACY"
	m68k-amigaos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	sh tests/check_nofloat.sh $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME)-hifi
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

$(PROGNAME)_020_hifi_forced:
	$(MAKE) $(PROGNAME) "CC = m68k-amigaos-gcc" "CFLAGS = -m68020" "FPM = -DFPM_64BIT -DOPT_ACCURACY -DMPEGA_FORCE_HIFI"
	m68k-amigaos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	sh tests/check_nofloat.sh $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME)-hifi-forced
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

$(PROGNAME)_040:
	$(MAKE) $(PROGNAME) "CC = m68k-amigaos-gcc" "CFLAGS = -m68040" "FPM = -DFPM_DEFAULT -DOPT_SPEED -DOPT_SSO"
#	$(MAKE) $(PROGNAME) "CC = m68k-amigaos-gcc" "CFLAGS = -m68040" "FPM = -DFPM_M68K -DOPT_SPEED -DOPT_SSO"
	m68k-amigaos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	sh tests/check_nofloat.sh $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME).040
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

$(PROGNAME)_PPC:
	$(MAKE) $(PROGNAME)_ELF "CC = ppc-morphos-gcc" "CFLAGS = -mcpu=603e -mmultiple" "FPM = -DFPM_PPC -DOPT_ACCURACY -DBUILD_MORPHOS"
	ppc-morphos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME).elf
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ)
	$(MAKE) elfobject.o "CC = ppc-morphos-gcc" "CFLAGS = -mcpu=603e -mmultiple -I/GCCInclude -I/PPCInclude -I/Include" "FPM = -DFPM_PPC -DOPT_ACCURACY -DBUILD_POWERUP"
	$(MAKE) $(PROGNAME)_PUP "CC = m68k-amigaos-gcc" "CFLAGS = -m68040" "FPM = -DBUILD_POWERUP"
	m68k-amigaos-strip --strip-unneeded --remove-section .comment $(PROGNAME)
	sh tests/check_nofloat.sh $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME).pup
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ) $(PUP_OBJ) $(STARTUP_OBJ) elfobject.elf elfobject.o

mpega_68k_os4.o: mpega_68k_os4.S
	ppc-amigaos-gcc -Wa,-mregnames $< -o $@ -c -I/V/src/kickstart/exec_sg/include

$(PROGNAME)_OS4: mpega_68k_os4.o
	$(MAKE) $(PROGNAME) "CC = ppc-amigaos-gcc" "CFLAGS = -D__USE_INLINE__ -D__USE_BASETYPE__ -mcpu=604e -mmultiple" "FPM = -DFPM_PPC -DOPT_ACCURACY -DBUILD_OS4 -I/V/include" "ADD_OBJ = mpega_68k_os4.o -lmoto"
	ppc-amigaos-strip --strip-all --remove-section .comment $(PROGNAME)
	-$(MV) $(PROGNAME) $(PROGNAME).os4
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ)

clean:
	-$(RM) $(LIB_OBJ) $(WRAP_OBJ) $(MAD_OBJ) $(PUP_OBJ) $(STARTUP_OBJ) elfobject.elf elfobject.o mpega_68k_os4.o

# Regression harness (host build, needs no Amiga toolchain). See tests/README.md.
check:
	@bash tests/run.sh

check-asan:
	@bash tests/run.sh --asan

# Stack-swap guard: asserts the generated m68k code cleans up the
# WRAP_* arguments BEFORE swapping back to the caller's stack. Needs amiga-gcc
# on PATH; FAILS if it is absent, because a guard that passes without compiling
# anything is worse than no guard. MPEGA_ALLOW_SKIP=1 accepts a skip as a pass.
check-stackswap:
	@bash tests/check_stackswap.sh

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

.PHONY: all clean check check-asan check-update check-stackswap fixtures bench mad_build \
        $(PROGNAME)_020_all $(PROGNAME)_020_bugfix $(PROGNAME)_020_hifi \
        $(PROGNAME)_020_hifi_forced $(PROGNAME)_040 $(PROGNAME)_PPC $(PROGNAME)_OS4

