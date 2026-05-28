# StellaPD — Atari 2600 emulator for Playdate
# Adapted from the Playdate SDK common.mk template + custom C++ rule.

HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = StellaPD.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
$(error SDK path not found; set PLAYDATE_SDK_PATH)
endif

VPATH += src
VPATH += src/emucore

# C sources
SRC = src/main.c
SRC += src/dtcm.c

# C++ sources (emucore)
CXXSRC = \
	src/stella_glue.cpp \
	src/emucore/Cart.cpp \
	src/emucore/Cart2K.cpp \
	src/emucore/Cart4K.cpp \
	src/emucore/CartF8.cpp \
	src/emucore/CartF8SC.cpp \
	src/emucore/CartF6.cpp \
	src/emucore/CartF6SC.cpp \
	src/emucore/CartFASC.cpp \
	src/emucore/Console.cpp \
	src/emucore/Control.cpp \
	src/emucore/Device.cpp \
	src/emucore/Event.cpp \
	src/emucore/EventHandler.cpp \
	src/emucore/Joystick.cpp \
	src/emucore/Paddles.cpp \
	src/emucore/M6502.cpp \
	src/emucore/M6502Low.cpp \
	src/emucore/M6502Smol.cpp \
	src/emucore/M6532.cpp \
	src/emucore/NullDev.cpp \
	src/emucore/Random.cpp \
	src/emucore/Switches.cpp \
	src/emucore/System.cpp \
	src/emucore/TIA.cpp \
	src/emucore/TIASound.cpp

UINCDIR = src src/emucore

# PLAYDATE_STACK_SIZE (0x2700, matching CrankBoy) is the *entire* DTCM region
# safely usable below the live stack frame -- do not exceed it. Only small hot
# state can live here; our cart buffer (16-20KB) is far too big, so it stays in
# static RAM. The DTCM pool is currently dormant (see main.c).
UDEFS = -DSTELLA_PLAYDATE=1 -DDTCM_ALLOC -DPLAYDATE_STACK_SIZE=0x2700 \
	-Wno-parentheses -Wno-misleading-indentation -Wno-unused-value \
	-Wno-unknown-warning-option -Wno-switch -Wno-unused-but-set-variable \
	-Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-function \
	-Wno-write-strings -Wno-narrowing -Wno-multichar \
	-falign-loops=32 -fprefetch-loop-arrays

# Optional tiny smolnes-derived CPU core. Enable with: make SMOLNES_CPU=1
# (also turns on 6507 decimal/BCD mode, which Atari score counters need).
SMOL_DEFS =
ifeq ($(SMOLNES_CPU),1)
SMOL_DEFS = -DUSE_SMOLNES_CPU -DSMOLNES_BCD
UDEFS += $(SMOL_DEFS)
endif

# Experimental: disable TIA collision detection. Enable with: make NO_COLLISIONS=1
ifeq ($(NO_COLLISIONS),1)
SMOL_DEFS += -DDISABLE_COLLISIONS
UDEFS += -DDISABLE_COLLISIONS
endif

# Diagnostic: skip markUpdatedRows so the firmware does no display push.
# Used to measure how much of "hidden/OS:0" is the display flush. make SKIP_ROW_MARK=1
ifeq ($(SKIP_ROW_MARK),1)
UDEFS += -DSKIP_ROW_MARK
endif

# Scanline-at-a-time rendering (TIA dithers each line to the LCD as it
# completes, instead of filling a full frame buffer dithered afterward). On by
# default; disabled for the dual-CPU comparison build, which needs the full
# frame buffer + stella_frame_signature. SCANLINE_DEFS is also passed to the
# simulator and host_test rules below.
SCANLINE_DEFS = -DSTELLA_SCANLINE_DITHER
ifeq ($(COMPARE_CPU),1)
SCANLINE_DEFS =
endif
UDEFS += $(SCANLINE_DEFS)

# Dual-CPU comparison build: compile BOTH cores, select at runtime, expose the
# compare API. Used by the host harness (host_test ... --compare). Enable with:
# make host_test COMPARE_CPU=1
ifeq ($(COMPARE_CPU),1)
SMOL_DEFS += -DSTELLA_DUAL_CPU -DSMOLNES_BCD
UDEFS += -DSTELLA_DUAL_CPU -DSMOLNES_BCD
endif

UASRC =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# ---- C++ extension ---------------------------------------------------------
# common.mk only knows about .c files; add rules for .cpp.

CXX_NATIVE = g++

CXXOPT   = -Os -falign-functions=16 -fomit-frame-pointer
CXXBASE  = -mthumb -mcpu=$(MCU) $(FPU) $(CXXOPT) \
	   -gdwarf-2 -Wall -Wno-unused -Wno-unknown-pragmas -fverbose-asm \
	   -mword-relocations -fno-common -ffunction-sections -fdata-sections \
	   -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	   -fno-use-cxa-atexit -fno-non-call-exceptions \
	   $(DEFS)
CXX_DEVICE = $(GCC)$(TRGT)g++ -g3
CXXFLAGS = $(CXXBASE)

CXX_OBJS = $(addprefix $(OBJDIR)/, $(CXXSRC:.cpp=.o))

$(OBJDIR)/%.o : %.cpp | MKOBJDIR MKDEPDIR
	mkdir -p `dirname $@`
	$(CXX_DEVICE) -c $(CXXFLAGS) -I . $(INCDIR) $< -o $@

# Override the device elf rule to also link C++ objects
$(OBJDIR)/pdex.elf: $(OBJS) $(CXX_OBJS) $(LDSCRIPT)
	$(CXX_DEVICE) $(OBJS) $(CXX_OBJS) $(LDFLAGS) $(LIBS) -o $@

# Override the simulator rule: g++ compiles cpp + links
$(OBJDIR)/pdex.${DYLIB_EXT}: $(SRC) $(CXXSRC) | MKOBJDIR
	$(CXX_NATIVE) -g $(DYLIB_FLAGS) -lm -DTARGET_SIMULATOR=1 -DTARGET_EXTENSION=1 $(SMOL_DEFS) $(SCANLINE_DEFS) \
		-fno-exceptions -fno-rtti -fno-threadsafe-statics \
		-x c $(SRC) -x c++ $(CXXSRC) \
		$(INCDIR) -o $(OBJDIR)/pdex.${DYLIB_EXT}

# ---- Native headless test harness -----------------------------------------
# Compiles the same emucore + stella_glue into a standalone host binary that
# loads a ROM, runs N frames, and dumps PGM/PBM screenshots. No Playdate SDK
# involvement on this target.
HOST_TEST_SRC = host_test/main.c $(CXXSRC)
HOST_TEST_BIN = build/host_test

host_test: $(HOST_TEST_BIN)

$(HOST_TEST_BIN): $(HOST_TEST_SRC) | MKOBJDIR
	$(CXX_NATIVE) -g -O2 -DHOST_TEST=1 $(SMOL_DEFS) $(SCANLINE_DEFS) \
		-fno-exceptions -fno-rtti -fno-threadsafe-statics \
		-Wno-narrowing -Wno-write-strings -Wno-deprecated-declarations \
		-I . -I src -I src/emucore \
		-x c host_test/main.c -x c++ $(CXXSRC) \
		-lm -o $(HOST_TEST_BIN)
